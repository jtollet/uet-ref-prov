/* SPDX-License-Identifier: Apache-2.0 */

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include <uet/uet.h>

#include <vlib/handoff.h>
#include <vlib/physmem_funcs.h>
#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <vnet/api_errno.h>
#include <vnet/fib/fib_table.h>
#include <vnet/plugin/plugin.h>
#include <vnet/udp/udp_local.h>
#include <vpp/app/version.h>

#include <uet/uet.api_enum.h>
#include <uet/uet.api_types.h>

#define REPLY_MSG_ID_BASE um->msg_id_base
#include <vlibapi/api_helper_macros.h>

#define UET_SVM_SEGMENT_BASE_SIZE (4ULL << 20)

uet_main_t uet_main;

VLIB_REGISTER_LOG_CLASS (uet_log) = {
  .class_name = "uet",
};

VLIB_PLUGIN_REGISTER () = {
  .version = UET_PLUGIN_BUILD_VERSION,
  .version_required = VPP_BUILD_VER,
  .description = "UET host dataplane (per-worker lockless channels)",
  .default_disabled = 1,
};

static void
uet_worker_counters_clear (uet_worker_t *uw)
{
  uw->poll_calls = 0;
  uw->tx_requests = 0;
  uw->invalid_requests = 0;
  uw->tx_completion_ring_full = 0;
  uw->tx_packets = 0;
  uw->tx_bytes = 0;
  uw->tx_completions = 0;
  uw->rx_ip4_packets = 0;
  uw->rx_ip4_bytes = 0;
  uw->rx_ip6_packets = 0;
  uw->rx_ip6_bytes = 0;
  uw->rx_udp4_packets = 0;
  uw->rx_udp4_bytes = 0;
  uw->rx_udp6_packets = 0;
  uw->rx_udp6_bytes = 0;
  uw->rx_delivered = 0;
  uw->rx_ambiguous = 0;
  uw->rx_ring_full = 0;
  uw->rx_bad_chain = 0;
  uw->rx_releases = 0;
  uw->rx_invalid_releases = 0;
}

void
uet_counters_clear (void)
{
  uet_main_t *um = &uet_main;

  um->dma_authorized_clients = 0;
  um->dma_rejected_clients = 0;
  vlib_worker_thread_barrier_sync (um->vlib_main);
  um->endpoint_collisions = 0;
  for (u32 worker = 0; worker < vlib_num_workers (); worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

      uet_worker_counters_clear (uw);
    }
  vlib_worker_thread_barrier_release (um->vlib_main);
}

static int
uet_svm_name_is_valid (const char *name)
{
  const u8 *p = (const u8 *) name;
  uword len = clib_strnlen (name, 65);

  if (len == 0 || len > 63)
    return 0;

  while (*p)
    {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
	    *p == '-' || *p == '_' || *p == '.'))
	return 0;
      p++;
    }
  return 1;
}

static u32
uet_client_count (const uet_main_t *um)
{
  return pool_elts (um->clients);
}

static uet_client_t *
uet_client_find (uet_main_t *um, const char *segment_name)
{
  uet_client_t *client;

  pool_foreach (client, um->clients)
    if (!strcmp ((char *) client->segment.name, segment_name))
      return client;
  return 0;
}

static void
uet_worker_channel_dma_buffers_free (uet_main_t *um, uet_worker_channel_t *channel)
{
  if (vec_len (channel->dma_buffer_indices))
    vlib_buffer_free (um->vlib_main, channel->dma_buffer_indices,
		      vec_len (channel->dma_buffer_indices));
  vec_free (channel->dma_buffer_indices);
}

static void
uet_dma_mapping_reset_if_unused (uet_main_t *um)
{
  if (uet_client_count (um))
    return;
  um->dma_buffer_pool_index = (u8) ~0;
  um->dma_buffer_data_size = 0;
  um->dma_map_size = 0;
  um->dma_map_base = 0;
}

static void
uet_worker_channel_runtime_vectors_free (uet_worker_channel_t *channel)
{
  vec_free (channel->rx_buffer_indices);
  vec_free (channel->rx_ids);
  vec_free (channel->rx_free_slots);
}

static void
uet_worker_channel_outstanding_rx_free (uet_worker_t *uw, uet_worker_channel_t *channel)
{
  vlib_main_t *worker_vm = vlib_get_main_by_index (uw->thread_index);
  u32 reclaimed = 0;

  for (u32 token = 0; token < channel->dma_slot_count; token++)
    if (channel->rx_ids[token] && channel->rx_buffer_indices[token] != ~0)
      {
	vlib_buffer_free_one (worker_vm, channel->rx_buffer_indices[token]);
	reclaimed++;
      }
  if (reclaimed)
    uet_log_warn ("reclaimed %u RX buffers from dead external owner %u on worker %u", reclaimed,
		  channel->client_index, uw->thread_index);
  ASSERT (uw->rx_outstanding >= channel->rx_outstanding);
  uw->rx_outstanding -= channel->rx_outstanding;
}

static void
uet_worker_channel_clear (uet_worker_channel_t *channel)
{
  clib_memset (channel, 0, sizeof (*channel));
}

static int
uet_svm_segment_lock (uet_client_t *client, int *lock_fd)
{
  /* Keep new clients out from the readiness transition through shm_unlink. */
  *lock_fd = shm_open ((char *) client->segment.name, O_RDWR, 0);
  if (*lock_fd < 0 || flock (*lock_fd, LOCK_EX | LOCK_NB) < 0)
    {
      if (*lock_fd >= 0)
	close (*lock_fd);
      *lock_fd = -1;
      return VNET_API_ERROR_INSTANCE_IN_USE;
    }
  return 0;
}

