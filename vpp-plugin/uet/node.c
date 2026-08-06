/* SPDX-License-Identifier: Apache-2.0 */

#include <uet/uet.h>

#include <vnet/udp/udp_packet.h>

#define UET_SPSC_MAX_BATCH 64

typedef enum
{
  UET_TX_NEXT_IP4_LOOKUP,
  UET_TX_NEXT_IP6_LOOKUP,
  UET_TX_N_NEXT,
} uet_tx_next_t;

#define foreach_uet_tx_error                                                                       \
  _ (INVALID_REQUEST, "invalid external UET request")                                              \
  _ (COMPLETION_RING_FULL, "UET completion ring full")                                             \
  _ (INVALID_RX_RELEASE, "invalid external UET RX release")

typedef enum
{
#define _(name, string) UET_TX_ERROR_##name,
  foreach_uet_tx_error
#undef _
    UET_TX_N_ERROR,
} uet_tx_error_t;

#define foreach_uet_local_error                                                                    \
  _ (NO_CLIENT, "UET packet dropped: no ready external client")                                    \
  _ (RING_FULL, "UET packet dropped: external RX ring full")                                       \
  _ (BAD_CHAIN, "UET packet dropped: invalid VLIB buffer chain")                                   \
  _ (HANDOFF_QUEUE_FULL, "UET packet dropped: worker handoff queue full")

typedef enum
{
#define _(name, string) UET_LOCAL_ERROR_##name,
  foreach_uet_local_error
#undef _
    UET_LOCAL_N_ERROR,
} uet_local_error_t;

typedef enum
{
  UET_TRACE_TX_ROUTED,
  UET_TRACE_RX_DELIVERED,
  UET_TRACE_RX_NO_CLIENT,
  UET_TRACE_RX_RING_FULL,
  UET_TRACE_RX_BAD_CHAIN,
} uet_trace_disposition_t;

typedef struct
{
  u64 id;
  u32 packet_length;
  u32 sw_if_index_or_table_id;
  u32 thread_index;
  u16 next_index;
  u8 direction;
  u8 ip_version;
  u8 is_udp;
  u8 disposition;
  u8 segment_count;
} uet_trace_t;

typedef struct
{
  u32 remaining;
  u8 initialized;
} uet_trace_state_t;

static_always_inline void
uet_tx_error_count (vlib_main_t *vm, uet_tx_error_t error)
{
  vlib_error_count (vm, uet_input_node.index, error, 1);
}

static_always_inline int
uet_trace_claim (vlib_main_t *vm, vlib_node_runtime_t *node, vlib_buffer_t *buffer, u32 next_index,
		 uet_trace_state_t *state)
{
  if (PREDICT_FALSE (!state->initialized))
    {
      state->remaining = vlib_get_trace_count (vm, node);
      state->initialized = 1;
    }

  if (state->remaining && vlib_trace_buffer (vm, node, next_index, buffer, 0 /* follow_chain */))
    {
      state->remaining--;
      /* `clear trace` may remove the per-node state concurrently. */
      if (PREDICT_TRUE (node->node_index < vec_len (vm->trace_main.nodes)))
	vlib_set_trace_count (vm, node, state->remaining);
      return 1;
    }
  return buffer->flags & VLIB_BUFFER_IS_TRACED;
}

static_always_inline void
uet_trace_add (vlib_main_t *vm, vlib_node_runtime_t *node, vlib_buffer_t *buffer,
	       uet_trace_state_t *state, u32 next_index, u64 id, u32 packet_length,
	       u32 sw_if_index_or_table_id, u8 direction, u8 ip_version, u8 is_udp,
	       uet_trace_disposition_t disposition, u8 segment_count)
{
  uet_trace_t *trace;

  if (PREDICT_TRUE (!vm->trace_main.trace_enable))
    return;
  if (PREDICT_TRUE (!uet_trace_claim (vm, node, buffer, next_index, state)))
    return;

  trace = vlib_add_trace (vm, node, buffer, sizeof (*trace));
  trace->id = id;
  trace->packet_length = packet_length;
  trace->sw_if_index_or_table_id = sw_if_index_or_table_id;
  trace->thread_index = vm->thread_index;
  trace->next_index = next_index;
  trace->direction = direction;
  trace->ip_version = ip_version;
  trace->is_udp = is_udp;
  trace->disposition = disposition;
  trace->segment_count = segment_count;
}

static_always_inline int
uet_tx_l4_prepare (u8 protocol, u8 *l4, u32 l4_length)
{
  if (protocol == UET_IP_PROTOCOL)
    return l4_length >= 4; /* Native-IP entropy header. */

  if (protocol == IP_PROTOCOL_UDP && l4_length >= sizeof (udp_header_t))
    {
      udp_header_t *udp = (udp_header_t *) l4;

      if (clib_net_to_host_u16 (udp->dst_port) != UET_UDP_PORT ||
	  clib_net_to_host_u16 (udp->length) != l4_length)
	return 0;

      /* UE Specification 1.0.3, section 3.5.10.1: udp.checksum MUST
       * be zero on send.  Normalize provider input here and do not request
       * UDP checksum offload. */
      udp->checksum = 0;
      return 1;
    }

  return 0;
}

