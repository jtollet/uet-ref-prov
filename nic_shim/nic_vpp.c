// SPDX-License-Identifier: MIT

/* Functional UET NIC shim over the out-of-tree VPP termination plugin. */

#if ENABLE_VPP

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <pthread.h>
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
#define UET_VPP_MAX_CHANNELS 256
#define UET_VPP_CHANNEL_ALIGNMENT 64
#define UET_VPP_CLOSE_TIMEOUT_NS (5ULL * 1000 * 1000 * 1000)

struct __attribute__((aligned(UET_VPP_CHANNEL_ALIGNMENT))) vpp_channel {
	uet_vpp_client_t *client;
	uint32_t channel_index;
	uet_vpp_client_rx_t pending_rx;
	bool rx_pending;
	uint64_t next_request_id;
	uint64_t tx_inflight;
	pthread_mutex_t lock;
	bool lock_initialized;
};

struct vpp_data {
	uet_vpp_client_t *client;
	struct vpp_channel *channels;
	size_t channel_count;
	size_t rx_cursor;
	size_t pending_channel;
	pthread_mutex_t rx_lock;
	bool rx_lock_initialized;
	bool dma_mapped;
	uet_vpp_client_info_t info;
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

static int nic_vpp_poll_tx(struct vpp_channel *channel,
			   int *completion_error)
{
	uet_vpp_client_completion_t completions[UET_VPP_TX_BATCH_SIZE];
	int error = 0;
	int i, rc;

	rc = uet_vpp_client_poll_batch(channel->client,
				       channel->channel_index, completions,
				       UET_VPP_TX_BATCH_SIZE);
	if (rc < 0)
		return rc;
	for (i = 0; i < rc; i++) {
		int status_error =
			nic_vpp_completion_error(completions[i].status);

		if (channel->tx_inflight)
			channel->tx_inflight--;
		else if (!error)
			error = -EPROTO;
		if (!error && status_error)
			error = status_error;
	}
	*completion_error = error;
	return rc;
}

static int nic_vpp_progress_tx(struct vpp_channel *channel)
{
	int completion_error;
	int rc;

	rc = nic_vpp_poll_tx(channel, &completion_error);
	if (rc < 0)
		return rc;
	return completion_error ? completion_error : rc;
}

static int nic_vpp_release_pending_rx(struct vpp_data *vdata)
{
	struct vpp_channel *channel;
	int rc;

	if (vdata->pending_channel >= vdata->channel_count)
		return 0;
	channel = &vdata->channels[vdata->pending_channel];
	rc = uet_vpp_client_release_rx(channel->client,
				       channel->channel_index,
				       &channel->pending_rx);
	if (!rc) {
		channel->rx_pending = false;
		vdata->pending_channel = SIZE_MAX;
	}
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

static uint32_t nic_vpp_hash_mix(uint32_t hash, uint32_t value)
{
	hash ^= value + 0x9e3779b9U + (hash << 6) + (hash >> 2);
	return hash;
}

static uint32_t nic_vpp_hash_finalize(uint32_t hash)
{
	hash ^= hash >> 16;
	hash *= 0x7feb352dU;
	hash ^= hash >> 15;
	hash *= 0x846ca68bU;
	return hash ^ (hash >> 16);
}

/*
 * Select a stable worker channel from the fields normally used as network
 * entropy.  Native UET places EV at the start of its entropy header; UDP uses
 * the source port.  Keeping one EV on one channel avoids introducing packet
 * reordering while allowing future per-endpoint/per-message EV selection to
 * spread traffic across VPP workers.
 */
static size_t nic_vpp_select_channel(const struct vpp_data *vdata,
				     const void *iphdr, size_t ip_length)
{
	const uint8_t *ip = iphdr;
	const uint8_t *l4;
	uint32_t hash = 0x811c9dc5U;
	uint16_t entropy;
	uint8_t protocol;
	size_t l4_length;

	if (vdata->channel_count == 1)
		return 0;
	if ((ip[0] >> 4) == 4) {
		const struct iphdr *ipv4 = iphdr;
		size_t header_length = ipv4->ihl << 2;

		if (header_length > ip_length)
			return 0;
		hash = nic_vpp_hash_mix(hash, ntohl(ipv4->saddr));
		hash = nic_vpp_hash_mix(hash, ntohl(ipv4->daddr));
		protocol = ipv4->protocol;
		l4 = ip + header_length;
		l4_length = ip_length - header_length;
	} else {
		const struct ipv6hdr *ipv6 = iphdr;
		uint32_t word;

		if (ip_length < sizeof(*ipv6))
			return 0;
		for (size_t i = 0; i < sizeof(ipv6->saddr); i += sizeof(word)) {
			memcpy(&word, (const uint8_t *)&ipv6->saddr + i,
			       sizeof(word));
			hash = nic_vpp_hash_mix(hash, ntohl(word));
			memcpy(&word, (const uint8_t *)&ipv6->daddr + i,
			       sizeof(word));
			hash = nic_vpp_hash_mix(hash, ntohl(word));
		}
		protocol = ipv6->nexthdr;
		l4 = ip + sizeof(*ipv6);
		l4_length = ip_length - sizeof(*ipv6);
	}

	hash = nic_vpp_hash_mix(hash, protocol);
	if (l4_length >= sizeof(entropy)) {
		memcpy(&entropy, l4, sizeof(entropy));
		hash = nic_vpp_hash_mix(hash, ntohs(entropy));
	}
	return nic_vpp_hash_finalize(hash) % vdata->channel_count;
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
	struct vpp_channel *channel;
	uet_vpp_client_tx_request_t request;
	uint8_t *packet = pkt;
	uint8_t *ip = iphdr;
	void *dma_data;
	size_t channel_index;
	size_t available, capacity, ip_length;
	int rc;

	if (!vdata || ip < packet || (size_t)(ip - packet) > pkt_size)
		return -EINVAL;
	available = pkt_size - (size_t)(ip - packet);
	rc = nic_vpp_ip_length(ip, available, &ip_length);
	if (rc)
		return rc;
	channel_index = nic_vpp_select_channel(vdata, ip, ip_length);
	channel = &vdata->channels[channel_index];
	rc = pthread_mutex_lock(&channel->lock);
	if (rc)
		return -rc;

	for (;;) {
		rc = uet_vpp_client_acquire_dma(channel->client,
						channel->channel_index,
						&request.dma_slot,
						&dma_data, &capacity);
		if (rc != -EAGAIN)
			break;
		rc = nic_vpp_progress_tx(channel);
		if (rc < 0)
			goto out_unlock;
	}
	if (rc)
		goto out_unlock;
	if (ip_length > capacity) {
		uet_vpp_client_release_dma(channel->client,
					   channel->channel_index,
					   request.dma_slot);
		rc = -EMSGSIZE;
		goto out_unlock;
	}

	memcpy(dma_data, ip, ip_length);
	request.packet_length = ip_length;
	request.request_id = ++channel->next_request_id;
	request.user_context = 0;
	for (;;) {
		rc = uet_vpp_client_submit_ip_batch(channel->client,
						channel->channel_index,
						&request, 1);
		if (rc != -EAGAIN)
			break;
		rc = nic_vpp_progress_tx(channel);
		if (rc < 0)
			break;
	}
	if (rc) {
		uet_vpp_client_release_dma(channel->client,
					   channel->channel_index,
					   request.dma_slot);
		goto out_unlock;
	}
	channel->tx_inflight++;

out_unlock:
	pthread_mutex_unlock(&channel->lock);
	return rc;
}

static int nic_vpp_rx_poll_locked(struct vpp_data *vdata)
{
	int lock_rc, rc;

	if (vdata->pending_channel < vdata->channel_count)
		return 1;

	for (size_t i = 0; i < vdata->channel_count; i++) {
		size_t index = (vdata->rx_cursor + i) % vdata->channel_count;
		struct vpp_channel *channel = &vdata->channels[index];

		lock_rc = pthread_mutex_lock(&channel->lock);
		if (lock_rc)
			return -lock_rc;
		rc = nic_vpp_progress_tx(channel);
		if (rc >= 0 && !channel->rx_pending) {
			rc = uet_vpp_client_poll_rx(channel->client,
						channel->channel_index,
						&channel->pending_rx);
			channel->rx_pending = rc == 1;
		}
		if (rc >= 0 && channel->rx_pending) {
			vdata->pending_channel = index;
			vdata->rx_cursor = (index + 1) % vdata->channel_count;
			pthread_mutex_unlock(&channel->lock);
			return 1;
		}
		pthread_mutex_unlock(&channel->lock);
		if (rc < 0)
			return rc;
	}
	return 0;
}

int nic_vpp_rx_poll(struct uet_nic *nic)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	int lock_rc, rc;

	if (!vdata)
		return -EINVAL;
	lock_rc = pthread_mutex_lock(&vdata->rx_lock);
	if (lock_rc)
		return -lock_rc;
	rc = nic_vpp_rx_poll_locked(vdata);
	pthread_mutex_unlock(&vdata->rx_lock);
	return rc;
}

int nic_vpp_rx_pkt(struct uet_nic *nic, void *pkt, size_t pkt_buf_size,
		   size_t *rx_pkt_size)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	struct vpp_channel *channel;
	struct ethhdr *eth = pkt;
	size_t frame_length, offset = sizeof(*eth);
	uint16_t i;
	int lock_rc, rc;