int
uet_svm_create (const char *segment_name, u32 queue_depth)
{
  uet_main_t *um = &uet_main;
  uet_client_t *client = 0;
  uword tx_ring_size, tx_completion_ring_size, rx_ring_size, rx_release_ring_size;
  uword control_request_ring_size, control_completion_ring_size;
  vlib_buffer_pool_t *buffer_pool;
  vlib_physmem_map_t *physmem_map;
  u32 worker_count = vlib_num_workers ();
  u32 client_index = ~0;
  u8 selected_pool_index = (u8) ~0;
  u8 worker_vectors_prepared = 0;
  clib_error_t *error;
  void *oldheap;
  int rv;

  if (!worker_count)
    return VNET_API_ERROR_INVALID_WORKER;
  if (!uet_svm_name_is_valid (segment_name) || queue_depth < UET_VPP_SVM_MIN_QUEUE_DEPTH ||
      queue_depth > UET_VPP_SVM_MAX_QUEUE_DEPTH)
    return VNET_API_ERROR_INVALID_VALUE;
  if (uet_client_find (um, segment_name))
    return VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
  if (um->next_client_namespace == UET_VPP_SVM_CLIENT_NAMESPACE_MAX)
    return VNET_API_ERROR_LIMIT_EXCEEDED;

  tx_ring_size =
    sizeof (uet_vpp_svm_spsc_ring_t) + (uword) queue_depth * sizeof (uet_vpp_svm_tx_desc_t);
  tx_completion_ring_size =
    sizeof (uet_vpp_svm_spsc_ring_t) + (uword) queue_depth * sizeof (uet_vpp_svm_tx_completion_t);
  rx_ring_size =
    sizeof (uet_vpp_svm_spsc_ring_t) + (uword) queue_depth * sizeof (uet_vpp_svm_rx_desc_t);
  rx_release_ring_size =
    sizeof (uet_vpp_svm_spsc_ring_t) + (uword) queue_depth * sizeof (uet_vpp_svm_rx_release_t);
  control_request_ring_size =
    sizeof (uet_vpp_svm_spsc_ring_t) + (uword) queue_depth * sizeof (uet_vpp_svm_control_request_t);
  control_completion_ring_size = sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uword) queue_depth * sizeof (uet_vpp_svm_control_completion_t);

  if ((error = uet_dma_listener_init ()))
    {
      clib_error_free (error);
      return VNET_API_ERROR_SYSCALL_ERROR_1;
    }

  for (u32 worker = 0; worker < worker_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      vlib_main_t *worker_vm = vlib_get_main_by_index (thread_index);
      u8 pool_index = vlib_buffer_pool_get_default_for_numa (um->vlib_main, worker_vm->numa_node);

      if (worker == 0)
	selected_pool_index = pool_index;
      else if (pool_index != selected_pool_index)
	return VNET_API_ERROR_INVALID_WORKER;
    }
  if (uet_client_count (um) && selected_pool_index != um->dma_buffer_pool_index)
    return VNET_API_ERROR_INVALID_WORKER;
  um->dma_buffer_pool_index = selected_pool_index;
  buffer_pool = vlib_get_buffer_pool (um->vlib_main, um->dma_buffer_pool_index);
  physmem_map = vlib_physmem_get_map (um->vlib_main, buffer_pool->physmem_map_index);
  um->dma_buffer_data_size = buffer_pool->data_size;
  um->dma_map_size = (u64) physmem_map->n_pages << physmem_map->log2_page_size;
  um->dma_map_base = physmem_map->base;

  um->svm_generation++;
  pool_get_zero (um->clients, client);
  client_index = client - um->clients;
  client->index = client_index;
  client->channel_count = worker_count;
  client->queue_depth = queue_depth;
  client->generation = um->svm_generation;
  client->client_namespace = ++um->next_client_namespace;

  client->segment.name = format (0, "%s%c", segment_name, 0);
  client->segment.ssvm_size = (uword) UET_SVM_SEGMENT_BASE_SIZE * worker_count;
  client->segment.my_pid = getpid ();
  if ((rv = ssvm_server_init (&client->segment, SSVM_SEGMENT_SHM)))
    {
      vec_free (client->segment.name);
      clib_memset (&client->segment, 0, sizeof (client->segment));
      goto create_failed;
    }

  oldheap = ssvm_push_heap (client->segment.sh);
  client->header = clib_mem_alloc_aligned (sizeof (*client->header), UET_VPP_SVM_SHARED_ALIGNMENT);
  if (client->header)
    client->shared_channels = clib_mem_alloc_aligned (
      worker_count * sizeof (*client->shared_channels), UET_VPP_SVM_SHARED_ALIGNMENT);
  if (client->shared_channels)
    client->control_request_ring =
      clib_mem_alloc_aligned (control_request_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
  if (client->control_request_ring)
    client->control_completion_ring =
      clib_mem_alloc_aligned (control_completion_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
  ssvm_pop_heap (oldheap);
  if (!client->header || !client->shared_channels || !client->control_request_ring ||
      !client->control_completion_ring)
    goto create_failed;

  clib_memset (client->header, 0, sizeof (*client->header));
  clib_memset (client->shared_channels, 0, worker_count * sizeof (*client->shared_channels));
  clib_memset (client->control_request_ring, 0, control_request_ring_size);
  clib_memset (client->control_completion_ring, 0, control_completion_ring_size);
  client->control_requests = (uet_vpp_svm_control_request_t *) (client->control_request_ring + 1);
  client->control_completions =
    (uet_vpp_svm_control_completion_t *) (client->control_completion_ring + 1);
  client->header->magic = UET_VPP_SVM_ABI_MAGIC;
  client->header->abi_major = UET_VPP_SVM_ABI_MAJOR;
  client->header->abi_minor = UET_VPP_SVM_ABI_MINOR;
  client->header->header_size = sizeof (*client->header);
  client->header->capabilities = UET_VPP_SVM_REQUIRED_CAPABILITIES;
  client->header->queue_depth = queue_depth;
  client->header->worker_count = worker_count;
  client->header->segment_size = client->segment.ssvm_size;
  client->header->generation = client->generation;
  client->header->worker_channel_table_offset =
    (u8 *) client->shared_channels - (u8 *) client->segment.sh;
  client->header->dma_map_size = um->dma_map_size;
  client->header->dma_buffer_data_size = um->dma_buffer_data_size;
  client->header->dma_buffer_pool_index = um->dma_buffer_pool_index;
  client->header->worker_channel_desc_size = sizeof (*client->shared_channels);
  client->header->tx_desc_size = sizeof (uet_vpp_svm_tx_desc_t);
  client->header->tx_completion_size = sizeof (uet_vpp_svm_tx_completion_t);
  client->header->rx_desc_size = sizeof (uet_vpp_svm_rx_desc_t);
  client->header->rx_release_size = sizeof (uet_vpp_svm_rx_release_t);
  client->header->client_namespace = client->client_namespace;
  client->header->control_ring_size = queue_depth;
  client->header->control_request_ring_offset =
    (u8 *) client->control_request_ring - (u8 *) client->segment.sh;
  client->header->control_completion_ring_offset =
    (u8 *) client->control_completion_ring - (u8 *) client->segment.sh;
  client->control_request_ring->size = queue_depth;
  client->control_request_ring->mask = (queue_depth & (queue_depth - 1)) ? 0 : queue_depth - 1;
  client->control_completion_ring->size = queue_depth;
  client->control_completion_ring->mask = client->control_request_ring->mask;

  /* Reserve the stable pool index in every worker vector while workers are
   * stopped. The entries remain inactive until all shared state is ready.
   */
  vlib_worker_thread_barrier_sync (um->vlib_main);
  for (u32 worker = 0; worker < worker_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

      vec_validate (uw->channels, client_index);
      uet_worker_channel_clear (uw->channels + client_index);
      uw->channels[client_index].client_index = client_index;
    }
  vlib_worker_thread_barrier_release (um->vlib_main);
  worker_vectors_prepared = 1;

  for (u32 worker = 0; worker < worker_count; worker++)
    {
      uet_vpp_svm_dma_slot_t *shared_dma_slots = 0;
      uet_vpp_svm_spsc_ring_t *shared_tx_ring = 0;
      uet_vpp_svm_spsc_ring_t *shared_tx_completion_ring = 0;
      uet_vpp_svm_spsc_ring_t *shared_rx_ring = 0;
      uet_vpp_svm_spsc_ring_t *shared_rx_release_ring = 0;
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
      uet_worker_channel_t *channel = uw->channels + client_index;
      uet_vpp_svm_worker_channel_t *shared_channel = client->shared_channels + worker;
      u32 n_dma_alloc;

      vec_validate (channel->dma_buffer_indices, queue_depth - 1);
      n_dma_alloc = vlib_buffer_alloc_from_pool (um->vlib_main, channel->dma_buffer_indices,
						 queue_depth, um->dma_buffer_pool_index);
      if (n_dma_alloc != queue_depth)
	{
	  if (n_dma_alloc)
	    vlib_buffer_free (um->vlib_main, channel->dma_buffer_indices, n_dma_alloc);
	  vec_free (channel->dma_buffer_indices);
	  goto create_failed;
	}
      oldheap = ssvm_push_heap (client->segment.sh);
      shared_dma_slots = clib_mem_alloc_aligned (queue_depth * sizeof (*shared_dma_slots),
						 UET_VPP_SVM_SHARED_ALIGNMENT);
      if (shared_dma_slots)
	shared_tx_ring = clib_mem_alloc_aligned (tx_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
      if (shared_tx_ring)
	shared_tx_completion_ring =
	  clib_mem_alloc_aligned (tx_completion_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
      if (shared_tx_completion_ring)
	shared_rx_ring = clib_mem_alloc_aligned (rx_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
      if (shared_rx_ring)
	shared_rx_release_ring =
	  clib_mem_alloc_aligned (rx_release_ring_size, UET_VPP_SVM_SHARED_ALIGNMENT);
      ssvm_pop_heap (oldheap);

      if (!shared_dma_slots || !shared_tx_ring || !shared_tx_completion_ring || !shared_rx_ring ||
	  !shared_rx_release_ring)
	goto create_failed;

      clib_memset (shared_dma_slots, 0, queue_depth * sizeof (*shared_dma_slots));
      clib_memset (shared_tx_ring, 0, tx_ring_size);
      clib_memset (shared_tx_completion_ring, 0, tx_completion_ring_size);
      clib_memset (shared_rx_ring, 0, rx_ring_size);
      clib_memset (shared_rx_release_ring, 0, rx_release_ring_size);
      shared_channel->worker_index = worker;
      shared_channel->thread_index = thread_index;
      shared_channel->dma_slot_table_offset = (u8 *) shared_dma_slots - (u8 *) client->segment.sh;
      shared_channel->dma_slot_count = queue_depth;
      shared_channel->tx_ring_offset = (u8 *) shared_tx_ring - (u8 *) client->segment.sh;
      shared_channel->tx_completion_ring_offset =
	(u8 *) shared_tx_completion_ring - (u8 *) client->segment.sh;
      shared_channel->tx_ring_size = queue_depth;
      shared_channel->rx_ring_offset = (u8 *) shared_rx_ring - (u8 *) client->segment.sh;
      shared_channel->rx_release_ring_offset =
	(u8 *) shared_rx_release_ring - (u8 *) client->segment.sh;
      shared_channel->rx_ring_size = queue_depth;

      shared_tx_ring->size = queue_depth;
      shared_tx_ring->mask = (queue_depth & (queue_depth - 1)) ? 0 : queue_depth - 1;
      shared_tx_completion_ring->size = queue_depth;
      shared_tx_completion_ring->mask = shared_tx_ring->mask;
      shared_rx_ring->size = queue_depth;
      shared_rx_ring->mask = shared_tx_ring->mask;
      shared_rx_release_ring->size = queue_depth;
      shared_rx_release_ring->mask = shared_tx_ring->mask;

      for (u32 i = 0; i < queue_depth; i++)
	{
	  vlib_buffer_t *buffer = vlib_get_buffer (um->vlib_main, channel->dma_buffer_indices[i]);

	  shared_dma_slots[i].data_offset = (u8 *) buffer->data - (u8 *) physmem_map->base;
	  shared_dma_slots[i].capacity = buffer_pool->data_size;
	}

      channel->dma_slots = shared_dma_slots;
      channel->svm_header = client->header;
      channel->tx_ring = shared_tx_ring;
      channel->tx_descs = (uet_vpp_svm_tx_desc_t *) (shared_tx_ring + 1);
      channel->tx_completion_ring = shared_tx_completion_ring;
      channel->tx_completion_entries =
	(uet_vpp_svm_tx_completion_t *) (shared_tx_completion_ring + 1);
      channel->rx_ring = shared_rx_ring;
      channel->rx_descs = (uet_vpp_svm_rx_desc_t *) (shared_rx_ring + 1);
      channel->rx_release_ring = shared_rx_release_ring;
      channel->rx_release_entries = (uet_vpp_svm_rx_release_t *) (shared_rx_release_ring + 1);
      channel->control_request_ring = client->control_request_ring;
      channel->control_requests = client->control_requests;
      channel->control_completion_ring = client->control_completion_ring;
      channel->control_completions = client->control_completions;
      channel->client_namespace = client->client_namespace;

      vec_validate (channel->rx_buffer_indices, queue_depth - 1);
      vec_validate (channel->rx_ids, queue_depth - 1);
      vec_validate (channel->rx_free_slots, queue_depth - 1);
      clib_memset (channel->rx_ids, 0, queue_depth * sizeof (*channel->rx_ids));
      for (u32 i = 0; i < queue_depth; i++)
	{
	  channel->rx_buffer_indices[i] = ~0;
	  channel->rx_free_slots[i] = i;
	}
      channel->dma_slot_count = queue_depth;
      channel->rx_free_count = queue_depth;
      channel->next_rx_id = (client->generation << 32) | ((u64) worker << 24) | 1;
    }

  client->segment.sh->opaque[0] = (void *) ((u8 *) client->header - (u8 *) client->segment.sh);

  vlib_worker_thread_barrier_sync (um->vlib_main);
  for (u32 worker = 0; worker < worker_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
      uet_worker_channel_t *channel = uw->channels + client_index;

      uw->thread_index = thread_index;
      channel->active = 1;
    }
  um->client_by_namespace[client->client_namespace] = client_index;
  vlib_worker_thread_barrier_release (um->vlib_main);

  clib_atomic_store_rel_n (&client->segment.sh->ready, 1);
  uet_log_notice ("created application segment %s generation %llu with %u worker channels, "
		  "queue depth %u and DMA map size %llu",
		  segment_name, client->generation, worker_count, queue_depth, um->dma_map_size);
  return 0;

create_failed:
  if (worker_vectors_prepared)
    {
      vlib_worker_thread_barrier_sync (um->vlib_main);
      for (u32 worker = 0; worker < worker_count; worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

	  uw->channels[client_index].active = 0;
	}
      vlib_worker_thread_barrier_release (um->vlib_main);
      for (u32 worker = 0; worker < worker_count; worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
	  uet_worker_channel_t *channel = uw->channels + client_index;

	  uet_worker_channel_runtime_vectors_free (channel);
	  uet_worker_channel_dma_buffers_free (um, channel);
	  uet_worker_channel_clear (channel);
	}
    }
  if (client->segment.sh)
    ssvm_delete (&client->segment);
  else
    vec_free (client->segment.name);
  ASSERT (client->client_namespace == um->next_client_namespace);
  um->next_client_namespace--;
  pool_put (um->clients, client);
  uet_dma_mapping_reset_if_unused (um);
  uet_log_warn ("failed to create worker channels for generation %llu", um->svm_generation);
  return VNET_API_ERROR_SVM_SEGMENT_CREATE_FAIL;
}

int
uet_tx_set_fib_tables (u32 ip4_table_id, u32 ip6_table_id)
{
  uet_main_t *um = &uet_main;
  u32 old_ip4_fib_index = um->tx_ip4_fib_index;
  u32 old_ip6_fib_index = um->tx_ip6_fib_index;
  u32 ip4_fib_index, ip6_fib_index;

  if (!vlib_num_workers ())
    return VNET_API_ERROR_INVALID_WORKER;

  ip4_fib_index = fib_table_find (FIB_PROTOCOL_IP4, ip4_table_id);
  ip6_fib_index = fib_table_find (FIB_PROTOCOL_IP6, ip6_table_id);
  if (ip4_fib_index == ~0 || ip6_fib_index == ~0)
    return VNET_API_ERROR_NO_SUCH_FIB;

  if (!um->tx_configured || ip4_fib_index != old_ip4_fib_index)
    fib_table_lock (ip4_fib_index, FIB_PROTOCOL_IP4, um->fib_source);
  if (!um->tx_configured || ip6_fib_index != old_ip6_fib_index)
    fib_table_lock (ip6_fib_index, FIB_PROTOCOL_IP6, um->fib_source);

  vlib_worker_thread_barrier_sync (um->vlib_main);
  for (u32 worker = 0; worker < vlib_num_workers (); worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

      uw->tx_ip4_fib_index = ip4_fib_index;
      uw->tx_ip6_fib_index = ip6_fib_index;
      uw->tx_ip4_table_id = ip4_table_id;
      uw->tx_ip6_table_id = ip6_table_id;
      uw->tx_configured = 1;
    }
  um->tx_ip4_fib_index = ip4_fib_index;
  um->tx_ip6_fib_index = ip6_fib_index;
  um->tx_ip4_table_id = ip4_table_id;
  um->tx_ip6_table_id = ip6_table_id;
  um->tx_configured = 1;
  vlib_worker_thread_barrier_release (um->vlib_main);

  if (old_ip4_fib_index != ~0 && old_ip4_fib_index != ip4_fib_index)
    fib_table_unlock (old_ip4_fib_index, FIB_PROTOCOL_IP4, um->fib_source);
  if (old_ip6_fib_index != ~0 && old_ip6_fib_index != ip6_fib_index)
    fib_table_unlock (old_ip6_fib_index, FIB_PROTOCOL_IP6, um->fib_source);

  uet_log_notice ("TX routing uses IPv4 table %u and IPv6 table %u", ip4_table_id, ip6_table_id);
  return 0;
}

static int
uet_protocols_register (vlib_main_t *vm)
{
  uet_main_t *um = &uet_main;

  if (um->protocols_registered)
    return 0;
  if (ip4_main.lookup_main.local_next_by_ip_protocol[UET_IP_PROTOCOL] != IP_LOCAL_NEXT_PUNT ||
      ip6_main.lookup_main.local_next_by_ip_protocol[UET_IP_PROTOCOL] != IP_LOCAL_NEXT_PUNT ||
      udp_is_valid_dst_port (UET_UDP_PORT, 1 /* is_ip4 */) ||
      udp_is_valid_dst_port (UET_UDP_PORT, 0 /* is_ip4 */))
    return VNET_API_ERROR_ADDRESS_IN_USE;

  ip4_register_protocol (UET_IP_PROTOCOL, uet4_ip_input_node.index);
  ip6_register_protocol (UET_IP_PROTOCOL, uet6_ip_input_node.index);
  udp_register_dst_port (vm, UET_UDP_PORT, uet4_udp_input_node.index, 1 /* is_ip4 */);
  udp_register_dst_port (vm, UET_UDP_PORT, uet6_udp_input_node.index, 0 /* is_ip4 */);
  um->protocols_registered = 1;
  uet_log_debug ("registered IP protocol %u and UDP destination port %u", UET_IP_PROTOCOL,
		 UET_UDP_PORT);
  return 0;
}

static void
uet_protocols_unregister (vlib_main_t *vm)
{
  uet_main_t *um = &uet_main;

  if (!um->protocols_registered)
    return;

  udp_unregister_dst_port (vm, UET_UDP_PORT, 1 /* is_ip4 */);
  udp_unregister_dst_port (vm, UET_UDP_PORT, 0 /* is_ip4 */);
  ip4_unregister_protocol (UET_IP_PROTOCOL);
  ip6_unregister_protocol (UET_IP_PROTOCOL);
  um->protocols_registered = 0;
  uet_log_debug ("unregistered IP protocol %u and UDP destination port %u", UET_IP_PROTOCOL,
		 UET_UDP_PORT);
}

typedef struct
{
  uet_main_t *um;
  u32 client_namespace;
} uet_endpoint_cleanup_ctx_t;

static int
uet_endpoint_client_cleanup_cb (clib_bihash_kv_40_8_t *kv, void *arg)
{
  uet_endpoint_cleanup_ctx_t *ctx = arg;

  if (kv->value == ctx->client_namespace)
    {
      clib_bihash_kv_40_8_t copy = *kv;

      if (!clib_bihash_add_del_40_8 (&ctx->um->endpoint_hash, &copy, 0))
	{
	  ASSERT (ctx->um->endpoint_registrations > 0);
	  ctx->um->endpoint_registrations--;
	}
    }
  return BIHASH_WALK_CONTINUE;
}

static void
uet_endpoint_client_cleanup (uet_client_t *client)
{
  uet_endpoint_cleanup_ctx_t ctx = {
    .um = &uet_main,
    .client_namespace = client->client_namespace,
  };

  clib_bihash_foreach_key_value_pair_40_8 (&ctx.um->endpoint_hash, uet_endpoint_client_cleanup_cb,
					   &ctx);
}

int
uet_svm_delete (const char *segment_name)
{
  uet_main_t *um = &uet_main;
  uet_client_t *client = 0;
  u32 client_index;
  int lock_fd = -1;
  int rv;

  if (segment_name && segment_name[0])
    client = uet_client_find (um, segment_name);
  else if (uet_client_count (um) == 1)
    pool_foreach (client, um->clients)
      break;
  else if (uet_client_count (um) > 1)
    return VNET_API_ERROR_INVALID_VALUE;
  if (!client)
    return VNET_API_ERROR_NO_SUCH_ENTRY;
  client_index = client->index;

  rv = uet_svm_segment_lock (client, &lock_fd);
  if (rv)
    return rv;

  clib_atomic_store_rel_n (&client->segment.sh->ready, 0);

  vlib_worker_thread_barrier_sync (um->vlib_main);
  for (u32 worker = 0; worker < client->channel_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
      uet_worker_channel_t *channel = uw->channels + client_index;

      channel->active = 0;
      uet_worker_channel_outstanding_rx_free (uw, channel);
      uet_worker_channel_runtime_vectors_free (channel);
    }
  uet_endpoint_client_cleanup (client);
  um->client_by_namespace[client->client_namespace] = UET_INVALID_CLIENT_INDEX;
  vlib_worker_thread_barrier_release (um->vlib_main);

  for (u32 worker = 0; worker < client->channel_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
      uet_worker_channel_t *channel = uw->channels + client_index;

      uet_worker_channel_dma_buffers_free (um, channel);
      uet_worker_channel_clear (channel);
    }
  uet_log_notice ("deleting application segment %s generation %llu", client->segment.name,
		  client->generation);
  ssvm_delete (&client->segment);
  close (lock_fd);
  pool_put (um->clients, client);
  uet_dma_mapping_reset_if_unused (um);
  return 0;
}

int
uet_enable_disable (u8 enable)
{
  uet_main_t *um = &uet_main;
  vlib_main_t *vm = um->vlib_main;

  if (!vlib_num_workers ())
    return VNET_API_ERROR_INVALID_WORKER;

  if (um->enabled == !!enable)
    return 0;

  if (enable)
    {
      int rv;

      if (!um->tx_configured)
	{
	  rv = uet_tx_set_fib_tables (0, 0);
	  if (rv)
	    return rv;
	}

      rv = uet_protocols_register (vm);

      if (rv)
	return rv;
    }

  vlib_worker_thread_barrier_sync (vm);

  if (enable)
    {
      for (u32 worker = 0; worker < vlib_num_workers (); worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
	  vlib_main_t *worker_vm = vlib_get_main_by_index (thread_index);

	  uw->thread_index = thread_index;
	  uet_worker_counters_clear (uw);
	  vlib_node_set_state (worker_vm, uet_input_node.index, VLIB_NODE_STATE_POLLING);
	}
      um->enabled = 1;
    }
  else
    {
      for (u32 worker = 0; worker < vlib_num_workers (); worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

	  if (uw->rx_outstanding)
	    {
	      vlib_worker_thread_barrier_release (vm);
	      return VNET_API_ERROR_INSTANCE_IN_USE;
	    }
	  for (u32 client_index = 0; client_index < vec_len (uw->channels); client_index++)
	    {
	      uet_worker_channel_t *channel = uw->channels + client_index;

	      if (channel->active && (clib_atomic_load_acq_n (&channel->svm_header->client_flags) &
				      UET_VPP_SVM_CLIENT_F_DMA_READY))
		{
		  vlib_worker_thread_barrier_release (vm);
		  return VNET_API_ERROR_INSTANCE_IN_USE;
		}
	    }
	}
      for (u32 worker = 0; worker < vlib_num_workers (); worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  vlib_main_t *worker_vm = vlib_get_main_by_index (thread_index);

	  vlib_node_set_state (worker_vm, uet_input_node.index, VLIB_NODE_STATE_DISABLED);
	}
      um->enabled = 0;
      uet_protocols_unregister (vm);
    }

  vlib_worker_thread_barrier_release (vm);
  uet_log_notice ("dataplane %s with %u workers", enable ? "enabled" : "disabled",
		  vlib_num_workers ());
  return 0;
}

static clib_error_t *
uet_enable_disable_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  u8 enable = 1;
  int rv;

  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "disable"))
	enable = 0;
      else if (!unformat (input, "enable"))
	return clib_error_return (0, "unknown input `%U'", format_unformat_error, input);

      if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
	return clib_error_return (0, "unexpected input `%U'", format_unformat_error, input);
    }

  rv = uet_enable_disable (enable);
  if (rv == VNET_API_ERROR_INVALID_WORKER)
    return clib_error_return (0, "UET requires at least one VPP worker");
  if (rv)
    return clib_error_return (0, "UET enable/disable failed: %U", format_vnet_api_errno, rv);

  return 0;
}