/*
 * Validate the provider-supplied IP packet and initialize exactly the
 * metadata consumed by ip4-lookup/ip6-lookup.  VLIB_TX carries a FIB index
 * override, not an output interface.  The lookup still selects the route,
 * adjacency, interface and device path.
 */
static_always_inline int
uet_tx_ip_prepare (vlib_buffer_t *buffer, u32 packet_length, u32 ip4_fib_index, u32 ip6_fib_index,
		   u16 *next)
{
  u8 *packet = buffer->data;
  u32 fib_index;
  u8 version;

  if (packet_length < sizeof (ip4_header_t))
    return 0;
  version = packet[0] >> 4;

  if (version == 4)
    {
      const ip4_header_t *ip4 = (const ip4_header_t *) packet;
      u32 header_length = ip4_header_bytes (ip4);

      if (header_length < sizeof (*ip4) || header_length > packet_length ||
	  clib_net_to_host_u16 (ip4->length) != packet_length ||
	  !uet_tx_l4_prepare (ip4->protocol, packet + header_length, packet_length - header_length))
	return 0;

      buffer->flags |= VNET_BUFFER_F_IS_IP4 | VNET_BUFFER_F_L3_HDR_OFFSET_VALID;
      fib_index = ip4_fib_index;
      *next = UET_TX_NEXT_IP4_LOOKUP;
    }
  else if (version == 6)
    {
      const ip6_header_t *ip6 = (const ip6_header_t *) packet;

      if (packet_length < sizeof (*ip6) ||
	  (u32) clib_net_to_host_u16 (ip6->payload_length) + sizeof (*ip6) != packet_length ||
	  !uet_tx_l4_prepare (ip6->protocol, packet + sizeof (*ip6), packet_length - sizeof (*ip6)))
	return 0;

      buffer->flags |= VNET_BUFFER_F_IS_IP6 | VNET_BUFFER_F_L3_HDR_OFFSET_VALID;
      fib_index = ip6_fib_index;
      *next = UET_TX_NEXT_IP6_LOOKUP;
    }
  else
    return 0;

  buffer->current_data = 0;
  buffer->current_length = packet_length;
  vnet_buffer (buffer)->l3_hdr_offset = 0;
  vnet_buffer (buffer)->sw_if_index[VLIB_RX] = 0;
  vnet_buffer (buffer)->sw_if_index[VLIB_TX] = fib_index;
  return 1;
}

static_always_inline u32
uet_spsc_index (const uet_vpp_svm_spsc_ring_t *ring, u32 counter)
{
  return ring->mask ? counter & ring->mask : counter % ring->size;
}

static_always_inline void
uet_rx_release_process (vlib_main_t *vm, uet_worker_t *uw)
{
  uet_vpp_svm_spsc_ring_t *ring = uw->rx_release_ring;
  u32 consumer = clib_atomic_load_relax_n (&ring->consumer);
  u32 producer = clib_atomic_load_acq_n (&ring->producer);
  u32 available = producer - consumer;

  if (PREDICT_FALSE (available > ring->size))
    {
      uw->rx_invalid_releases++;
      uet_tx_error_count (vm, UET_TX_ERROR_INVALID_RX_RELEASE);
      return;
    }
  available = clib_min (available, (u32) UET_SPSC_MAX_BATCH);

  for (u32 i = 0; i < available; i++)
    {
      const uet_vpp_svm_rx_release_t *release =
	uw->rx_release_entries + uet_spsc_index (ring, consumer + i);
      u32 token = release->release_token;

      if (release->reserved || !release->rx_id || token >= uw->dma_slot_count ||
	  uw->rx_ids[token] != release->rx_id)
	{
	  uw->rx_invalid_releases++;
	  uet_tx_error_count (vm, UET_TX_ERROR_INVALID_RX_RELEASE);
	  continue;
	}

      vlib_buffer_free_one (vm, uw->rx_buffer_indices[token]);
      uw->rx_ids[token] = 0;
      uw->rx_buffer_indices[token] = ~0;
      ASSERT (uw->rx_free_count < uw->dma_slot_count);
      uw->rx_free_slots[uw->rx_free_count++] = token;
      uw->rx_releases++;
      ASSERT (uw->rx_outstanding > 0);
      uw->rx_outstanding--;
    }

  if (available)
    clib_atomic_store_rel_n (&ring->consumer, consumer + available);
}

