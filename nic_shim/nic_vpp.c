// SPDX-License-Identifier: MIT

/* Functional UET NIC shim over the out-of-tree VPP termination plugin. */

#if ENABLE_VPP

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uet_vpp_client.h>

#include "uet_api.h"
#include "uet_api_private.h"
#include "uet_nic.h"

#define UET_NETWORK_TYPE_VPP "VPP"
#define UET_VPP_TX_BATCH_SIZE 64
#define UET_VPP_DEFAULT_MTU 1500
#define UET_VPP_CLOSE_TIMEOUT_NS (5ULL * 1000 * 1000 * 1000)

struct vpp_data {
	uet_vpp_client_t *client;
	uet_vpp_client_info_t info;
	uet_vpp_client_rx_t pending_rx;
	bool rx_pending;
	uint64_t next_request_id;
	uint64_t tx_inflight;
};

static int nic_vpp_monotonic_ns(uint64_t *now)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -errno;
	*now = (uint64_t)ts.tv_sec * 1000000000ULL +
	       (uint64_t)ts.tv_nsec;
	return 0;
}

static int nic_vpp_completion_error(int32_t status)
{
	switch (status) {
	case UET_VPP_CLIENT_STATUS_OK:
		return 0;
	case UET_VPP_CLIENT_STATUS_INVALID_PACKET:
		return -EINVAL;
	case UET_VPP_CLIENT_STATUS_TX_NOT_CONFIGURED:
		return -ENETDOWN;
	case UET_VPP_CLIENT_STATUS_SLOT_BUSY:
		return -EBUSY;
	case UET_VPP_CLIENT_STATUS_NO_BUFFERS:
		return -ENOBUFS;
	default:
		return -EPROTO;
	}
}

static int nic_vpp_poll_tx(struct vpp_data *vdata, int *completion_error)
{
	uet_vpp_client_completion_t completions[UET_VPP_TX_BATCH_SIZE];
	int error = 0;
	int i, rc;

	rc = uet_vpp_client_poll_batch(vdata->client, completions,
				       UET_VPP_TX_BATCH_SIZE);
	if (rc < 0)
		return rc;
	for (i = 0; i < rc; i++) {
		int status_error =
			nic_vpp_completion_error(completions[i].status);

		if (vdata->tx_inflight)
			vdata->tx_inflight--;
		else if (!error)
			error = -EPROTO;
		if (!error && status_error)
			error = status_error;
	}
	*completion_error = error;
	return rc;
}

static int nic_vpp_progress_tx(struct vpp_data *vdata)
{
	int completion_error;
	int rc;

	rc = nic_vpp_poll_tx(vdata, &completion_error);
	if (rc < 0)
		return rc;
	return completion_error ? completion_error : rc;
}

static int nic_vpp_release_pending_rx(struct vpp_data *vdata)
{
	int rc;

	if (!vdata->rx_pending)
		return 0;
	rc = uet_vpp_client_release_rx(vdata->client, &vdata->pending_rx);
	if (!rc)
		vdata->rx_pending = false;
	return rc;
}

static int nic_vpp_ip_length(void *iphdr, size_t available,
			     size_t *ip_length)
{
	uint8_t version;

	if (!iphdr || available < sizeof(struct iphdr))
		return -EINVAL;
	version = (*(uint8_t *)iphdr) >> 4;
	if (version == 4) {
		struct iphdr *ipv4 = iphdr;
		size_t header_length = ipv4->ihl << 2;

		if (header_length < sizeof(*ipv4))
			return -EINVAL;
		*ip_length = ntohs(ipv4->tot_len);
	} else if (version == 6) {
		struct ipv6hdr *ipv6 = iphdr;

		if (available < sizeof(*ipv6))
			return -EINVAL;
		*ip_length = sizeof(*ipv6) + ntohs(ipv6->payload_len);
	} else {
		return -EINVAL;
	}

	if (*ip_length > available)
		return -EMSGSIZE;
	return 0;
}

