/* SPDX-License-Identifier: Apache-2.0 */
#include <arpa/inet.h>
#include <assert.h>
#include <infiniband/verbs.h>
#include <linux/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "uet_api.h"
#include "uet_nic.h"
#include "uet_pkt_hdr.h"
#include "uet_util.h"
#include "shared_endpoint_rdma.h"

/* Benchmark-only NIC backend. One raw Ethernet QP per peer, used exclusively
 * by that peer's owner. The QPs carry native UET frames. */
enum { DEPTH = 256, FRAME_SIZE = 2048, PEER_SLOTS = 10 };
static struct ibv_context *device;
static struct ibv_pd *pd;
static struct uet_nic *link_nic;
static uint8_t peer_mac[ETH_ALEN];
static struct lane {
  struct ibv_cq *rx_cq, *tx_cq;
  struct ibv_qp *qp;
  struct ibv_mr *mr;
  struct ibv_flow *flow;
  struct ibv_wc rx;
  bool have_rx;
  unsigned sent, reclaimed;
  uint8_t rx_buf[DEPTH][FRAME_SIZE], tx_buf[DEPTH][FRAME_SIZE];
} *lanes[PEER_SLOTS], *sole_rx;
static unsigned sole_rx_slot;
static struct lane *worker_tx[8];
static _Thread_local struct lane *selected_tx;

static void
require (bool ok, const char *operation)
{
  if (!ok)
    {
      fprintf (stderr, "%s failed (errno %d)\n", operation, errno);
      exit (1);
    }
}

static void
post_rx (struct lane *q, unsigned slot)
{
  struct ibv_sge sge = { .addr = (uintptr_t) q->rx_buf[slot],
                         .length = FRAME_SIZE, .lkey = q->mr->lkey };
  struct ibv_recv_wr wr = { .wr_id = slot, .sg_list = &sge, .num_sge = 1 }, *bad;
  require (!ibv_post_recv (q->qp, &wr, &bad), "post raw receive");
}

static struct lane *
create_lane (void)
{
  struct lane *q = calloc (1, sizeof (*q));
  require (q != NULL, "allocate packet queue");
  q->rx_cq = ibv_create_cq (device, DEPTH, NULL, NULL, 0);
  q->tx_cq = ibv_create_cq (device, DEPTH, NULL, NULL, 0);
  require (q->rx_cq && q->tx_cq, "create raw completion queues");
  q->mr = ibv_reg_mr (pd, q->rx_buf, sizeof (q->rx_buf) + sizeof (q->tx_buf),
                      IBV_ACCESS_LOCAL_WRITE);
  require (q->mr != NULL, "register packet buffers");
  struct ibv_qp_init_attr init = {
    .qp_type = IBV_QPT_RAW_PACKET, .send_cq = q->tx_cq, .recv_cq = q->rx_cq,
    .cap = { .max_send_wr = DEPTH, .max_recv_wr = DEPTH,
             .max_send_sge = 1, .max_recv_sge = 1 },
  };
  q->qp = ibv_create_qp (pd, &init);
  require (q->qp != NULL, "create raw Ethernet QP");
  struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .port_num = 1 };
  require (!ibv_modify_qp (q->qp, &a, IBV_QP_STATE | IBV_QP_PORT), "init raw QP");
  a.qp_state = IBV_QPS_RTR;
  require (!ibv_modify_qp (q->qp, &a, IBV_QP_STATE), "enable raw RX");
  a.qp_state = IBV_QPS_RTS;
  require (!ibv_modify_qp (q->qp, &a, IBV_QP_STATE), "enable raw TX");
  return q;
}

void
physical_open_worker_tx (unsigned worker)
{
  assert (worker < 8 && !worker_tx[worker]);
  worker_tx[worker] = create_lane ();
}

void
physical_select_worker_tx (unsigned worker)
{
  assert (worker < 8 && worker_tx[worker]);
  selected_tx = worker_tx[worker];
}

int
physical_open (uint32_t peer_ip)
{
  unsigned slot = peer_ip & 0xff;
  assert ((peer_ip >> 8) == 0xc00002 && slot < PEER_SLOTS && !lanes[slot]);
  struct lane *q = create_lane ();
  for (unsigned i = 0; i < DEPTH; i++)
    post_rx (q, i);

  struct {
    struct ibv_flow_attr attr;
    struct ibv_flow_spec_eth eth;
    struct ibv_flow_spec_ipv4_ext ip;
  } __attribute__((packed)) rule = {
    .attr = { .type = IBV_FLOW_ATTR_NORMAL, .size = sizeof (rule),
              .num_of_specs = 2, .port = 1 },
    .eth = { .type = IBV_FLOW_SPEC_ETH, .size = sizeof (rule.eth),
             .val = { .ether_type = htons (ETH_P_IP) }, .mask = { .ether_type = 0xffff } },
    .ip = { .type = IBV_FLOW_SPEC_IPV4_EXT, .size = sizeof (rule.ip),
            .val = { .src_ip = htonl (peer_ip), .dst_ip = htonl (link_nic->ipv4_addr),
                     .proto = UET_IPPROTO },
            .mask = { .src_ip = UINT32_MAX, .dst_ip = UINT32_MAX, .proto = 0xff } },
  };
  memcpy (rule.eth.val.dst_mac, link_nic->mac_addr, ETH_ALEN);
  memset (rule.eth.mask.dst_mac, 0xff, ETH_ALEN);
  q->flow = ibv_create_flow (q->qp, (struct ibv_flow_attr *) (void *) &rule);
  require (q->flow != NULL, "steer UET IPv4 pair to raw QP");
  lanes[slot] = q;
  sole_rx = q;
  sole_rx_slot = slot;
  return slot;
}