static_always_inline int
uet_rx_descriptor_build (vlib_main_t *vm, uet_main_t *um, vlib_buffer_t *buffer, u32 release_token,
			 u8 is_ip4, u8 is_udp, uet_vpp_svm_rx_desc_t *desc)
{
  i16 l3_offset = vnet_buffer (buffer)->l3_hdr_offset;
  u8 *start = buffer->data + l3_offset;
  u8 *end = vlib_buffer_get_current (buffer) + buffer->current_length;
  u32 packet_length, remaining;
  vlib_buffer_t *segment_buffer = buffer;
  u8 *segment_start = start;

  if (start < buffer->data - VLIB_BUFFER_PRE_DATA_SIZE || start >= end)
    return 0;

  if (is_ip4)
    {
      const ip4_header_t *ip4 = (const ip4_header_t *) start;

      if ((uword) (end - start) < sizeof (*ip4) || ip4->ip_version_and_header_length >> 4 != 4)
	return 0;
      packet_length = clib_net_to_host_u16 (ip4->length);
    }
  else
    {
      const ip6_header_t *ip6 = (const ip6_header_t *) start;

      if ((uword) (end - start) < sizeof (*ip6) ||
	  clib_net_to_host_u32 (ip6->ip_version_traffic_class_and_flow_label) >> 28 != 6)
	return 0;
      packet_length = sizeof (*ip6) + clib_net_to_host_u16 (ip6->payload_length);
    }

  if (!packet_length ||
      packet_length > (u32) (end - start) + buffer->total_length_not_including_first_buffer)
    return 0;

  clib_memset (desc, 0, sizeof (*desc));
  desc->release_token = release_token;
  desc->packet_length = packet_length;
  desc->rx_sw_if_index = vnet_buffer (buffer)->ip.rx_sw_if_index;
  desc->flags = is_ip4 ? UET_VPP_SVM_RX_F_IP4 : UET_VPP_SVM_RX_F_IP6;
  desc->flags |= is_udp ? UET_VPP_SVM_RX_F_UDP : 0;
  remaining = packet_length;

  while (remaining)
    {
      u32 length;
      u64 data_offset;

      if (desc->segment_count == UET_VPP_SVM_MAX_RX_SEGS)
	return 0;
      length = clib_min (remaining, (u32) ((u8 *) vlib_buffer_get_current (segment_buffer) +
					   segment_buffer->current_length - segment_start));
      if (!length || segment_start < um->dma_map_base ||
	  (u64) (segment_start - um->dma_map_base) >= um->dma_map_size)
	return 0;
      data_offset = segment_start - um->dma_map_base;
      if (length > um->dma_map_size - data_offset)
	return 0;

      desc->segments[desc->segment_count].data_offset = data_offset;
      desc->segments[desc->segment_count].length = length;
      desc->segment_count++;
      remaining -= length;
      if (!remaining)
	break;
      if (!(segment_buffer->flags & VLIB_BUFFER_NEXT_PRESENT))
	return 0;
      segment_buffer = vlib_get_buffer (vm, segment_buffer->next_buffer);
      segment_start = vlib_buffer_get_current (segment_buffer);
    }

  return 1;
}

static_always_inline u32
uet_rx_ip_packet_length (vlib_main_t *vm, vlib_buffer_t *buffer, u8 is_ip4)
{
  u8 *start = buffer->data + vnet_buffer (buffer)->l3_hdr_offset;
  u8 *end = (u8 *) vlib_buffer_get_current (buffer) + buffer->current_length;
  u32 length = 0;

  if (start >= buffer->data - VLIB_BUFFER_PRE_DATA_SIZE && start < end)
    {
      if (is_ip4 && (uword) (end - start) >= sizeof (ip4_header_t))
	length = clib_net_to_host_u16 (((ip4_header_t *) start)->length);
      else if (!is_ip4 && (uword) (end - start) >= sizeof (ip6_header_t))
	length =
	  sizeof (ip6_header_t) + clib_net_to_host_u16 (((ip6_header_t *) start)->payload_length);
      if (length <= (u32) (end - start) + buffer->total_length_not_including_first_buffer)
	return length;
    }
  return vlib_buffer_length_in_chain (vm, buffer);
}

static_always_inline void
uet_tx_completion_add (uet_worker_t *uw, const uet_vpp_svm_tx_completion_t *completion,
		       u32 *producer)
{
  u32 index = uet_spsc_index (uw->tx_completion_ring, *producer);

  clib_memcpy_fast (uw->tx_completion_entries + index, completion, sizeof (*completion));
  (*producer)++;
  uw->tx_completions++;
}

/*
 * Transfer the populated slot buffer to the VPP graph and replenish the slot
 * before publishing its completion.  The graph owns the old buffer normally;
 * no output-interface or device-node knowledge is required to reclaim it.
 */
