/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uet_api.h>
#include <uet_pkt_hdr.h>
#ifdef SE_USE_RDMA
#include <uet_api_private.h>
#include "shared_endpoint_rdma.h"
#endif
#include <unistd.h>

enum { FLOWS = 8, JOB = 1, WORDS = 512, MAX_WINDOW = 32, FRAME = 2048,
       RX_DEPTH = 2 * FLOWS * MAX_WINDOW, RING = 1024, RX_PID = 1, RX_RI = 101 };
static const uint32_t tx_ip = 0xc0000202;
static struct fi_cq_attr cq_attr = { .size = RX_DEPTH, .format = FI_CQ_FORMAT_DATA };
static uet_ep_handle_t parent;
static unsigned iterations, window, workers;
static atomic_bool stopping;
static atomic_uint completed_workers;
static int ingress;
static __typeof__ (&uet_ep_context_open) open_context;
static __typeof__ (&uet_ep_context_route) route_frame;
static __typeof__ (&uet_ep_context_rx) receive_frame;

static uint64_t
now (clockid_t clock)
{
  struct timespec ts;
  assert (!clock_gettime (clock, &ts));
  return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void
check (int rc, const char *operation)
{
  if (rc)
    {
      fprintf (stderr, "%s failed: %d\n", operation, rc);
      exit (1);
    }
}

static void
pin (unsigned cpu)
{
  cpu_set_t set;
  CPU_ZERO (&set);
  CPU_SET (cpu, &set);
  check (pthread_setaffinity_np (pthread_self (), sizeof set, &set), "pin CPU");
}

static unsigned
completions (uet_cq_handle_t cq, struct fi_cq_data_entry *entries)
{
  ssize_t count = uet_cq_read (cq, entries, 16);
  if (count == -FI_EAGAIN)
    return 0;
  if (count == -FI_EAVAIL)
    {
      struct fi_cq_err_entry err;
      if (uet_cq_readerr (cq, &err) == 1)
        check (-err.err, "completion error");
    }
  assert (count >= 0);
  return count;
}

static void
post_rx (uet_ep_handle_t ep, uint64_t *data)
{
  struct iovec iov = { .iov_base = data, .iov_len = WORDS * sizeof (*data) };
  check (uet_recvv (ep, JOB, &iov, 1, NULL, NULL, data), "post RX");
}

static void
wait_command (const char *expected)
{
  char command[16];
  assert (scanf ("%15s", command) == 1 && !strcmp (command, expected));
}

static int
open_ingress (uint32_t peer)
{
#ifdef SE_USE_RDMA
  return physical_open (peer);
#else
  (void) peer;
  struct sockaddr_ll addr = { .sll_family = AF_PACKET,
    .sll_protocol = htons (ETH_P_IP), .sll_ifindex = if_nametoindex (getenv ("UET_IFNAME")) };
  int fd = socket (AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, 0);
  assert (fd >= 0 && addr.sll_ifindex);
  check (bind (fd, (struct sockaddr *) &addr, sizeof addr), "bind test RX");
  return fd;
#endif
}

#ifndef SE_BATCH_RDMA
static ssize_t
read_frame (void *frame)
{
#ifdef SE_USE_RDMA
  return physical_receive (ingress, frame, FRAME);
#else
  return recv (ingress, frame, FRAME, 0);
#endif
}
#endif

struct frame { size_t len; uint8_t bytes[FRAME]; };
static struct worker {
  uet_ep_handle_t ep;
  uet_cq_handle_t cq;
  unsigned index, cpu;
  unsigned received[FLOWS];
  uint64_t done_ns, routed;
  atomic_bool done;
  alignas(64) atomic_uint head;
  alignas(64) atomic_uint tail;
  struct frame queue[RING];
  uint64_t buffers[RX_DEPTH][WORDS];
} rx_workers[FLOWS];

static uint64_t dispatch_frames, dispatch_wall_ns, dispatch_cpu_ns, queue_waits;
static unsigned pdc_count, established_count;
static int pdc_owner[UINT16_MAX + 1];
static uint16_t destination_pdc[UINT16_MAX + 1];

static void
load_context_ops (void)
{
  open_context = dlsym (RTLD_DEFAULT, "uet_ep_context_open");
  route_frame = dlsym (RTLD_DEFAULT, "uet_ep_context_route");
  receive_frame = dlsym (RTLD_DEFAULT, "uet_ep_context_rx");
  assert (open_context && route_frame && receive_frame);
}

static void *
dispatch (void *arg)
{
  pin (*(unsigned *) arg);
  unsigned heads[FLOWS] = {};
  uint64_t cpu_start = 0, wall_start = 0;
  while (!atomic_load_explicit (&stopping, memory_order_relaxed))
    {
      unsigned touched = 0;
#ifdef SE_BATCH_RDMA
      struct physical_rx frames[PHYSICAL_RX_BATCH];
      unsigned count = physical_poll_frames (ingress, frames);
#else
      unsigned count = 16;
#endif
      for (unsigned batch = 0; batch < count; batch++)
        {
#ifdef SE_BATCH_RDMA
          void *frame = frames[batch].data;
          size_t len = frames[batch].len;
#else
          uint8_t frame[FRAME];
          ssize_t len = read_frame (frame);
          if (len < 0 && errno == EAGAIN)
            break;
          assert (len > 0);
#endif
          uet_ep_handle_t context;
          int rc = route_frame (parent, frame, len, &context);
          if (rc == -EINVAL || rc == -ENOENT)
            continue;
          check (rc, "classify frame");
          unsigned owner = 0;
          while (owner < workers && rx_workers[owner].ep != context)
            owner++;
          assert (owner < workers);
          const struct uet_pds_req *hdr = (const void *) (frame + ETH_HLEN + 20 + 4);
          uint16_t prologue = ntohs (hdr->prlg.type_next_flags);
          if ((prologue >> UET_PDS_TYPE_SHIFT) == UET_PDS_TYPE_ROD_REQ)
            {
              uint16_t id = ntohs (hdr->spdcid);
              assert (id);
              if (pdc_owner[id] == -1)
                {
                  assert (prologue & UET_PDS_REQ_FLAGS_SYN);
                  pdc_owner[id] = owner;
                  pdc_count++;
                }
              assert (pdc_owner[id] == (int) owner);
              if (!(prologue & UET_PDS_REQ_FLAGS_SYN))
                {
                  uint16_t dpdcid = ntohs (hdr->dpdcid);
                  assert (dpdcid / 64 == owner && dpdcid % 64);
                  if (!destination_pdc[id])
                    established_count++;
                  else
                    assert (destination_pdc[id] == dpdcid);
                  destination_pdc[id] = dpdcid;
                }
            }
          struct worker *w = &rx_workers[owner];
          if (!wall_start)
            {
              wall_start = now (CLOCK_MONOTONIC);
              cpu_start = now (CLOCK_THREAD_CPUTIME_ID);
            }
          unsigned head = heads[owner];
          if (head - atomic_load_explicit (&w->tail, memory_order_acquire) == RING)
            {
              queue_waits++;
              atomic_store_explicit (&w->head, head, memory_order_release);
              do
                {
                  if (atomic_load_explicit (&stopping, memory_order_relaxed))
                    return NULL;
                }
              while (head - atomic_load_explicit (&w->tail, memory_order_acquire) == RING);
            }
          struct frame *packet = &w->queue[head % RING];
          packet->len = len;
          memcpy (packet->bytes, frame, len);
          heads[owner]++;
          touched |= 1U << owner;
          dispatch_frames++;
        }
#ifdef SE_BATCH_RDMA
      physical_repost_frames (ingress, frames, count);
#endif
      for (unsigned i = 0; i < workers; i++)
        if (touched & (1U << i))
          atomic_store_explicit (&rx_workers[i].head, heads[i], memory_order_release);
      if (!dispatch_wall_ns && atomic_load_explicit (&completed_workers, memory_order_acquire) == workers)
        {
          dispatch_cpu_ns = now (CLOCK_THREAD_CPUTIME_ID) - cpu_start;
          dispatch_wall_ns = now (CLOCK_MONOTONIC) - wall_start;
        }
    }
  return NULL;
}

static unsigned
progress_rx (struct worker *w, unsigned tail)
{
  unsigned head = atomic_load_explicit (&w->head, memory_order_acquire);
  while (tail != head)
    {
      struct frame *p = &w->queue[tail % RING];
      int rc = receive_frame (w->ep, p->bytes, p->len);
      if (rc != -EEXIST && rc != -EINVAL)
        check (rc, "context RX");
      w->routed++;
      tail++;
    }
  atomic_store_explicit (&w->tail, tail, memory_order_release);
  return tail;
}

static void *
receive (void *arg)
{
  struct worker *w = arg;
  pin (w->cpu);
#ifdef SE_USE_RDMA
  physical_select_worker_tx (w->index);
#endif
  unsigned tail = 0, total = 0, expected = iterations * (FLOWS / workers);
  while (!atomic_load_explicit (&stopping, memory_order_relaxed))
    {
      tail = progress_rx (w, tail);
      struct fi_cq_data_entry entries[16];
      unsigned count = completions (w->cq, entries);
      for (unsigned i = 0; i < count; i++)
        {
          uint64_t *data = entries[i].op_context;
          unsigned flow = data[0] >> 48;
          assert (entries[i].len == sizeof (w->buffers[0]) && flow < FLOWS);
          assert (w->received[flow] < iterations);
          uint64_t pattern = ((uint64_t) flow << 48) | ((uint64_t) w->received[flow]++ << 16);
          for (unsigned j = 0; j < WORDS; j++)
            assert (data[j] == (pattern | j));
          post_rx (w->ep, data);
          if (++total == expected)
            {
              w->done_ns = now (CLOCK_MONOTONIC);
              atomic_store_explicit (&w->done, true, memory_order_release);
              atomic_fetch_add_explicit (&completed_workers, 1, memory_order_release);
            }
          assert (total <= expected);
        }
    }
  return NULL;
}

static void
run_receiver (uet_domain_handle_t domain, struct fi_info *info, unsigned first_cpu, unsigned worker_cpus)
{
  load_context_ops ();
  check (uet_endpoint (domain, info, NULL, NULL, &parent, RX_PID, RX_RI, 1001,
                       JOB, false, false, 0), "create shared endpoint");
  ingress = open_ingress (tx_ip);
  memset (pdc_owner, -1, sizeof pdc_owner);
  pthread_t receiver[FLOWS], dispatcher;
  for (unsigned i = 0; i < workers; i++)
    {
      struct worker *w = &rx_workers[i];
      w->index = i;
      w->cpu = first_cpu + i % worker_cpus + 1;
      check (open_context (parent, tx_ip, &w->ep), "open worker context");
      check (uet_ep_bind_cq (w->ep, &cq_attr, NULL, FI_RECV, NULL, &w->cq), "bind RX CQ");
    }
  for (unsigned i = 0; i < workers; i++)
    {
      struct worker *w = &rx_workers[i];
      check (uet_ep_enable (w->ep), "enable worker context");
      for (unsigned j = 0; j < RX_DEPTH; j++)
        post_rx (w->ep, w->buffers[j]);
#ifdef SE_USE_RDMA
      physical_open_worker_tx (i);
#endif
    }
  for (unsigned i = 0; i < workers; i++)
    check (pthread_create (&receiver[i], NULL, receive, &rx_workers[i]), "start RX worker");
  check (pthread_create (&dispatcher, NULL, dispatch, &first_cpu), "start dispatcher");
  puts ("SE_READY");
  wait_command ("stop");
  atomic_store_explicit (&stopping, true, memory_order_relaxed);
  check (pthread_join (dispatcher, NULL), "join dispatcher");
  for (unsigned i = 0; i < workers; i++)
    check (pthread_join (receiver[i], NULL), "join RX worker");
  assert (pdc_count == FLOWS && established_count == FLOWS);
  printf ("SE_RX {\"pdc_count\":%u,\"established_count\":%u,\"dispatch_frames\":%llu,"
          "\"dispatch_wall_ns\":%llu,\"dispatch_cpu_ns\":%llu,\"queue_waits\":%llu,\"owners\":[",
          pdc_count, established_count, (unsigned long long) dispatch_frames,
          (unsigned long long) dispatch_wall_ns,
          (unsigned long long) dispatch_cpu_ns, (unsigned long long) queue_waits);
  for (unsigned i = 0; i < workers; i++)
    {
      struct worker *w = &rx_workers[i];
      assert (atomic_load_explicit (&w->done, memory_order_acquire));
      printf ("%s{\"cpu\":%u,\"frames\":%llu,\"done_ns\":%llu,\"messages\":[",
              i ? "," : "", w->cpu, (unsigned long long) w->routed,
              (unsigned long long) w->done_ns);
      for (unsigned flow = 0; flow < FLOWS; flow++)
        printf ("%s%u", flow ? "," : "", w->received[flow]);
      printf ("]}");
      check (uet_ep_close (w->ep), "close worker context");
    }
  puts ("]}");
  check (uet_ep_close (parent), "close shared endpoint");
#ifndef SE_USE_RDMA
  close (ingress);
#endif
}

struct sender {
  uet_ep_handle_t ep;
  uet_cq_handle_t cq;
  uet_addr_handle_t address;
  unsigned sent, completed;
  uint64_t cpu_ns;
  uint64_t buffers[MAX_WINDOW][WORDS];
};

static struct uet_addr destination = {
  .flags = UET_ADDR_FA_V | UET_ADDR_FEP_CAP_V | UET_ADDR_PID_ON_FEP_V |
           UET_ADDR_INDEX_V | UET_ADDR_INITIATOR_V,
  .fa.v4 = 0xc0000201, .fep_cap = UET_FEP_CAP_AI_FULL | UET_FEP_CAP_HPC,
  .pid_on_fep = RX_PID, .start_index = RX_RI, .num_indices = 1, .initiator_id = 1001,
};

static unsigned
send_step (struct sender *f, unsigned flow_id)
{
  struct fi_cq_data_entry entries[16];
  unsigned n = completions (f->cq, entries);
  for (unsigned j = 0; j < n; j++)
    {
      assert (f->completed < f->sent && entries[j].op_context == f->buffers[f->completed % window]);
      f->completed++;
    }
  while (f->sent < iterations && f->sent - f->completed < window)
    {
      uint64_t *data = f->buffers[f->sent % window];
      uint64_t pattern = ((uint64_t) flow_id << 48) | ((uint64_t) f->sent << 16);
      for (unsigned j = 0; j < WORDS; j++)
        data[j] = pattern | j;
      struct iovec iov = { .iov_base = data, .iov_len = sizeof (f->buffers[0]) };
      check (uet_sendv (f->ep, JOB, &iov, 1, NULL, f->address, data, RX_RI), "post SEND");
      f->sent++;
    }
  return n;
}

static void
run_sender (uet_domain_handle_t domain, struct fi_info *info, unsigned cpu)
{
  struct sender *flow = calloc (FLOWS, sizeof (*flow));
  assert (flow);
  pin (cpu);
  uet_addr_handle_t address;
  check (uet_av_insert (domain, &destination, &address), "insert destination");
#ifdef SE_USE_RDMA
  physical_open (destination.fa.v4);
#endif
  for (unsigned i = 0; i < FLOWS; i++)
    {
      check (uet_endpoint (domain, info, NULL, NULL, &flow[i].ep, 10 + i, 200 + i,
                           2000 + i, JOB, false, false, 0), "create source endpoint");
      flow[i].address = address;
      check (uet_ep_bind_cq (flow[i].ep, &cq_attr, NULL, FI_SEND, NULL, &flow[i].cq), "bind TX CQ");
      check (uet_ep_enable (flow[i].ep), "enable source endpoint");
    }
  puts ("SE_READY");
  wait_command ("go");
  uint64_t begin = now (CLOCK_MONOTONIC), cpu_start = now (CLOCK_THREAD_CPUTIME_ID);
  unsigned done = 0;
  while (done < FLOWS * iterations)
    {
      for (unsigned i = 0; i < FLOWS; i++)
        done += send_step (&flow[i], i);
      assert (now (CLOCK_MONOTONIC) - begin < 30000000000ULL);
    }
  printf ("SE_TX {\"messages\":%u,\"start_ns\":%llu,\"elapsed_ns\":%llu,\"cpu_ns\":%llu}\n",
          done, (unsigned long long) begin, (unsigned long long) (now (CLOCK_MONOTONIC) - begin),
          (unsigned long long) (now (CLOCK_THREAD_CPUTIME_ID) - cpu_start));
  for (unsigned i = 0; i < FLOWS; i++)
    check (uet_ep_close (flow[i].ep), "close source endpoint");
  check (uet_av_remove (address), "remove destination");
  free (flow);
}

#ifdef SE_USE_RDMA
static struct sender tx_flows[FLOWS];
static atomic_bool start_tx;

static void *
send_parallel (void *arg)
{
  struct worker *w = arg;
  struct sender *f = &tx_flows[w->index];
  pin (w->cpu);
  physical_select_worker_tx (w->index);
  while (!atomic_load_explicit (&start_tx, memory_order_acquire))
    ;
  uint64_t start = now (CLOCK_THREAD_CPUTIME_ID);
  uint64_t deadline = now (CLOCK_MONOTONIC) + 30000000000ULL;
  unsigned tail = 0;
  do
    {
      tail = progress_rx (w, tail);
      send_step (f, w->index);
      assert (now (CLOCK_MONOTONIC) < deadline);
    }
  while (f->completed < iterations);
  f->cpu_ns = now (CLOCK_THREAD_CPUTIME_ID) - start;
  atomic_fetch_add_explicit (&completed_workers, 1, memory_order_release);
  return NULL;
}

static void
run_parallel_sender (uet_domain_handle_t domain, struct fi_info *info, unsigned first_cpu)
{
  load_context_ops ();
  assert (workers == FLOWS);
  check (uet_endpoint (domain, info, NULL, NULL, &parent, 10, 200, 2000,
                       JOB, false, false, 0), "create sender parent");
  ingress = physical_open (destination.fa.v4);
  pthread_t sender[FLOWS], dispatcher;
  for (unsigned i = 0; i < FLOWS; i++)
    {
      struct sender *f = &tx_flows[i];
      struct worker *w = &rx_workers[i];
      check (open_context (parent, destination.fa.v4, &f->ep), "open sender context");
      /* The load generator represents distinct originating ROD groups on
       * one FEP. Set their source identities while all contexts are stopped. */
      struct uet_ep *ep = f->ep;
      ep->uet_addr.pid_on_fep = 10 + i;
      ep->uet_addr.initiator_id = 2000 + i;
      check (uet_av_insert (domain, &destination, &f->address), "insert context destination");
      check (uet_ep_bind_cq (f->ep, &cq_attr, NULL, FI_SEND, NULL, &f->cq), "bind context TX");
      w->ep = f->ep;
      w->index = i;
      w->cpu = first_cpu + i + 1;
      physical_open_worker_tx (i);
    }
  for (unsigned i = 0; i < FLOWS; i++)
    {
      check (uet_ep_enable (tx_flows[i].ep), "enable sender context");
      check (pthread_create (&sender[i], NULL, send_parallel, &rx_workers[i]), "start sender worker");
    }
  check (pthread_create (&dispatcher, NULL, dispatch, &first_cpu), "start ACK dispatcher");
  puts ("SE_READY");
  wait_command ("go");
  uint64_t begin = now (CLOCK_MONOTONIC), cpu_ns = 0;
  atomic_store_explicit (&start_tx, true, memory_order_release);
  for (unsigned i = 0; i < FLOWS; i++)
    {
      check (pthread_join (sender[i], NULL), "join sender worker");
      assert (tx_flows[i].completed == iterations);
      cpu_ns += tx_flows[i].cpu_ns;
    }
  uint64_t elapsed = now (CLOCK_MONOTONIC) - begin;
  atomic_store_explicit (&stopping, true, memory_order_relaxed);
  check (pthread_join (dispatcher, NULL), "join ACK dispatcher");
  printf ("SE_TX {\"messages\":%u,\"start_ns\":%llu,\"elapsed_ns\":%llu,\"cpu_ns\":%llu,"
          "\"ack_dispatch_wall_ns\":%llu,\"ack_dispatch_cpu_ns\":%llu,\"ack_queue_waits\":%llu}\n",
          FLOWS * iterations, (unsigned long long) begin, (unsigned long long) elapsed,
          (unsigned long long) cpu_ns, (unsigned long long) dispatch_wall_ns,
          (unsigned long long) dispatch_cpu_ns, (unsigned long long) queue_waits);
  for (unsigned i = 0; i < FLOWS; i++)
    {
      check (uet_ep_close (tx_flows[i].ep), "close sender context");
      check (uet_av_remove (tx_flows[i].address), "remove context destination");
    }
  check (uet_ep_close (parent), "close sender parent");
}
#endif

int
main (int argc, char **argv)
{
  assert (argc == 7);
  bool sender = !strcmp (argv[1], "tx");
  iterations = atoi (argv[2]);
  window = atoi (argv[3]);
  workers = atoi (argv[4]);
  unsigned cpu = atoi (argv[5]);
  unsigned worker_cpus = atoi (argv[6]);
  assert (iterations > window && window && window <= MAX_WINDOW && workers && FLOWS % workers == 0);
  setvbuf (stdout, NULL, _IOLBF, 0);
  uet_handle_t instance;
  uet_domain_handle_t domain;
  struct fi_info *info;
  check (uet_initialize (&instance), "initialize libuet");
  check (uet_getinfo (instance, NULL, NULL, &info), "get info");
  info->rx_attr->size = RX_DEPTH;
  info->tx_attr->size = MAX_WINDOW * 2;
  info->tx_attr->op_flags = info->rx_attr->op_flags = FI_COMPLETION;
  info->tx_attr->msg_order = FI_ORDER_SAS;
  check (uet_domain (instance, NULL, info, NULL, NULL, NULL, NULL, &domain), "create domain");
#ifdef SE_USE_RDMA
  if (!strcmp (argv[1], "parallel-tx"))
    run_parallel_sender (domain, info, cpu);
  else
#endif
  if (sender)
    run_sender (domain, info, cpu);
  else
    run_receiver (domain, info, cpu, worker_cpus);
  check (uet_domain_close (domain), "close domain");
  uet_verbs_fi_freeinfo (info);
  check (uet_finalize (instance), "finalize libuet");
  puts ("SE_CLOSED");
  return 0;
}