	if (!vdata)
		return -EINVAL;
	lock_rc = pthread_mutex_lock(&vdata->rx_lock);
	if (lock_rc)
		return -lock_rc;
	if (vdata->pending_channel >= vdata->channel_count) {
		rc = nic_vpp_rx_poll_locked(vdata);
		if (rc <= 0)
			goto out_unlock_rx;
	}
	channel = &vdata->channels[vdata->pending_channel];
	lock_rc = pthread_mutex_lock(&channel->lock);
	if (lock_rc) {
		rc = -lock_rc;
		goto out_unlock_rx;
	}

	frame_length = sizeof(*eth) + channel->pending_rx.packet_length;
	if (frame_length < nic->min_pkt_size)
		frame_length = nic->min_pkt_size;
	if (frame_length > pkt_buf_size) {
		rc = nic_vpp_release_pending_rx(vdata);
		if (!rc)
			rc = -EMSGSIZE;
		goto out_unlock_channel;
	}

	memset(pkt, 0, frame_length);
	memcpy(eth->h_dest, nic->mac_addr, ETH_ALEN);
	eth->h_source[0] = 0x02;
	eth->h_source[3] = 0x50;
	eth->h_source[4] = 0x45;
	eth->h_source[5] = 0x45;
	eth->h_proto = htons((channel->pending_rx.flags &
			      UET_VPP_CLIENT_RX_F_IP6) ?
			     ETH_P_IPV6 : ETH_P_IP);
	for (i = 0; i < channel->pending_rx.iov_count; i++) {
		memcpy((uint8_t *)pkt + offset,
		       channel->pending_rx.iov[i].base,
		       channel->pending_rx.iov[i].length);
		offset += channel->pending_rx.iov[i].length;
	}