static_always_inline int
uet_dma_slot_replace (vlib_main_t *vm, uet_main_t *um, uet_worker_t *uw, u32 slot,
		      u32 *tx_buffer_index)
{
  u32 replacement_index;
  vlib_buffer_t *replacement;
  u64 data_offset;

  if (vlib_buffer_alloc_from_pool (vm, &replacement_index, 1, um->dma_buffer_pool_index) != 1)
    return 0;

  replacement = vlib_get_buffer (vm, replacement_index);
  data_offset = (u8 *) replacement->data - um->dma_map_base;
  if (PREDICT_FALSE (data_offset >= um->dma_map_size ||
		     um->dma_buffer_data_size > um->dma_map_size - data_offset))
    {
      vlib_buffer_free_one (vm, replacement_index);
      return 0;
    }

  *tx_buffer_index = uw->dma_buffer_indices[slot];
  uw->dma_buffer_indices[slot] = replacement_index;
  uw->dma_slots[slot].data_offset = data_offset;
  uw->dma_slots[slot].capacity = um->dma_buffer_data_size;
  return 1;
}

static_always_inline void
uet_tx_process (vlib_main_t *vm, uet_worker_t *uw, u32 *tx_buffers, u16 *tx_nexts,
		u64 *tx_trace_ids, u32 *n_tx)
{
  uet_main_t *um = &uet_main;
  uet_vpp_svm_spsc_ring_t *tx_ring = uw->tx_ring;
  uet_vpp_svm_spsc_ring_t *cq_ring = uw->tx_completion_ring;
  u32 cq_producer = clib_atomic_load_relax_n (&cq_ring->producer);
  u32 cq_initial_producer = cq_producer;
  u32 cq_consumer = clib_atomic_load_acq_n (&cq_ring->consumer);
  u32 tx_consumer, tx_producer, tx_available, n_processed = 0;

  if (PREDICT_FALSE (cq_producer - cq_consumer > cq_ring->size))
    {
      uet_tx_error_count (vm, UET_TX_ERROR_COMPLETION_RING_FULL);
      return;
    }

  tx_consumer = clib_atomic_load_relax_n (&tx_ring->consumer);
  tx_producer = clib_atomic_load_acq_n (&tx_ring->producer);
  tx_available = tx_producer - tx_consumer;
  if (PREDICT_FALSE (tx_available > tx_ring->size))
    {
      uw->invalid_requests++;
      uet_tx_error_count (vm, UET_TX_ERROR_INVALID_REQUEST);
      tx_available = tx_ring->size;
    }

  while (n_processed < tx_available && *n_tx < UET_SPSC_MAX_BATCH)
    {
      u32 index = uet_spsc_index (tx_ring, tx_consumer + n_processed);
      const uet_vpp_svm_tx_desc_t *desc = uw->tx_descs + index;
      u32 slot = desc->dma_slot;
      uet_vpp_svm_tx_completion_t completion = {
	.dma_slot = UET_VPP_SVM_INVALID_DMA_SLOT,
	.request_id = desc->request_id,
	.user_context = desc->user_context,
      };
      u8 valid = 0;

      if (cq_producer - cq_consumer >= cq_ring->size)
	{
	  uw->tx_completion_ring_full++;
	  uet_tx_error_count (vm, UET_TX_ERROR_COMPLETION_RING_FULL);
	  break;
	}

      if (slot < uw->dma_slot_count)
	completion.dma_slot = slot;
      if (!uw->tx_configured)
	completion.status = UET_VPP_SVM_STATUS_TX_NOT_CONFIGURED;
      else if (slot >= uw->dma_slot_count || desc->packet_length > uw->dma_slots[slot].capacity ||
	       desc->reserved)
	{
	  completion.status = UET_VPP_SVM_STATUS_INVALID_PACKET;
	  uw->invalid_requests++;
	  uet_tx_error_count (vm, UET_TX_ERROR_INVALID_REQUEST);
	}
      else
	{
	  vlib_buffer_t *buffer = vlib_get_buffer (vm, uw->dma_buffer_indices[slot]);

	  if (clib_atomic_load_acq_n (&buffer->ref_count) != 1)
	    {
	      completion.status = UET_VPP_SVM_STATUS_SLOT_BUSY;
	      uw->invalid_requests++;
	      uet_tx_error_count (vm, UET_TX_ERROR_INVALID_REQUEST);
	    }
	  else
	    valid = 1;
	}

      if (!valid)
	uet_tx_completion_add (uw, &completion, &cq_producer);
      else
	{
	  vlib_buffer_t *buffer = vlib_get_buffer (vm, uw->dma_buffer_indices[slot]);
	  vlib_buffer_pool_t *pool = vlib_get_buffer_pool (vm, buffer->buffer_pool_index);

	  vlib_buffer_copy_template (buffer, &pool->buffer_template);
	  if (!uet_tx_ip_prepare (buffer, desc->packet_length, uw->tx_ip4_fib_index,
				  uw->tx_ip6_fib_index, tx_nexts + *n_tx))
	    {
	      completion.status = UET_VPP_SVM_STATUS_INVALID_PACKET;
	      uw->invalid_requests++;
	      uet_tx_error_count (vm, UET_TX_ERROR_INVALID_REQUEST);
	      uet_tx_completion_add (uw, &completion, &cq_producer);
	      goto next_request;
	    }
	  if (!uet_dma_slot_replace (vm, um, uw, slot, tx_buffers + *n_tx))
	    break;

	  completion.status = UET_VPP_SVM_STATUS_OK;
	  completion.completed_length = desc->packet_length;
	  uet_tx_completion_add (uw, &completion, &cq_producer);
	  tx_trace_ids[*n_tx] = desc->request_id;
	  (*n_tx)++;
	  uw->tx_packets++;
	  uw->tx_bytes += desc->packet_length;
	}

    next_request:
      uw->tx_requests++;
      n_processed++;
    }

  if (cq_producer != cq_initial_producer)
    clib_atomic_store_rel_n (&cq_ring->producer, cq_producer);
  if (n_processed)
    clib_atomic_store_rel_n (&tx_ring->consumer, tx_consumer + n_processed);
}