static bool
poll_rx (struct lane *q)
{
  if (!q->have_rx)
    {
      int n = ibv_poll_cq (q->rx_cq, 1, &q->rx);
      require (n >= 0 && (n == 0 || q->rx.status == IBV_WC_SUCCESS), "poll raw RX");
      q->have_rx = n != 0;
    }
  return q->have_rx;
}

ssize_t
physical_receive (int queue, void *frame, size_t len)
{
  struct lane *q = lanes[queue];
  if (!poll_rx (q))
    {
      errno = EAGAIN;
      return -1;
    }
  unsigned slot = q->rx.wr_id;
  size_t bytes = q->rx.byte_len;
  require (bytes <= len && slot < DEPTH, "check raw receive length");
  memcpy (frame, q->rx_buf[slot], bytes);
  post_rx (q, slot);
  q->have_rx = false;
  return bytes;
}

unsigned
physical_poll_frames (int queue, struct physical_rx frames[PHYSICAL_RX_BATCH])
{
  struct lane *q = lanes[queue];
  struct ibv_wc wc[PHYSICAL_RX_BATCH];
  int count = ibv_poll_cq (q->rx_cq, PHYSICAL_RX_BATCH, wc);
  require (count >= 0, "poll raw RX batch");
  for (int i = 0; i < count; i++)
    {
      require (wc[i].status == IBV_WC_SUCCESS && wc[i].wr_id < DEPTH && wc[i].byte_len <= FRAME_SIZE,
               "raw RX completion");
      frames[i] = (struct physical_rx) { q->rx_buf[wc[i].wr_id], wc[i].byte_len, wc[i].wr_id };
    }
  return count;
}

void
physical_repost_frames (int queue, const struct physical_rx *frames, unsigned count)
{
  if (!count)
    return;
  assert (count <= PHYSICAL_RX_BATCH);
  struct lane *q = lanes[queue];
  struct ibv_sge sge[PHYSICAL_RX_BATCH];
  struct ibv_recv_wr wr[PHYSICAL_RX_BATCH], *bad;
  for (unsigned i = 0; i < count; i++)
    {
      unsigned slot = frames[i].slot;
      sge[i] = (struct ibv_sge) { .addr = (uintptr_t) q->rx_buf[slot],
                                 .length = FRAME_SIZE, .lkey = q->mr->lkey };
      wr[i] = (struct ibv_recv_wr) { .wr_id = slot, .sg_list = &sge[i], .num_sge = 1,
                                     .next = i + 1 < count ? &wr[i + 1] : NULL };
    }
  require (!ibv_post_recv (q->qp, wr, &bad), "repost raw RX batch");
}

static int
nic_rx_poll (struct uet_nic *nic)
{
  (void) nic;
  return sole_rx && poll_rx (sole_rx);
}

static int
nic_rx (struct uet_nic *nic, void *frame, size_t max, size_t *len)
{
  (void) nic;
  ssize_t n = physical_receive (sole_rx_slot, frame, max);
  if (n < 0)
    return 0;
  *len = n;
  return 1;
}

static void
reclaim_tx (struct lane *q)
{
  struct ibv_wc wc[16];
  int n = ibv_poll_cq (q->tx_cq, 16, wc);
  require (n >= 0, "poll raw TX");
  for (int i = 0; i < n; i++)
    require (wc[i].status == IBV_WC_SUCCESS, "raw TX completion");
  q->reclaimed += n;
}

static int
nic_tx (struct uet_nic *nic, void *frame, void *ip_header, size_t len)
{
  (void) nic;
  (void) ip_header;
  const struct iphdr *ip = (void *) ((uint8_t *) frame + ETH_HLEN);
  uint32_t addr = ntohl (ip->daddr);
  unsigned slot = addr & 0xff;
  require ((addr >> 8) == 0xc00002 && slot < PEER_SLOTS && lanes[slot] && len <= FRAME_SIZE,
           "select raw TX queue");
  struct lane *q = selected_tx ? selected_tx : lanes[slot];
  if (q->sent - q->reclaimed == DEPTH)
    reclaim_tx (q);
  require (q->sent - q->reclaimed < DEPTH, "raw TX ring full");
  void *data = q->tx_buf[q->sent % DEPTH];
  memcpy (data, frame, len);
  struct ibv_sge sge = { .addr = (uintptr_t) data, .length = len, .lkey = q->mr->lkey };
  struct ibv_send_wr wr = { .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
                           .sg_list = &sge, .num_sge = 1 }, *bad;
  require (!ibv_post_send (q->qp, &wr, &bad), "post raw send");
  q->sent++;
  return 0;
}