int nic_vpp_getinfo(struct uet_nic *nic, struct uet_nic_info *nic_info)
{
	nic_info->ifname = nic->ifname;
	nic_info->network_type = nic->network_type;
	nic_info->mac_addr_str = nic->mac_addr_str;
	nic_info->mtu = nic->mtu;
	nic_info->link_state = UET_NIC_LINK_STATE_UP;
	return 0;
}

int nic_vpp_get_nh(struct uet_nic *nic, const struct uet_fa *fa,
		   bool is_ipv6, uint8_t *mac)
{
	/* VPP owns FIB lookup, adjacency resolution and the Ethernet rewrite.
	 * The traditional provider still builds an Ethernet header, so give it
	 * a stable placeholder which the VPP shim discards on transmit.
	 */
	(void)nic;
	(void)fa;
	(void)is_ipv6;
	memset(mac, 0, ETH_ALEN);
	mac[0] = 0x02;
	mac[3] = 0x55;
	mac[4] = 0x45;
	mac[5] = 0x54;
	return 0;
}

int nic_vpp_tx_pkt(struct uet_nic *nic, void *pkt, void *iphdr,
		   size_t pkt_size)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	uet_vpp_client_tx_request_t request;
	uint8_t *packet = pkt;
	uint8_t *ip = iphdr;
	void *dma_data;
	size_t available, capacity, ip_length;
	int rc;

	if (!vdata || ip < packet || (size_t)(ip - packet) > pkt_size)
		return -EINVAL;
	available = pkt_size - (size_t)(ip - packet);
	rc = nic_vpp_ip_length(ip, available, &ip_length);
	if (rc)
		return rc;

	for (;;) {
		rc = uet_vpp_client_acquire_dma(vdata->client,
						&request.dma_slot,
						&dma_data, &capacity);
		if (rc != -EAGAIN)
			break;
		rc = nic_vpp_progress_tx(vdata);
		if (rc < 0)
			return rc;
	}
	if (rc)
		return rc;
	if (ip_length > capacity) {
		uet_vpp_client_release_dma(vdata->client, request.dma_slot);
		return -EMSGSIZE;
	}

	memcpy(dma_data, ip, ip_length);
	request.packet_length = ip_length;
	request.request_id = ++vdata->next_request_id;
	request.user_context = 0;
	for (;;) {
		rc = uet_vpp_client_submit_ip_batch(vdata->client, &request, 1);
		if (rc != -EAGAIN)
			break;
		rc = nic_vpp_progress_tx(vdata);
		if (rc < 0)
			break;
	}
	if (rc) {
		uet_vpp_client_release_dma(vdata->client, request.dma_slot);
		return rc;
	}
	vdata->tx_inflight++;
	return 0;
}

int nic_vpp_rx_poll(struct uet_nic *nic)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	int rc;

	if (!vdata)
		return -EINVAL;
	rc = nic_vpp_progress_tx(vdata);
	if (rc < 0)
		return rc;
	if (vdata->rx_pending)
		return 1;
	rc = uet_vpp_client_poll_rx(vdata->client, &vdata->pending_rx);
	if (rc < 0)
		return rc;
	vdata->rx_pending = rc == 1;
	return rc;
}