static __clib_noinline void
uet_tx_trace_batch (vlib_main_t *vm, vlib_node_runtime_t *node, uet_worker_t *uw,
		    const u32 *tx_buffers, const u16 *tx_nexts, const u64 *tx_trace_ids, u32 n_tx)
{
  uet_trace_state_t trace_state = { 0 };

  for (u32 i = 0; i < n_tx; i++)
    {
      vlib_buffer_t *buffer = vlib_get_buffer (vm, tx_buffers[i]);
      u8 ip_version = buffer->data[0] >> 4;
      u8 is_udp = ip_version == 4 ? ((ip4_header_t *) buffer->data)->protocol == IP_PROTOCOL_UDP :
				    ((ip6_header_t *) buffer->data)->protocol == IP_PROTOCOL_UDP;

      uet_trace_add (vm, node, buffer, &trace_state, tx_nexts[i], tx_trace_ids[i],
		     buffer->current_length,
		     ip_version == 4 ? uw->tx_ip4_table_id : uw->tx_ip6_table_id, 0 /* TX */,
		     ip_version, is_udp, UET_TRACE_TX_ROUTED, 1);
    }
}

static_always_inline u32
uet_svm_process (vlib_main_t *vm, vlib_node_runtime_t *node, uet_worker_t *uw)
{
  u32 tx_buffers[UET_SPSC_MAX_BATCH + 1];
  u16 tx_nexts[UET_SPSC_MAX_BATCH + 1];
  u64 tx_trace_ids[UET_SPSC_MAX_BATCH + 1];
  u32 n_tx = 0;
  u32 client_flags;

  client_flags = clib_atomic_load_acq_n (&uw->svm_header->client_flags);
  clib_atomic_store_rel_n (
    &uw->svm_header->server_flags,
    client_flags & UET_VPP_SVM_CLIENT_F_DMA_READY ? UET_VPP_SVM_SERVER_F_DMA_READY_ACK : 0);

  uet_rx_release_process (vm, uw);
  uet_tx_process (vm, uw, tx_buffers, tx_nexts, tx_trace_ids, &n_tx);

  if (n_tx)
    {
      if (PREDICT_FALSE (vm->trace_main.trace_enable))
	uet_tx_trace_batch (vm, node, uw, tx_buffers, tx_nexts, tx_trace_ids, n_tx);
      vlib_buffer_enqueue_to_next (vm, node, tx_buffers, tx_nexts, n_tx);
    }

  return n_tx;
}

VLIB_NODE_FN (uet_input_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  uet_main_t *um = &uet_main;
  uet_worker_t *uw;

  if (PREDICT_FALSE (!um->enabled || vm->thread_index == 0 ||
		     vm->thread_index >= vec_len (um->workers)))
    return 0;

  uw = vec_elt_at_index (um->workers, vm->thread_index);
  ASSERT (uw->thread_index == vm->thread_index);

  uw->poll_calls++;

  if (PREDICT_TRUE (uw->svm_attached))
    return uet_svm_process (vm, node, uw);

  return 0;
}

typedef enum
{
  UET_LOCAL_NEXT_DROP,
  UET_LOCAL_N_NEXT,
} uet_local_next_t;