static int
nic_get_nh (struct uet_nic *nic, const struct uet_fa *fa, bool ipv6, uint8_t *mac)
{
  (void) nic;
  if (ipv6 || (fa->v4 >> 8) != 0xc00002 || (fa->v4 & 0xff) >= PEER_SLOTS)
    return -EINVAL;
  memcpy (mac, peer_mac, ETH_ALEN);
  return 0;
}

static int
nic_getinfo (struct uet_nic *nic, struct uet_nic_info *info)
{
  *info = (struct uet_nic_info) { .ifname = nic->ifname, .network_type = nic->network_type,
    .mac_addr_str = nic->mac_addr_str, .mtu = nic->mtu, .link_state = UET_NIC_LINK_STATE_UP };
  return 0;
}

static void
close_lane (struct lane *q)
{
  if (!q)
    return;
  while (q->sent != q->reclaimed)
    reclaim_tx (q);
  require ((!q->flow || !ibv_destroy_flow (q->flow)) && !ibv_destroy_qp (q->qp) &&
           !ibv_destroy_cq (q->rx_cq) && !ibv_destroy_cq (q->tx_cq) && !ibv_dereg_mr (q->mr),
           "close raw Ethernet QP");
  free (q);
}

static void
nic_finalize (struct uet_nic *nic)
{
  (void) nic;
  for (unsigned i = 0; i < PEER_SLOTS; i++)
    close_lane (lanes[i]);
  for (unsigned i = 0; i < 8; i++)
    close_lane (worker_tx[i]);
  require (!ibv_dealloc_pd (pd) && !ibv_close_device (device), "close RDMA device");
}

/* Interpose the NIC initialization entry point when linking the fixture. */
int
uet_nic_initialize (struct uet_nic *nic)
{
  const char *name = getenv ("SE_RDMA_DEVICE"), *ifname = getenv ("UET_IFNAME");
  const char *address = getenv ("SE_IPV4"), *remote_mac = getenv ("SE_PEER_MAC");
  require (name && ifname && strlen (ifname) < IFNAMSIZ && address && remote_mac, "RDMA setup");
  struct ibv_device **devices = ibv_get_device_list (NULL);
  require (devices != NULL, "list RDMA devices");
  for (unsigned i = 0; devices[i]; i++)
    if (!strcmp (ibv_get_device_name (devices[i]), name))
      {
        device = ibv_open_device (devices[i]);
        break;
      }
  ibv_free_device_list (devices);
  require (device != NULL, "open RDMA device");
  pd = ibv_alloc_pd (device);
  require (pd != NULL, "allocate protection domain");

  struct ifreq ifr;
  memset (&ifr, 0, sizeof (ifr));
  strcpy (ifr.ifr_name, ifname);
  int fd = socket (AF_INET, SOCK_DGRAM, 0);
  require (fd >= 0 && !ioctl (fd, SIOCGIFHWADDR, &ifr), "read local MAC");
  memcpy (nic->mac_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
  require (!ioctl (fd, SIOCGIFMTU, &ifr) && ifr.ifr_mtu == 1500, "read 1500-byte MTU");
  close (fd);
  require (sscanf (remote_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &peer_mac[0], &peer_mac[1],
                   &peer_mac[2], &peer_mac[3], &peer_mac[4], &peer_mac[5]) == ETH_ALEN, "peer MAC");
  uint32_t ip;
  require (inet_pton (AF_INET, address, &ip) == 1, "local IPv4");
  nic->has_ipv4 = true;
  nic->ipv4_addr = ntohl (ip);
  strcpy (nic->ipv4_addr_str, address);
  strcpy (nic->ifname, ifname);
  strcpy (nic->network_type, "Ethernet(raw RDMA)");
  uet_mac_addr_to_str (nic->mac_addr_str, nic->mac_addr);
  nic->mtu = 1500;
  nic->l2_hdr_size = ETH_HLEN;
  nic->max_pkt_size = nic->mtu + nic->l2_hdr_size;
  nic->min_pkt_size = UET_MIN_PKT_SIZE;
  nic->min_ip_pkt_size = nic->min_pkt_size - nic->l2_hdr_size;
  nic->nic_getinfo = nic_getinfo;
  nic->nic_get_nh = nic_get_nh;
  nic->nic_tx_pkt = nic_tx;
  nic->nic_rx_pkt = nic_rx;
  nic->nic_rx_poll = nic_rx_poll;
  nic->nic_finalize = nic_finalize;
  link_nic = nic;
  return 0;
}