VLIB_CLI_COMMAND (uet_enable_disable_command, static) = {
  .path = "uet",
  .short_help = "uet [enable|disable]",
  .function = uet_enable_disable_command_fn,
};

static clib_error_t *
uet_svm_create_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  u32 queue_depth = UET_VPP_SVM_DEFAULT_QUEUE_DEPTH;
  u8 *segment_name = 0;
  int rv;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "name %s", &segment_name))
	;
      else if (unformat (input, "queue-size %u", &queue_depth))
	;
      else
	{
	  vec_free (segment_name);
	  return clib_error_return (0, "unknown input `%U'", format_unformat_error, input);
	}
    }

  if (!segment_name)
    return clib_error_return (0, "segment name is required");

  vec_add1 (segment_name, 0);
  rv = uet_svm_create ((char *) segment_name, queue_depth);
  vec_free (segment_name);
  if (rv)
    return clib_error_return (0, "UET SVM create failed: %U", format_vnet_api_errno, rv);
  return 0;
}

VLIB_CLI_COMMAND (uet_svm_create_command, static) = {
  .path = "uet svm create",
  .short_help = "uet svm create name <segment-name> [queue-size <8-4096>]",
  .function = uet_svm_create_command_fn,
};

static clib_error_t *
uet_svm_delete_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  u8 *segment_name = 0;
  int rv;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    if (unformat (input, "name %s", &segment_name))
      ;
    else
      {
	vec_free (segment_name);
	return clib_error_return (0, "unexpected input `%U'", format_unformat_error, input);
      }

  if (segment_name)
    vec_add1 (segment_name, 0);
  rv = uet_svm_delete ((char *) segment_name);
  vec_free (segment_name);
  if (rv)
    return clib_error_return (0, "UET SVM delete failed: %U", format_vnet_api_errno, rv);
  return 0;
}