static_always_inline uword
uet_local_deliver (vlib_main_t *vm, vlib_node_runtime_t *node, u32 *from, u32 n_vectors, u8 is_ip4,
		   u8 is_udp)
{
  uet_main_t *um = &uet_main;
  uet_worker_t *uw = 0;
  u32 drop_buffers[VLIB_FRAME_SIZE];
  u32 n_drop = 0;
  u64 bytes = 0;
  uet_trace_state_t trace_state = { 0 };

  if (PREDICT_TRUE (um->enabled && vm->thread_index > 0 &&
		    vm->thread_index < vec_len (um->workers)))
    {
      uw = vec_elt_at_index (um->workers, vm->thread_index);
      if (PREDICT_TRUE (uw->svm_attached))
	uet_rx_release_process (vm, uw);
      for (u32 i = 0; i < n_vectors; i++)
	bytes += uet_rx_ip_packet_length (vm, vlib_get_buffer (vm, from[i]), is_ip4);

      if (is_udp && is_ip4)
	{
	  uw->rx_udp4_packets += n_vectors;
	  uw->rx_udp4_bytes += bytes;
	}
      else if (is_udp)
	{
	  uw->rx_udp6_packets += n_vectors;
	  uw->rx_udp6_bytes += bytes;
	}
      else if (is_ip4)
	{
	  uw->rx_ip4_packets += n_vectors;
	  uw->rx_ip4_bytes += bytes;
	}
      else
	{
	  uw->rx_ip6_packets += n_vectors;
	  uw->rx_ip6_bytes += bytes;
	}

      if (PREDICT_TRUE (uw->svm_attached &&
			(clib_atomic_load_acq_n (&uw->svm_header->client_flags) &
			 UET_VPP_SVM_CLIENT_F_DMA_READY)))
	{
	  uet_vpp_svm_spsc_ring_t *ring = uw->rx_ring;
	  u32 producer = clib_atomic_load_relax_n (&ring->producer);
	  u32 consumer = clib_atomic_load_acq_n (&ring->consumer);

	  for (u32 i = 0; i < n_vectors; i++)
	    {
	      u32 buffer_index = from[i];
	      vlib_buffer_t *buffer = vlib_get_buffer (vm, buffer_index);
	      uet_vpp_svm_rx_desc_t descriptor;
	      u32 token;
	      u64 rx_id;

	      if (PREDICT_FALSE (producer - consumer >= ring->size || !uw->rx_free_count))
		{
		  uw->rx_ring_full++;
		  buffer->error = node->errors[UET_LOCAL_ERROR_RING_FULL];
		  uet_trace_add (vm, node, buffer, &trace_state, UET_LOCAL_NEXT_DROP, 0,
				 uet_rx_ip_packet_length (vm, buffer, is_ip4),
				 vnet_buffer (buffer)->ip.rx_sw_if_index, 1 /* RX */,
				 is_ip4 ? 4 : 6, is_udp, UET_TRACE_RX_RING_FULL, 0);
		  drop_buffers[n_drop++] = buffer_index;
		  continue;
		}

	      token = uw->rx_free_slots[uw->rx_free_count - 1];
	      if (PREDICT_FALSE (
		    !uet_rx_descriptor_build (vm, um, buffer, token, is_ip4, is_udp, &descriptor)))
		{
		  uw->rx_bad_chain++;
		  buffer->error = node->errors[UET_LOCAL_ERROR_BAD_CHAIN];
		  uet_trace_add (vm, node, buffer, &trace_state, UET_LOCAL_NEXT_DROP, 0,
				 uet_rx_ip_packet_length (vm, buffer, is_ip4),
				 vnet_buffer (buffer)->ip.rx_sw_if_index, 1 /* RX */,
				 is_ip4 ? 4 : 6, is_udp, UET_TRACE_RX_BAD_CHAIN, 0);
		  drop_buffers[n_drop++] = buffer_index;
		  continue;
		}

	      rx_id = uw->next_rx_id++;
	      if (PREDICT_FALSE (!rx_id))
		rx_id = uw->next_rx_id++;
	      descriptor.rx_id = rx_id;
	      clib_memcpy_fast (uw->rx_descs + uet_spsc_index (ring, producer), &descriptor,
				sizeof (descriptor));
	      uw->rx_free_count--;
	      uw->rx_ids[token] = rx_id;
	      uw->rx_buffer_indices[token] = buffer_index;
	      uw->rx_outstanding++;
	      uw->rx_delivered++;
	      uet_trace_add (vm, node, buffer, &trace_state, UET_LOCAL_NEXT_DROP, rx_id,
			     descriptor.packet_length, descriptor.rx_sw_if_index, 1 /* RX */,
			     is_ip4 ? 4 : 6, is_udp, UET_TRACE_RX_DELIVERED,
			     descriptor.segment_count);
	      producer++;
	    }

	  clib_atomic_store_rel_n (&ring->producer, producer);
	}
      else
	{
	  for (u32 i = 0; i < n_vectors; i++)
	    {
	      vlib_buffer_t *buffer = vlib_get_buffer (vm, from[i]);

	      buffer->error = node->errors[UET_LOCAL_ERROR_NO_CLIENT];
	      uet_trace_add (vm, node, buffer, &trace_state, UET_LOCAL_NEXT_DROP, 0,
			     uet_rx_ip_packet_length (vm, buffer, is_ip4),
			     vnet_buffer (buffer)->ip.rx_sw_if_index, 1 /* RX */, is_ip4 ? 4 : 6,
			     is_udp, UET_TRACE_RX_NO_CLIENT, 0);
	      drop_buffers[n_drop++] = from[i];
	    }
	}
    }
  else
    {
      for (u32 i = 0; i < n_vectors; i++)
	{
	  vlib_buffer_t *buffer = vlib_get_buffer (vm, from[i]);

	  buffer->error = node->errors[UET_LOCAL_ERROR_NO_CLIENT];
	  uet_trace_add (vm, node, buffer, &trace_state, UET_LOCAL_NEXT_DROP, 0,
			 uet_rx_ip_packet_length (vm, buffer, is_ip4),
			 vnet_buffer (buffer)->ip.rx_sw_if_index, 1 /* RX */, is_ip4 ? 4 : 6,
			 is_udp, UET_TRACE_RX_NO_CLIENT, 0);
	  drop_buffers[n_drop++] = from[i];
	}
    }

  if (n_drop)
    vlib_buffer_enqueue_to_single_next (vm, node, drop_buffers, UET_LOCAL_NEXT_DROP, n_drop);
  return n_vectors;
}