	rc = nic_vpp_release_pending_rx(vdata);
	if (!rc) {
		*rx_pkt_size = frame_length;
		rc = 1;
	}

out_unlock_channel:
	pthread_mutex_unlock(&channel->lock);
out_unlock_rx:
	pthread_mutex_unlock(&vdata->rx_lock);
	return rc;
}

static int nic_vpp_discard_rx(struct vpp_channel *channel)
{
	int rc;

	if (!channel->rx_pending) {
		rc = uet_vpp_client_poll_rx(channel->client,
					    channel->channel_index,
					    &channel->pending_rx);
		if (rc <= 0)
			return rc;
		channel->rx_pending = true;
	}
	rc = uet_vpp_client_release_rx(channel->client,
				       channel->channel_index,
				       &channel->pending_rx);
	if (!rc)
		channel->rx_pending = false;
	return rc ? rc : 1;
}

void nic_vpp_finalize(struct uet_nic *nic)
{
	struct vpp_data *vdata = nic->nic_priv_data;
	uint64_t deadline = 0, now;
	int drain_error = 0;
	int close_error = 0;
	bool timed = true;

	if (!vdata)
		return;
	if (nic_vpp_monotonic_ns(&now)) {
		timed = false;
		drain_error = -EIO;
	} else {
		deadline = now + UET_VPP_CLOSE_TIMEOUT_NS;
	}
	while (timed && vdata->client && vdata->dma_mapped) {
		bool drained = true;
		bool progressed = false;

		for (size_t i = 0; i < vdata->channel_count; i++) {
			struct vpp_channel *channel = &vdata->channels[i];
			int completion_error = 0;
			int rx_rc, tx_rc;

			tx_rc = nic_vpp_poll_tx(channel, &completion_error);
			if (tx_rc < 0) {
				if (!drain_error)
					drain_error = tx_rc;
				timed = false;
				break;
			}
			progressed |= tx_rc > 0;
			if (completion_error && !drain_error)
				drain_error = completion_error;

			rx_rc = nic_vpp_discard_rx(channel);
			if (rx_rc < 0 && rx_rc != -EAGAIN) {
				if (!drain_error)
					drain_error = rx_rc;
				timed = false;
				break;
			}
			progressed |= rx_rc > 0;
			drained &= !channel->tx_inflight &&
				   !channel->rx_pending && rx_rc == 0;
		}

		if (!timed)
			break;
		if (drained) {
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
	if (close_error)
		UET_API_ERR("VPP client close failed: %d", close_error);
	if (drain_error)
		UET_API_ERR("VPP channel-set drain failed: %d", drain_error);
	for (size_t i = 0; i < vdata->channel_count; i++)
		if (vdata->channels[i].lock_initialized)
			pthread_mutex_destroy(&vdata->channels[i].lock);
	if (vdata->rx_lock_initialized)
		pthread_mutex_destroy(&vdata->rx_lock);
	free(vdata->channels);
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

static void nic_vpp_initialize_cleanup(struct vpp_data *vdata)
{
	if (!vdata)
		return;
	for (size_t i = 0; i < vdata->channel_count; i++) {
		struct vpp_channel *channel = &vdata->channels[i];

		if (channel->lock_initialized)
			pthread_mutex_destroy(&channel->lock);
	}
	if (vdata->client)
		uet_vpp_client_close(vdata->client);
	if (vdata->rx_lock_initialized)
		pthread_mutex_destroy(&vdata->rx_lock);
	free(vdata->channels);
	free(vdata);
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
	vdata->pending_channel = SIZE_MAX;
	rc = uet_vpp_client_open(&vdata->client, segment_name,
				 &vdata->info);
	if (rc) {
		UET_API_ERR("could not open VPP channel-set %s: %d",
			    segment_name, rc);
		goto err_cleanup;
	}
	if (!vdata->info.channel_count ||
	    vdata->info.channel_count > UET_VPP_MAX_CHANNELS) {
		rc = -EPROTO;
		goto err_cleanup;
	}
	vdata->channel_count = vdata->info.channel_count;
	rc = uet_vpp_client_map_dma(vdata->client, dma_socket);
	if (rc)
		goto err_cleanup;
	vdata->dma_mapped = true;
	rc = posix_memalign((void **)&vdata->channels,
			    UET_VPP_CHANNEL_ALIGNMENT,
			    vdata->channel_count * sizeof(*vdata->channels));
	if (rc) {
		rc = rc == ENOMEM ? -ENOMEM : -EINVAL;
		goto err_cleanup;
	}
	memset(vdata->channels, 0,
	       vdata->channel_count * sizeof(*vdata->channels));
	rc = pthread_mutex_init(&vdata->rx_lock, NULL);
	if (rc)
		goto err_cleanup;
	vdata->rx_lock_initialized = true;

	for (size_t i = 0; i < vdata->channel_count; i++) {
		struct vpp_channel *channel = &vdata->channels[i];

		rc = pthread_mutex_init(&channel->lock, NULL);
		if (rc)
			goto err_cleanup;
		channel->lock_initialized = true;
		channel->client = vdata->client;
		channel->channel_index = (uint32_t)i;
		channel->next_request_id = (uint64_t)i << 56;
	}

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
err_cleanup:
	if (rc > 0)
		rc = -rc;
	nic_vpp_initialize_cleanup(vdata);
	return rc;
}

#endif /* ENABLE_VPP */