VLIB_CLI_COMMAND (uet_svm_delete_command, static) = {
  .path = "uet svm delete",
  .short_help = "uet svm delete [name <segment-name>]",
  .function = uet_svm_delete_command_fn,
};

static clib_error_t *
uet_tx_fib_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  u32 ip4_table_id = ~0, ip6_table_id = ~0, table_id = ~0;
  int rv;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "table %u", &table_id))
	;
      else if (unformat (input, "ip4-table %u", &ip4_table_id))
	;
      else if (unformat (input, "ip6-table %u", &ip6_table_id))
	;
      else
	return clib_error_return (0, "unknown input `%U'", format_unformat_error, input);
    }

  if (table_id != ~0)
    {
      if (ip4_table_id != ~0 || ip6_table_id != ~0)
	return clib_error_return (0, "use either table or both ip4-table and ip6-table");
      ip4_table_id = table_id;
      ip6_table_id = table_id;
    }
  if (ip4_table_id == ~0 || ip6_table_id == ~0)
    return clib_error_return (0, "table or both ip4-table and ip6-table are required");

  rv = uet_tx_set_fib_tables (ip4_table_id, ip6_table_id);
  if (rv)
    return clib_error_return (0, "UET TX FIB configuration failed: %U", format_vnet_api_errno, rv);
  return 0;
}