static_always_inline u16
uet_rx_target_thread (vlib_main_t *vm, vlib_buffer_t *buffer, u8 is_ip4)
{
  u8 *start = buffer->data + vnet_buffer (buffer)->l3_hdr_offset;
  u8 *end = vlib_buffer_get_current (buffer) + buffer->current_length;
  u8 *entropy;
  u32 hash;
  u16 value;

  if (PREDICT_FALSE (start < buffer->data - VLIB_BUFFER_PRE_DATA_SIZE || start >= end))
    return vm->thread_index;

  if (is_ip4)
    {
      ip4_header_t *ip4 = (ip4_header_t *) start;
      u32 header_length;

      if (PREDICT_FALSE ((uword) (end - start) < sizeof (*ip4)))
	return vm->thread_index;
      header_length = ip4_header_bytes (ip4);
      if (PREDICT_FALSE (header_length < sizeof (*ip4) ||
			 (uword) (end - start) < header_length + sizeof (value)))
	return vm->thread_index;
      entropy = start + header_length;
    }
  else
    {
      if (PREDICT_FALSE ((uword) (end - start) < sizeof (ip6_header_t) + sizeof (value)))
	return vm->thread_index;
      entropy = start + sizeof (ip6_header_t);
    }

  clib_memcpy_fast (&value, entropy, sizeof (value));
  hash = clib_net_to_host_u16 (value);
  hash ^= hash >> 7;
  hash *= 0x9e3779b1U;
  hash ^= hash >> 16;
  return vlib_get_worker_thread_index (hash % vlib_num_workers ());
}

static_always_inline uet_rx_path_t
uet_rx_path (u8 is_ip4, u8 is_udp)
{
  if (is_udp)
    return is_ip4 ? UET_RX_PATH_IP4_UDP : UET_RX_PATH_IP6_UDP;
  return is_ip4 ? UET_RX_PATH_IP4_NATIVE : UET_RX_PATH_IP6_NATIVE;
}

static_always_inline uword
uet_local_input (vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame, u8 is_ip4,
		 u8 is_udp)
{
  uet_main_t *um = &uet_main;
  u32 *from = vlib_frame_vector_args (frame);
  u32 local_buffers[VLIB_FRAME_SIZE], remote_buffers[VLIB_FRAME_SIZE];
  u16 remote_threads[VLIB_FRAME_SIZE];
  u32 n_local = 0, n_remote = 0;

  if (PREDICT_TRUE (!um->rx_entropy_handoff || vlib_num_workers () == 1))
    return uet_local_deliver (vm, node, from, frame->n_vectors, is_ip4, is_udp);

  for (u32 i = 0; i < frame->n_vectors; i++)
    {
      u16 target = uet_rx_target_thread (vm, vlib_get_buffer (vm, from[i]), is_ip4);

      if (target == vm->thread_index)
	local_buffers[n_local++] = from[i];
      else
	{
	  remote_buffers[n_remote] = from[i];
	  remote_threads[n_remote++] = target;
	}
    }

  if (n_remote)
    {
      u32 n_enqueued = vlib_buffer_enqueue_to_thread (
	vm, node, um->rx_handoff_queue_indices[uet_rx_path (is_ip4, is_udp)], remote_buffers,
	remote_threads, n_remote);

      if (PREDICT_FALSE (n_enqueued != n_remote))
	vlib_node_increment_counter (vm, node->node_index, UET_LOCAL_ERROR_HANDOFF_QUEUE_FULL,
				     n_remote - n_enqueued);
    }
  if (n_local)
    uet_local_deliver (vm, node, local_buffers, n_local, is_ip4, is_udp);
  return frame->n_vectors;
}

VLIB_NODE_FN (uet4_ip_input_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_input (vm, node, frame, 1 /* is_ip4 */, 0 /* is_udp */);
}

VLIB_NODE_FN (uet6_ip_input_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_input (vm, node, frame, 0 /* is_ip4 */, 0 /* is_udp */);
}

VLIB_NODE_FN (uet4_udp_input_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_input (vm, node, frame, 1 /* is_ip4 */, 1 /* is_udp */);
}

VLIB_NODE_FN (uet6_udp_input_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_input (vm, node, frame, 0 /* is_ip4 */, 1 /* is_udp */);
}