int nic_vpp_rx_pkt(struct uet_nic *nic, void *pkt, size_t pkt_buf_size,
		   size_t *rx_pkt_size)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	struct ethhdr *eth = pkt;
	size_t frame_length, offset = sizeof(*eth);
	uint16_t i;
	int rc;

	if (!vdata)
		return -EINVAL;
	if (!vdata->rx_pending) {
		rc = nic_vpp_rx_poll(nic);
		if (rc <= 0)
			return rc;
	}

	frame_length = sizeof(*eth) + vdata->pending_rx.packet_length;
	if (frame_length < nic->min_pkt_size)
		frame_length = nic->min_pkt_size;
	if (frame_length > pkt_buf_size) {
		rc = nic_vpp_release_pending_rx(vdata);
		if (rc)
			return rc;
		return -EMSGSIZE;
	}

	memset(pkt, 0, frame_length);
	memcpy(eth->h_dest, nic->mac_addr, ETH_ALEN);
	eth->h_source[0] = 0x02;
	eth->h_source[3] = 0x50;
	eth->h_source[4] = 0x45;
	eth->h_source[5] = 0x45;
	eth->h_proto = htons((vdata->pending_rx.flags &
			      UET_VPP_CLIENT_RX_F_IP6) ?
			     ETH_P_IPV6 : ETH_P_IP);
	for (i = 0; i < vdata->pending_rx.iov_count; i++) {
		memcpy((uint8_t *)pkt + offset,
		       vdata->pending_rx.iov[i].base,
		       vdata->pending_rx.iov[i].length);
		offset += vdata->pending_rx.iov[i].length;
	}

	rc = nic_vpp_release_pending_rx(vdata);
	if (rc)
		return rc;
	*rx_pkt_size = frame_length;
	return 1;
}

static int nic_vpp_discard_rx(struct vpp_data *vdata)
{
	int rc;

	if (!vdata->rx_pending) {
		rc = uet_vpp_client_poll_rx(vdata->client, &vdata->pending_rx);
		if (rc <= 0)
			return rc;
		vdata->rx_pending = true;
	}
	rc = nic_vpp_release_pending_rx(vdata);
	return rc ? rc : 1;
}

void nic_vpp_finalize(struct uet_nic *nic)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	uint64_t deadline = 0, now;
	int close_error = -EBUSY;
	int drain_error = 0;
	bool timed = true;

	if (!vdata)
		return;
	if (nic_vpp_monotonic_ns(&now)) {
		timed = false;
		drain_error = -EIO;
	} else {
		deadline = now + UET_VPP_CLOSE_TIMEOUT_NS;
	}
	while (timed) {
		int completion_error = 0;
		bool progressed = false;
		int rx_rc, tx_rc;

		tx_rc = nic_vpp_poll_tx(vdata, &completion_error);
		if (tx_rc < 0) {
			drain_error = tx_rc;
			break;
		}
		if (tx_rc)
			progressed = true;
		if (completion_error && !drain_error)
			drain_error = completion_error;

		rx_rc = nic_vpp_discard_rx(vdata);
		if (rx_rc > 0)
			progressed = true;
		else if (rx_rc < 0 && rx_rc != -EAGAIN) {
			if (!drain_error)
				drain_error = rx_rc;
			break;
		}

		if (!vdata->tx_inflight && !vdata->rx_pending && rx_rc == 0) {
			close_error = uet_vpp_client_close(vdata->client);
			if (!close_error) {
				vdata->client = NULL;
				break;
			}
			if (close_error != -EBUSY)
				break;
		}

		if (nic_vpp_monotonic_ns(&now)) {
			if (!drain_error)
				drain_error = -EIO;
			break;
		}
		if (now >= deadline)
			break;
		if (!progressed)
			usleep(50);
	}
	if (vdata->client) {
		close_error = uet_vpp_client_close(vdata->client);
		if (!close_error)
			vdata->client = NULL;
	}
	if (drain_error)
		UET_API_ERR("VPP channel drain failed: %d", drain_error);
	if (close_error)
		UET_API_ERR("uet_vpp_client_close failed: %d", close_error);
	free(vdata);
	nic->nic_priv_data = NULL;
}

static int nic_vpp_configure_mtu(struct uet_nic *nic,
				 struct vpp_data *vdata)
{
	const char *text = getenv(UET_VPP_MTU);
	unsigned long long mtu = UET_VPP_DEFAULT_MTU;
	char *end = NULL;

	if (text) {
		errno = 0;
		mtu = strtoull(text, &end, 10);
		if (errno || end == text || *end != '\0' || text[0] == '-')
			return -EINVAL;
	}
	if (mtu < nic->min_ip_pkt_size ||
	    mtu > vdata->info.dma_buffer_data_size)
		return -ERANGE;
	nic->mtu = (size_t)mtu;
	nic->max_pkt_size = nic->mtu + nic->l2_hdr_size;
	return 0;
}