VLIB_CLI_COMMAND (uet_tx_fib_command, static) = {
  .path = "uet tx fib",
  .short_help = "uet tx fib table <table-id> | ip4-table <table-id> "
		"ip6-table <table-id>",
  .function = uet_tx_fib_command_fn,
};

static clib_error_t *
uet_rx_placement_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  u8 entropy_handoff;

  if (unformat (input, "current-worker"))
    entropy_handoff = 0;
  else if (unformat (input, "entropy-handoff"))
    entropy_handoff = 1;
  else
    return clib_error_return (0, "expected current-worker or entropy-handoff");
  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    return clib_error_return (0, "unexpected input `%U'", format_unformat_error, input);

  vlib_worker_thread_barrier_sync (vm);
  uet_main.rx_entropy_handoff = entropy_handoff;
  vlib_worker_thread_barrier_release (vm);
  uet_log_notice ("RX placement uses %s",
		  entropy_handoff ? "entropy handoff" : "the current worker");
  return 0;
}

VLIB_CLI_COMMAND (uet_rx_placement_command, static) = {
  .path = "uet rx placement",
  .short_help = "uet rx placement current-worker|entropy-handoff",
  .function = uet_rx_placement_command_fn,
};

typedef struct
{
  u32 thread_index;
  u8 svm_attached;
  u64 tx_requests;
  u64 tx_packets;
  u64 rx_delivered;
  u64 rx_ring_full;
} uet_cli_worker_snapshot_t;