VLIB_NODE_FN (uet4_ip_handoff_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_deliver (vm, node, vlib_frame_vector_args (frame), frame->n_vectors,
			    1 /* is_ip4 */, 0 /* is_udp */);
}

VLIB_NODE_FN (uet6_ip_handoff_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_deliver (vm, node, vlib_frame_vector_args (frame), frame->n_vectors,
			    0 /* is_ip4 */, 0 /* is_udp */);
}

VLIB_NODE_FN (uet4_udp_handoff_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_deliver (vm, node, vlib_frame_vector_args (frame), frame->n_vectors,
			    1 /* is_ip4 */, 1 /* is_udp */);
}

VLIB_NODE_FN (uet6_udp_handoff_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return uet_local_deliver (vm, node, vlib_frame_vector_args (frame), frame->n_vectors,
			    0 /* is_ip4 */, 1 /* is_udp */);
}

#ifndef CLIB_MARCH_VARIANT
static char *uet_tx_error_strings[] = {
#define _(name, string) string,
  foreach_uet_tx_error
#undef _
};

static char *uet_local_error_strings[] = {
#define _(name, string) string,
  foreach_uet_local_error
#undef _
};

static u8 *
format_uet_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  uet_trace_t *trace = va_arg (*args, uet_trace_t *);
  const char *disposition = "unknown";

  switch (trace->disposition)
    {
    case UET_TRACE_TX_ROUTED:
      disposition = "routed";
      break;
    case UET_TRACE_RX_DELIVERED:
      disposition = "delivered";
      break;
    case UET_TRACE_RX_NO_CLIENT:
      disposition = "drop-no-client";
      break;
    case UET_TRACE_RX_RING_FULL:
      disposition = "drop-ring-full";
      break;
    case UET_TRACE_RX_BAD_CHAIN:
      disposition = "drop-bad-chain";
      break;
    }

  s = format (s, "UET %s: worker %u ip%u/%s length %u ", trace->direction ? "RX" : "TX",
	      trace->thread_index, trace->ip_version, trace->is_udp ? "udp" : "native",
	      trace->packet_length);
  if (trace->direction)
    s = format (s, "sw_if_index %u ", trace->sw_if_index_or_table_id);
  else
    s = format (s, "fib-table %u ", trace->sw_if_index_or_table_id);
  return format (s, "id %llu segments %u next %u disposition %s", trace->id, trace->segment_count,
		 trace->next_index, disposition);
}

VLIB_REGISTER_NODE(uet_input_node) = {
    .name = "uet-input",
    .type = VLIB_NODE_TYPE_INPUT,
    .state = VLIB_NODE_STATE_DISABLED,
    .format_trace = format_uet_trace,
    .flags = VLIB_NODE_FLAG_TRACE_SUPPORTED,
    .n_errors = ARRAY_LEN(uet_tx_error_strings),
    .error_strings = uet_tx_error_strings,
    .n_next_nodes = UET_TX_N_NEXT,
    .next_nodes =
        {
            [UET_TX_NEXT_IP4_LOOKUP] = "ip4-lookup",
            [UET_TX_NEXT_IP6_LOOKUP] = "ip6-lookup",
        },
};

#define UET_LOCAL_NODE_REGISTER(registration, node_name)                                           \
  VLIB_REGISTER_NODE (registration) = {                                                            \
    .name = node_name,                                                                             \
    .vector_size = sizeof (u32),                                                                   \
    .type = VLIB_NODE_TYPE_INTERNAL,                                                               \
    .format_trace = format_uet_trace,                                                              \
    .flags = VLIB_NODE_FLAG_TRACE_SUPPORTED,                                                       \
    .n_errors = ARRAY_LEN (uet_local_error_strings),                                               \
    .error_strings = uet_local_error_strings,                                                      \
    .n_next_nodes = UET_LOCAL_N_NEXT,                                                              \
    .next_nodes = { [UET_LOCAL_NEXT_DROP] = "error-drop" },                                        \
  }

UET_LOCAL_NODE_REGISTER (uet4_ip_input_node, "uet4-ip-input");
UET_LOCAL_NODE_REGISTER (uet6_ip_input_node, "uet6-ip-input");
UET_LOCAL_NODE_REGISTER (uet4_udp_input_node, "uet4-udp-input");
UET_LOCAL_NODE_REGISTER (uet6_udp_input_node, "uet6-udp-input");
UET_LOCAL_NODE_REGISTER (uet4_ip_handoff_node, "uet4-ip-handoff");
UET_LOCAL_NODE_REGISTER (uet6_ip_handoff_node, "uet6-ip-handoff");
UET_LOCAL_NODE_REGISTER (uet4_udp_handoff_node, "uet4-udp-handoff");
UET_LOCAL_NODE_REGISTER (uet6_udp_handoff_node, "uet6-udp-handoff");

#undef UET_LOCAL_NODE_REGISTER
#endif