static int nic_vpp_parse_addresses(struct uet_nic *nic)
{
	const char *ipv4_text = getenv(UET_VPP_IPV4_ADDR);
	const char *ipv6_text = getenv(UET_VPP_IPV6_ADDR);
	struct in_addr ipv4;

	if (ipv4_text) {
		if (inet_pton(AF_INET, ipv4_text, &ipv4) != 1)
			return -EINVAL;
		nic->ipv4_addr = ntohl(ipv4.s_addr);
		snprintf(nic->ipv4_addr_str, sizeof(nic->ipv4_addr_str),
			 "%s", ipv4_text);
		nic->has_ipv4 = true;
	}
	if (ipv6_text) {
		if (inet_pton(AF_INET6, ipv6_text, nic->ipv6_addr) != 1)
			return -EINVAL;
		snprintf(nic->ipv6_addr_str, sizeof(nic->ipv6_addr_str),
			 "%s", ipv6_text);
		nic->has_ipv6 = true;
	}
	return (nic->has_ipv4 || nic->has_ipv6) ? 0 : -EINVAL;
}

int nic_vpp_initialize(struct uet_nic *nic)
{
	const char *segment_name = getenv(UET_VPP_SEGMENT);
	const char *dma_socket = getenv(UET_VPP_DMA_SOCKET);
	const char *ifname = getenv(UET_IFNAME);
	struct vpp_data *vdata;
	int rc;

	if (!segment_name || !dma_socket) {
		UET_API_ERR("%s and %s are required for the VPP shim",
			    UET_VPP_SEGMENT, UET_VPP_DMA_SOCKET);
		return -EINVAL;
	}
	vdata = calloc(1, sizeof(*vdata));
	if (!vdata)
		return -ENOMEM;
	vdata->next_request_id = 1;
	rc = uet_vpp_client_open(&vdata->client, segment_name, &vdata->info);
	if (rc)
		goto err_free;
	rc = uet_vpp_client_map_dma(vdata->client, dma_socket);
	if (rc)
		goto err_close;

	nic->nic_priv_data = vdata;
	nic->min_pkt_size = UET_MIN_PKT_SIZE;
	nic->l2_hdr_size = sizeof(struct ethhdr);
	nic->min_ip_pkt_size = nic->min_pkt_size - nic->l2_hdr_size;
	rc = nic_vpp_configure_mtu(nic, vdata);
	if (rc) {
		UET_API_ERR("%s must be between %zu and %u (default %u)",
			    UET_VPP_MTU, nic->min_ip_pkt_size,
			    vdata->info.dma_buffer_data_size,
			    UET_VPP_DEFAULT_MTU);
		goto err_finalize;
	}
	nic->sock_fd = -1;
	snprintf(nic->network_type, sizeof(nic->network_type), "%s",
		 UET_NETWORK_TYPE_VPP);
	snprintf(nic->ifname, sizeof(nic->ifname), "%s",
		 ifname ? ifname : "vpp0");
	nic->mac_addr[0] = 0x02;
	nic->mac_addr[3] = 0x55;
	nic->mac_addr[4] = 0x45;
	nic->mac_addr[5] = 0x54;
	uet_mac_addr_to_str(nic->mac_addr_str, nic->mac_addr);
	rc = nic_vpp_parse_addresses(nic);
	if (rc) {
		UET_API_ERR("set %s and/or %s for the VPP shim",
			    UET_VPP_IPV4_ADDR, UET_VPP_IPV6_ADDR);
		goto err_finalize;
	}
	return 0;

err_finalize:
	nic_vpp_finalize(nic);
	return rc;
err_close:
	uet_vpp_client_close(vdata->client);
err_free:
	free(vdata);
	return rc;
}

#endif /* ENABLE_VPP */