static clib_error_t *
show_uet_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  uet_main_t *um = &uet_main;
  u64 poll_calls = 0, tx_requests = 0;
  u64 invalid_requests = 0, tx_completion_ring_full = 0;
  u64 tx_packets = 0, tx_bytes = 0, tx_completions = 0;
  u32 tx_pending = 0, tx_completions_pending = 0;
  u64 rx_ip4_packets = 0, rx_ip4_bytes = 0, rx_ip6_packets = 0, rx_ip6_bytes = 0;
  u64 rx_udp4_packets = 0, rx_udp4_bytes = 0, rx_udp6_packets = 0, rx_udp6_bytes = 0;
  u64 rx_delivered = 0, rx_ambiguous = 0, rx_ring_full = 0, rx_bad_chain = 0, rx_releases = 0;
  u64 rx_invalid_releases = 0, rx_outstanding = 0;
  u64 endpoint_registrations = 0, endpoint_collisions = 0;
  u32 tx_ip4_table_id = ~0, tx_ip6_table_id = ~0;
  u32 rx_pending = 0, rx_release_pending = 0;
  u32 svm_attached = 0;
  u8 tx_configured = 0;
  u8 provider_ready = 0;
  u32 owner_pid = 0;
  u32 worker_count = vlib_num_workers ();
  u32 client_count = uet_client_count (um);
  uet_vpp_svm_shared_header_t *first_header = 0;
  uet_client_t *first_client = 0;
  uet_cli_worker_snapshot_t *worker_snapshots = 0;

  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    return clib_error_return (0, "unexpected input `%U'", format_unformat_error, input);

  if (worker_count && um->workers)
    {
      vec_validate (worker_snapshots, worker_count - 1);
      vlib_worker_thread_barrier_sync (vm);
      for (u32 worker = 0; worker < worker_count; worker++)
	{
	  u32 thread_index = vlib_get_worker_thread_index (worker);
	  uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);
	  uet_cli_worker_snapshot_t *snapshot = vec_elt_at_index (worker_snapshots, worker);

	  snapshot->thread_index = thread_index;
	  snapshot->svm_attached = 0;
	  snapshot->tx_requests = uw->tx_requests;
	  snapshot->tx_packets = uw->tx_packets;
	  snapshot->rx_delivered = uw->rx_delivered;
	  snapshot->rx_ring_full = uw->rx_ring_full;
	  poll_calls += uw->poll_calls;
	  tx_requests += uw->tx_requests;
	  invalid_requests += uw->invalid_requests;
	  tx_completion_ring_full += uw->tx_completion_ring_full;
	  tx_packets += uw->tx_packets;
	  tx_bytes += uw->tx_bytes;
	  tx_completions += uw->tx_completions;
	  rx_ip4_packets += uw->rx_ip4_packets;
	  rx_ip4_bytes += uw->rx_ip4_bytes;
	  rx_ip6_packets += uw->rx_ip6_packets;
	  rx_ip6_bytes += uw->rx_ip6_bytes;
	  rx_udp4_packets += uw->rx_udp4_packets;
	  rx_udp4_bytes += uw->rx_udp4_bytes;
	  rx_udp6_packets += uw->rx_udp6_packets;
	  rx_udp6_bytes += uw->rx_udp6_bytes;
	  rx_delivered += uw->rx_delivered;
	  rx_ambiguous += uw->rx_ambiguous;
	  rx_ring_full += uw->rx_ring_full;
	  rx_bad_chain += uw->rx_bad_chain;
	  rx_releases += uw->rx_releases;
	  rx_invalid_releases += uw->rx_invalid_releases;
	  rx_outstanding += uw->rx_outstanding;
	  if (worker == 0)
	    {
	      tx_configured = uw->tx_configured;
	      tx_ip4_table_id = uw->tx_ip4_table_id;
	      tx_ip6_table_id = uw->tx_ip6_table_id;
	    }
	  else
	    tx_configured &= uw->tx_configured;
	  for (u32 client_index = 0; client_index < vec_len (uw->channels); client_index++)
	    {
	      uet_worker_channel_t *channel = uw->channels + client_index;
	      u32 client_flags, server_flags;

	      if (!channel->active)
		continue;
	      client_flags = clib_atomic_load_acq_n (&channel->svm_header->client_flags);
	      server_flags = clib_atomic_load_acq_n (&channel->svm_header->server_flags);
	      snapshot->svm_attached = 1;
	      if (!first_header)
		{
		  first_header = channel->svm_header;
		  first_client = pool_elt_at_index (um->clients, client_index);
		  owner_pid = clib_atomic_load_acq_n (&channel->svm_header->owner_pid);
		  provider_ready = !!(client_flags & UET_VPP_SVM_CLIENT_F_DMA_READY) &&
				   !!(server_flags & UET_VPP_SVM_SERVER_F_DMA_READY_ACK);
		}
	      svm_attached++;
	      tx_pending += clib_atomic_load_acq_n (&channel->tx_ring->producer) -
			    clib_atomic_load_acq_n (&channel->tx_ring->consumer);
	      tx_completions_pending +=
		clib_atomic_load_acq_n (&channel->tx_completion_ring->producer) -
		clib_atomic_load_acq_n (&channel->tx_completion_ring->consumer);
	      rx_pending += clib_atomic_load_acq_n (&channel->rx_ring->producer) -
			    clib_atomic_load_acq_n (&channel->rx_ring->consumer);
	      rx_release_pending += clib_atomic_load_acq_n (&channel->rx_release_ring->producer) -
				    clib_atomic_load_acq_n (&channel->rx_release_ring->consumer);
	    }
	  endpoint_registrations = um->endpoint_registrations;
	  endpoint_collisions = um->endpoint_collisions;
	}
      vlib_worker_thread_barrier_release (vm);
    }

  vlib_cli_output (vm, "state %s", um->enabled ? "enabled" : "disabled");
  vlib_cli_output (vm, "main-thread 0");
  vlib_cli_output (vm, "workers %u", worker_count);
  vlib_cli_output (vm, "rx-placement %s",
		   um->rx_entropy_handoff ? "entropy-handoff" : "current-worker");
  if (worker_count == 1)
    vlib_cli_output (vm, "owner-worker %U", format_vlib_thread_name_and_index,
		     vlib_get_worker_thread_index (0));
  else
    vlib_cli_output (vm, "owner-workers %u", worker_count);
  vlib_cli_output (vm, "poll-calls %llu", poll_calls);
  vlib_cli_output (vm, "tx-requests %llu", tx_requests);
  vlib_cli_output (vm, "invalid-requests %llu", invalid_requests);
  vlib_cli_output (vm, "tx-completion-ring-full %llu", tx_completion_ring_full);
  if (tx_configured)
    {
      vlib_cli_output (vm, "tx-ip4-table %u", tx_ip4_table_id);
      vlib_cli_output (vm, "tx-ip6-table %u", tx_ip6_table_id);
    }
  else
    vlib_cli_output (vm, "tx-fib unconfigured");
  vlib_cli_output (vm, "tx-packets %llu", tx_packets);
  vlib_cli_output (vm, "tx-bytes %llu", tx_bytes);
  vlib_cli_output (vm, "tx-completions %llu", tx_completions);
  vlib_cli_output (vm, "rx-ip4-packets %llu", rx_ip4_packets);
  vlib_cli_output (vm, "rx-ip4-bytes %llu", rx_ip4_bytes);
  vlib_cli_output (vm, "rx-ip6-packets %llu", rx_ip6_packets);
  vlib_cli_output (vm, "rx-ip6-bytes %llu", rx_ip6_bytes);
  vlib_cli_output (vm, "rx-udp4-packets %llu", rx_udp4_packets);
  vlib_cli_output (vm, "rx-udp4-bytes %llu", rx_udp4_bytes);
  vlib_cli_output (vm, "rx-udp6-packets %llu", rx_udp6_packets);
  vlib_cli_output (vm, "rx-udp6-bytes %llu", rx_udp6_bytes);
  vlib_cli_output (vm, "rx-delivered %llu", rx_delivered);
  vlib_cli_output (vm, "rx-ambiguous %llu", rx_ambiguous);
  vlib_cli_output (vm, "rx-ring-full %llu", rx_ring_full);
  vlib_cli_output (vm, "rx-bad-chain %llu", rx_bad_chain);
  vlib_cli_output (vm, "rx-releases %llu", rx_releases);
  vlib_cli_output (vm, "rx-invalid-releases %llu", rx_invalid_releases);
  vlib_cli_output (vm, "rx-outstanding %llu", rx_outstanding);
  vlib_cli_output (vm, "endpoint-registrations %llu", endpoint_registrations);
  vlib_cli_output (vm, "endpoint-collisions %llu", endpoint_collisions);
  vlib_cli_output (vm, "dma-authorized-clients %llu", um->dma_authorized_clients);
  vlib_cli_output (vm, "dma-rejected-clients %llu", um->dma_rejected_clients);
  vlib_cli_output (vm, "local-dispatch %s ip-protocol %u udp-port %u",
		   um->protocols_registered ? "registered" : "unregistered", UET_IP_PROTOCOL,
		   UET_UDP_PORT);
  vlib_cli_output (vm, "svm-state %s",
		   svm_attached == worker_count * client_count && worker_count && client_count ?
		     "attached" :
		   svm_attached ? "partial" :
				  "detached");
  vlib_cli_output (vm, "svm-segments %u", client_count);
  if (first_header && first_client && client_count == 1)
    {
      vlib_cli_output (vm, "svm-segment %s", first_client->segment.name);
      vlib_cli_output (vm, "svm-channels %u", first_client->channel_count);
      vlib_cli_output (vm, "svm-abi %u.%u", first_header->abi_major, first_header->abi_minor);
      vlib_cli_output (vm, "svm-generation %llu", first_header->generation);
      vlib_cli_output (vm, "svm-queue-depth %u", first_header->queue_depth);
      vlib_cli_output (vm, "svm-dma-slot-count %u",
		       first_header->queue_depth * first_header->worker_count);
      vlib_cli_output (vm, "svm-dma-buffer-data-size %u", first_header->dma_buffer_data_size);
      vlib_cli_output (vm, "svm-dma-map-size %llu", first_header->dma_map_size);
      vlib_cli_output (vm, "svm-dma-socket %s", um->dma_socket_name);
      vlib_cli_output (vm, "provider-ready %s", provider_ready ? "yes" : "no");
      vlib_cli_output (vm, "provider-owner-pid %u", owner_pid);
      vlib_cli_output (vm, "tx-pending %u", tx_pending);
      vlib_cli_output (vm, "tx-completions-pending %u", tx_completions_pending);
      vlib_cli_output (vm, "rx-pending %u", rx_pending);
      vlib_cli_output (vm, "rx-release-pending %u", rx_release_pending);
    }

  {
    uet_client_t *client;

    pool_foreach (client, um->clients)
      {
	u32 client_flags = clib_atomic_load_acq_n (&client->header->client_flags);
	u32 server_flags = clib_atomic_load_acq_n (&client->header->server_flags);
	u32 client_owner = clib_atomic_load_acq_n (&client->header->owner_pid);
	u8 ready = !!(client_flags & UET_VPP_SVM_CLIENT_F_DMA_READY) &&
		   !!(server_flags & UET_VPP_SVM_SERVER_F_DMA_READY_ACK);

	vlib_cli_output (vm, "application-%u-segment %s", client->index, client->segment.name);
	vlib_cli_output (vm, "application-%u-generation %llu", client->index, client->generation);
	vlib_cli_output (vm, "application-%u-namespace %u", client->index,
			 client->client_namespace);
	vlib_cli_output (vm, "application-%u-channels %u", client->index, client->channel_count);
	vlib_cli_output (vm, "application-%u-queue-depth %u", client->index, client->queue_depth);
	vlib_cli_output (vm, "application-%u-provider-ready %s", client->index,
			 ready ? "yes" : "no");
	vlib_cli_output (vm, "application-%u-pds-mode %s", client->index,
			 client_flags & UET_VPP_SVM_CLIENT_F_PDS_SNG ? "sng" : "pds");
	vlib_cli_output (vm, "application-%u-owner-pid %u", client->index, client_owner);
      }
  }

  for (u32 worker = 0; worker < vec_len (worker_snapshots); worker++)
    {
      uet_cli_worker_snapshot_t *snapshot = vec_elt_at_index (worker_snapshots, worker);

      vlib_cli_output (vm, "worker-%u-thread %U", worker, format_vlib_thread_name_and_index,
		       snapshot->thread_index);
      vlib_cli_output (vm, "worker-%u-channel %s", worker,
		       snapshot->svm_attached ? "attached" : "detached");
      vlib_cli_output (vm, "worker-%u-tx-requests %llu", worker, snapshot->tx_requests);
      vlib_cli_output (vm, "worker-%u-tx-packets %llu", worker, snapshot->tx_packets);
      vlib_cli_output (vm, "worker-%u-rx-delivered %llu", worker, snapshot->rx_delivered);
      vlib_cli_output (vm, "worker-%u-rx-ring-full %llu", worker, snapshot->rx_ring_full);
    }
  vec_free (worker_snapshots);

  return 0;
}

VLIB_CLI_COMMAND (show_uet_command, static) = {
  .path = "show uet",
  .short_help = "show uet",
  .function = show_uet_command_fn,
};

static clib_error_t *
uet_clear_counters_command_fn (vlib_main_t *vm, unformat_input_t *input, vlib_cli_command_t *cmd)
{
  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    return clib_error_return (0, "unexpected input `%U'", format_unformat_error, input);

  uet_counters_clear ();
  uet_log_info ("plugin counters cleared");
  return 0;
}

VLIB_CLI_COMMAND (uet_clear_counters_command, static) = {
  .path = "clear uet counters",
  .short_help = "clear uet counters",
  .function = uet_clear_counters_command_fn,
};

static void
vl_api_uet_enable_disable_t_handler (vl_api_uet_enable_disable_t *mp)
{
  vl_api_uet_enable_disable_reply_t *rmp;
  uet_main_t *um = &uet_main;
  int rv;

  rv = uet_enable_disable (mp->enable);
  REPLY_MACRO (VL_API_UET_ENABLE_DISABLE_REPLY);
}

static void
vl_api_uet_svm_create_t_handler (vl_api_uet_svm_create_t *mp)
{
  vl_api_uet_svm_create_reply_t *rmp;
  uet_main_t *um = &uet_main;
  char segment_name[65];
  int rv;

  clib_memcpy_fast (segment_name, mp->segment_name, 64);
  segment_name[64] = 0;
  rv = uet_svm_create (segment_name, clib_net_to_host_u32 (mp->queue_depth));
  REPLY_MACRO (VL_API_UET_SVM_CREATE_REPLY);
}

static void
vl_api_uet_svm_delete_t_handler (vl_api_uet_svm_delete_t *mp)
{
  vl_api_uet_svm_delete_reply_t *rmp;
  uet_main_t *um = &uet_main;
  char segment_name[65];
  int rv;

  clib_memcpy_fast (segment_name, mp->segment_name, 64);
  segment_name[64] = 0;
  rv = uet_svm_delete (segment_name);
  REPLY_MACRO (VL_API_UET_SVM_DELETE_REPLY);
}

static void
vl_api_uet_clear_counters_t_handler (vl_api_uet_clear_counters_t *mp)
{
  vl_api_uet_clear_counters_reply_t *rmp;
  uet_main_t *um = &uet_main;
  int rv = 0;

  uet_counters_clear ();
  uet_log_info ("plugin counters cleared through the binary API");
  REPLY_MACRO (VL_API_UET_CLEAR_COUNTERS_REPLY);
}

static void
vl_api_uet_tx_fib_set_t_handler (vl_api_uet_tx_fib_set_t *mp)
{
  vl_api_uet_tx_fib_set_reply_t *rmp;
  uet_main_t *um = &uet_main;
  int rv;

  rv = uet_tx_set_fib_tables (clib_net_to_host_u32 (mp->ip4_table_id),
			      clib_net_to_host_u32 (mp->ip6_table_id));
  REPLY_MACRO (VL_API_UET_TX_FIB_SET_REPLY);
}

typedef struct
{
  u64 generation;
  u64 tx_requests;
  u64 invalid_requests;
  u64 tx_completion_ring_full;
  u64 tx_packets;
  u64 tx_bytes;
  u64 tx_completions;
  u64 rx_delivered;
  u64 rx_ring_full;
  u64 rx_bad_chain;
  u64 rx_releases;
  u64 rx_invalid_releases;
  u64 rx_outstanding;
  u32 worker_index;
  u32 thread_index;
  u32 queue_depth;
  u32 tx_ip4_table_id;
  u32 tx_ip6_table_id;
  u32 tx_pending;
  u32 tx_completions_pending;
  u32 rx_pending;
  u32 rx_release_pending;
  u8 enabled;
  u8 svm_attached;
  u8 provider_ready;
  u8 tx_configured;
} uet_api_worker_snapshot_t;

static void
uet_api_worker_snapshot_read (uet_main_t *um, u32 worker_index, const uet_worker_t *uw,
			      uet_api_worker_snapshot_t *snapshot)
{
  clib_memset (snapshot, 0, sizeof (*snapshot));
  snapshot->worker_index = worker_index;
  snapshot->thread_index = uw->thread_index;
  snapshot->enabled = um->enabled;
  snapshot->tx_configured = uw->tx_configured;
  snapshot->tx_ip4_table_id = uw->tx_ip4_table_id;
  snapshot->tx_ip6_table_id = uw->tx_ip6_table_id;
  snapshot->tx_requests = uw->tx_requests;
  snapshot->invalid_requests = uw->invalid_requests;
  snapshot->tx_completion_ring_full = uw->tx_completion_ring_full;
  snapshot->tx_packets = uw->tx_packets;
  snapshot->tx_bytes = uw->tx_bytes;
  snapshot->tx_completions = uw->tx_completions;
  snapshot->rx_delivered = uw->rx_delivered;
  snapshot->rx_ring_full = uw->rx_ring_full;
  snapshot->rx_bad_chain = uw->rx_bad_chain;
  snapshot->rx_releases = uw->rx_releases;
  snapshot->rx_invalid_releases = uw->rx_invalid_releases;
  snapshot->rx_outstanding = uw->rx_outstanding;

  for (u32 client_index = 0; client_index < vec_len (uw->channels); client_index++)
    {
      const uet_worker_channel_t *channel = uw->channels + client_index;
      u32 client_flags, server_flags;

      if (!channel->active)
	continue;
      client_flags = clib_atomic_load_acq_n (&channel->svm_header->client_flags);
      server_flags = clib_atomic_load_acq_n (&channel->svm_header->server_flags);
      if (!snapshot->svm_attached)
	{
	  snapshot->generation = channel->svm_header->generation;
	  snapshot->queue_depth = channel->svm_header->queue_depth;
	}
      snapshot->svm_attached = 1;
      snapshot->provider_ready |= !!(client_flags & UET_VPP_SVM_CLIENT_F_DMA_READY) &&
				  !!(server_flags & UET_VPP_SVM_SERVER_F_DMA_READY_ACK);
      snapshot->tx_pending += clib_atomic_load_acq_n (&channel->tx_ring->producer) -
			      clib_atomic_load_acq_n (&channel->tx_ring->consumer);
      snapshot->tx_completions_pending +=
	clib_atomic_load_acq_n (&channel->tx_completion_ring->producer) -
	clib_atomic_load_acq_n (&channel->tx_completion_ring->consumer);
      snapshot->rx_pending += clib_atomic_load_acq_n (&channel->rx_ring->producer) -
			      clib_atomic_load_acq_n (&channel->rx_ring->consumer);
      snapshot->rx_release_pending += clib_atomic_load_acq_n (&channel->rx_release_ring->producer) -
				      clib_atomic_load_acq_n (&channel->rx_release_ring->consumer);
    }
}

static void
uet_send_worker_details (vl_api_registration_t *registration, u32 context,
			 const uet_api_worker_snapshot_t *snapshot)
{
  uet_main_t *um = &uet_main;
  vl_api_uet_worker_details_t *rmp;

  REPLY_MACRO_DETAILS4 (
    VL_API_UET_WORKER_DETAILS, registration, context, ({
      rmp->worker_index = clib_host_to_net_u32 (snapshot->worker_index);
      rmp->thread_index = clib_host_to_net_u32 (snapshot->thread_index);
      rmp->enabled = snapshot->enabled;
      rmp->svm_attached = snapshot->svm_attached;
      rmp->provider_ready = snapshot->provider_ready;
      rmp->tx_configured = snapshot->tx_configured;
      rmp->tx_ip4_table_id = clib_host_to_net_u32 (snapshot->tx_ip4_table_id);
      rmp->tx_ip6_table_id = clib_host_to_net_u32 (snapshot->tx_ip6_table_id);
      rmp->generation = clib_host_to_net_u64 (snapshot->generation);
      rmp->queue_depth = clib_host_to_net_u32 (snapshot->queue_depth);
      rmp->tx_pending = clib_host_to_net_u32 (snapshot->tx_pending);
      rmp->tx_completions_pending = clib_host_to_net_u32 (snapshot->tx_completions_pending);
      rmp->rx_pending = clib_host_to_net_u32 (snapshot->rx_pending);
      rmp->rx_release_pending = clib_host_to_net_u32 (snapshot->rx_release_pending);
      rmp->tx_requests = clib_host_to_net_u64 (snapshot->tx_requests);
      rmp->invalid_requests = clib_host_to_net_u64 (snapshot->invalid_requests);
      rmp->tx_completion_ring_full = clib_host_to_net_u64 (snapshot->tx_completion_ring_full);
      rmp->tx_packets = clib_host_to_net_u64 (snapshot->tx_packets);
      rmp->tx_bytes = clib_host_to_net_u64 (snapshot->tx_bytes);
      rmp->tx_completions = clib_host_to_net_u64 (snapshot->tx_completions);
      rmp->rx_delivered = clib_host_to_net_u64 (snapshot->rx_delivered);
      rmp->rx_ring_full = clib_host_to_net_u64 (snapshot->rx_ring_full);
      rmp->rx_bad_chain = clib_host_to_net_u64 (snapshot->rx_bad_chain);
      rmp->rx_releases = clib_host_to_net_u64 (snapshot->rx_releases);
      rmp->rx_invalid_releases = clib_host_to_net_u64 (snapshot->rx_invalid_releases);
      rmp->rx_outstanding = clib_host_to_net_u64 (snapshot->rx_outstanding);
    }));
}

static void
vl_api_uet_worker_dump_t_handler (vl_api_uet_worker_dump_t *mp)
{
  uet_main_t *um = &uet_main;
  vl_api_registration_t *registration;
  uet_api_worker_snapshot_t *snapshots = 0;
  u32 worker_count = vlib_num_workers ();

  registration = vl_api_client_index_to_registration (mp->client_index);
  if (!registration)
    return;

  if (!worker_count || !um->workers)
    return;

  vec_validate (snapshots, worker_count - 1);
  vlib_worker_thread_barrier_sync (um->vlib_main);
  for (u32 worker = 0; worker < worker_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

      uet_api_worker_snapshot_read (um, worker, uw, vec_elt_at_index (snapshots, worker));
    }
  vlib_worker_thread_barrier_release (um->vlib_main);

  for (u32 worker = 0; worker < worker_count; worker++)
    uet_send_worker_details (registration, mp->context, vec_elt_at_index (snapshots, worker));
  vec_free (snapshots);
}

#include <uet/uet.api.c>

static clib_error_t *
uet_init (vlib_main_t *vm)
{
  uet_main_t *um = &uet_main;
  vlib_node_registration_t *rx_handoff_nodes[UET_RX_N_PATHS] = {
    [UET_RX_PATH_IP4_NATIVE] = &uet4_ip_handoff_node,
    [UET_RX_PATH_IP6_NATIVE] = &uet6_ip_handoff_node,
    [UET_RX_PATH_IP4_UDP] = &uet4_udp_handoff_node,
    [UET_RX_PATH_IP6_UDP] = &uet6_udp_handoff_node,
  };
  u32 n_threads = vlib_get_thread_main ()->n_vlib_mains;
  u32 i;

  um->vlib_main = vm;
  um->vnet_main = vnet_get_main ();
  um->dma_buffer_pool_index = (u8) ~0;
  um->dma_listener_file_index = ~0;
  um->tx_ip4_fib_index = ~0;
  um->tx_ip6_fib_index = ~0;
  um->tx_ip4_table_id = ~0;
  um->tx_ip6_table_id = ~0;
  clib_memset (um->client_by_namespace, 0xff, sizeof (um->client_by_namespace));
  clib_bihash_init_40_8 (&um->endpoint_hash, "uet endpoint registrations", 1024, 1 << 20);
  um->fib_source = fib_source_allocate ("uet", FIB_SOURCE_PRIORITY_HI, FIB_SOURCE_BH_SIMPLE);
  vec_validate_aligned (um->workers, n_threads - 1, CLIB_CACHE_LINE_BYTES);

  for (uet_rx_path_t path = 0; path < UET_RX_N_PATHS; path++)
    um->rx_handoff_queue_indices[path] =
      vlib_handoff_alloc_queues (&(vlib_handoff_alloc_queues_args_t){
	.node_index = rx_handoff_nodes[path]->index,
      });

  for (i = 0; i < n_threads; i++)
    {
      um->workers[i].thread_index = UET_INVALID_THREAD_INDEX;
      um->workers[i].tx_ip4_fib_index = ~0;
      um->workers[i].tx_ip6_fib_index = ~0;
      um->workers[i].tx_ip4_table_id = ~0;
      um->workers[i].tx_ip6_table_id = ~0;
    }

  um->msg_id_base = setup_message_id_table ();
  uet_log_debug ("initialized plugin version %s", UET_PLUGIN_BUILD_VERSION);
  return 0;
}

VLIB_INIT_FUNCTION (uet_init);

static clib_error_t *
uet_exit (vlib_main_t *vm)
{
  while (pool_elts (uet_main.clients))
    {
      uet_client_t *client;
      u8 *name = 0;

      pool_foreach (client, uet_main.clients)
	{
	  name = format (0, "%s%c", client->segment.name, 0);
	  break;
	}
      if (!name || uet_svm_delete ((char *) name))
	{
	  vec_free (name);
	  break;
	}
      vec_free (name);
    }
  uet_protocols_unregister (vm);
  uet_dma_listener_delete ();
  clib_bihash_free_40_8 (&uet_main.endpoint_hash);
  if (uet_main.tx_configured)
    {
      fib_table_unlock (uet_main.tx_ip4_fib_index, FIB_PROTOCOL_IP4, uet_main.fib_source);
      fib_table_unlock (uet_main.tx_ip6_fib_index, FIB_PROTOCOL_IP6, uet_main.fib_source);
      uet_main.tx_configured = 0;
    }
  return 0;
}

VLIB_MAIN_LOOP_EXIT_FUNCTION (uet_exit);
