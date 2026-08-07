/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <svm/ssvm.h>
#include <vppinfra/mem.h>

#include <uet/dma_abi.h>
#include <uet/svm_abi.h>

#include "uet_vpp_client.h"

_Static_assert(UET_VPP_CLIENT_NAMESPACE_BITS == UET_VPP_SVM_CLIENT_NAMESPACE_BITS,
	       "client and SVM namespace widths differ");
_Static_assert(UET_VPP_CLIENT_PDC_LOCAL_BITS == UET_VPP_SVM_PDC_LOCAL_BITS,
	       "client and SVM PDC widths differ");
_Static_assert(UET_VPP_CLIENT_RUDI_LOCAL_BITS == UET_VPP_SVM_RUDI_LOCAL_BITS,
	       "client and SVM RUDI widths differ");

#define UET_VPP_CLIENT_HEAP_SIZE       (64U << 20)
#define UET_VPP_CLIENT_ACK_SPINS       1000000U
#define UET_VPP_CLIENT_ATTACH_ATTEMPTS 5

typedef enum
{
  UET_VPP_SLOT_FREE = 0,
  UET_VPP_SLOT_RESERVED,
  UET_VPP_SLOT_INFLIGHT,
} uet_vpp_slot_state_t;

typedef struct
{
  uet_vpp_svm_worker_channel_t *shared;
  uet_vpp_svm_dma_slot_t *dma_slots;
  uet_vpp_svm_spsc_ring_t *tx_ring;
  uet_vpp_svm_tx_desc_t *tx_descs;
  uet_vpp_svm_spsc_ring_t *tx_completion_ring;
  uet_vpp_svm_tx_completion_t *tx_completion_entries;
  uet_vpp_svm_spsc_ring_t *rx_ring;
  uet_vpp_svm_rx_desc_t *rx_descs;
  uet_vpp_svm_spsc_ring_t *rx_release_ring;
  uet_vpp_svm_rx_release_t *rx_release_entries;
  uint8_t *dma_slot_states;
  uint64_t *dma_slot_request_ids;
  uint64_t *rx_token_ids;
  uint32_t dma_next_slot;
  uint32_t dma_inflight;
  uint32_t rx_inflight;
} __attribute__ ((aligned (UET_VPP_SVM_SHARED_ALIGNMENT))) uet_vpp_client_channel_t;

struct uet_vpp_client
{
  ssvm_private_t segment;
  uet_vpp_svm_shared_header_t *header;
  uet_vpp_svm_worker_channel_t *shared_channels;
  uet_vpp_svm_spsc_ring_t *control_request_ring;
  uet_vpp_svm_control_request_t *control_requests;
  uet_vpp_svm_spsc_ring_t *control_completion_ring;
  uet_vpp_svm_control_completion_t *control_completions;
  uet_vpp_client_channel_t *channels;
  uint32_t channel_count;
  void *dma_map;
  uint32_t owner_pid;
  int dma_fd;
  int owner_fd;
  int owner_claimed;
  pthread_mutex_t control_mutex;
  int control_mutex_initialized;
  uint64_t next_control_request_id;
};

static pthread_once_t uet_vpp_heap_once = PTHREAD_ONCE_INIT;
static int uet_vpp_heap_error = -ENOMEM;

static int
uet_vpp_client_wait_dma_ack (uet_vpp_client_t *client, int expected)
{
  uint32_t expected_count = expected ? client->header->worker_count : 0;

  for (uint32_t i = 0; i < UET_VPP_CLIENT_ACK_SPINS; i++)
    {
      uint32_t count = __atomic_load_n (&client->header->server_dma_ready_count, __ATOMIC_ACQUIRE);

      if (count == expected_count)
	return 0;
      if (count > client->header->worker_count)
	return -EPROTO;
      if (!__atomic_load_n (&client->segment.sh->ready, __ATOMIC_ACQUIRE))
	return -ECONNRESET;
      sched_yield ();
    }
  return -ETIMEDOUT;
}

static void
uet_vpp_heap_init (void)
{
  /* Applications embedding another VPP client library may already have a
   * VPP heap.  Reuse it instead of trying to initialize a second main heap.
   */
  if (clib_mem_get_heap () || clib_mem_init (0, UET_VPP_CLIENT_HEAP_SIZE))
    uet_vpp_heap_error = 0;
}

static int
uet_vpp_range_is_valid (uint64_t offset, uint64_t length, uint64_t size)
{
  return offset <= size && length <= size - offset;
}

static int
uet_vpp_header_is_compatible (const uet_vpp_svm_shared_header_t *header, uint64_t header_offset,
			      uint64_t mapped_size)
{
  if (header->magic != UET_VPP_SVM_ABI_MAGIC || header->abi_major != UET_VPP_SVM_ABI_MAJOR ||
      header->abi_minor < UET_VPP_SVM_ABI_MINOR || header->header_size < sizeof (*header) ||
      (header->capabilities & UET_VPP_SVM_REQUIRED_CAPABILITIES) !=
	UET_VPP_SVM_REQUIRED_CAPABILITIES ||
      header->queue_depth < UET_VPP_SVM_MIN_QUEUE_DEPTH ||
      header->queue_depth > UET_VPP_SVM_MAX_QUEUE_DEPTH || header->segment_size > mapped_size ||
      !uet_vpp_range_is_valid (header_offset, header->header_size, header->segment_size))
    return 0;

  if (!header->worker_count ||
      header->worker_channel_desc_size != sizeof (uet_vpp_svm_worker_channel_t) ||
      header->worker_count > UINT32_MAX / header->queue_depth ||
      !uet_vpp_range_is_valid (header->worker_channel_table_offset,
			       (uint64_t) header->worker_count * header->worker_channel_desc_size,
			       header->segment_size) ||
      header->dma_buffer_data_size == 0 || header->dma_map_size == 0)
    return 0;

  if (!header->client_namespace || header->client_namespace > UET_VPP_SVM_CLIENT_NAMESPACE_MAX ||
      header->control_ring_size < UET_VPP_SVM_MIN_QUEUE_DEPTH ||
      header->control_ring_size > UET_VPP_SVM_MAX_QUEUE_DEPTH ||
      !uet_vpp_range_is_valid (header->control_request_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) header->control_ring_size *
				   sizeof (uet_vpp_svm_control_request_t),
			       header->segment_size) ||
      !uet_vpp_range_is_valid (header->control_completion_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) header->control_ring_size *
				   sizeof (uet_vpp_svm_control_completion_t),
			       header->segment_size))
    return 0;

  if (header->tx_desc_size != sizeof (uet_vpp_svm_tx_desc_t) ||
      header->tx_completion_size != sizeof (uet_vpp_svm_tx_completion_t) ||
      header->rx_desc_size != sizeof (uet_vpp_svm_rx_desc_t) ||
      header->rx_release_size != sizeof (uet_vpp_svm_rx_release_t))
    return 0;

  return 1;
}

static int
uet_vpp_channel_is_compatible (const uet_vpp_svm_shared_header_t *header,
			       const uet_vpp_svm_worker_channel_t *channel, uint32_t channel_index)
{
  if (channel->worker_index != channel_index || channel->dma_slot_count != header->queue_depth ||
      channel->tx_ring_size != header->queue_depth ||
      channel->rx_ring_size != header->queue_depth ||
      !uet_vpp_range_is_valid (channel->dma_slot_table_offset,
			       (uint64_t) channel->dma_slot_count * sizeof (uet_vpp_svm_dma_slot_t),
			       header->segment_size) ||
      !uet_vpp_range_is_valid (channel->tx_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) channel->tx_ring_size * header->tx_desc_size,
			       header->segment_size) ||
      !uet_vpp_range_is_valid (channel->tx_completion_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) channel->tx_ring_size * header->tx_completion_size,
			       header->segment_size) ||
      !uet_vpp_range_is_valid (channel->rx_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) channel->rx_ring_size * header->rx_desc_size,
			       header->segment_size) ||
      !uet_vpp_range_is_valid (channel->rx_release_ring_offset,
			       sizeof (uet_vpp_svm_spsc_ring_t) +
				 (uint64_t) channel->rx_ring_size * header->rx_release_size,
			       header->segment_size))
    return 0;

  return 1;
}

static uet_vpp_client_channel_t *
uet_vpp_client_channel (uet_vpp_client_t *client, uint32_t channel_index)
{
  if (!client || channel_index >= client->channel_count)
    return 0;
  return client->channels + channel_index;
}

static int
uet_vpp_ring_is_compatible (const uet_vpp_svm_spsc_ring_t *ring, uint32_t expected_size)
{
  uint32_t expected_mask = (expected_size & (expected_size - 1)) == 0 ? expected_size - 1 : 0;
  uint32_t producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
  uint32_t consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);

  return ring->size == expected_size && ring->mask == expected_mask &&
	 producer - consumer <= expected_size;
}

static int
uet_vpp_client_is_drained (uet_vpp_client_t *client, int require_release_drain)
{
  for (uint32_t i = 0; i < client->channel_count; i++)
    {
      uet_vpp_client_channel_t *channel = client->channels + i;
      const uet_vpp_svm_spsc_ring_t *rings[] = {
	channel->tx_ring,
	channel->tx_completion_ring,
	channel->rx_ring,
	channel->rx_release_ring,
      };
      uint32_t ring_count = require_release_drain ? 4 : 3;

      if (channel->dma_inflight || channel->rx_inflight)
	return 0;
      for (uint32_t ring_index = 0; ring_index < ring_count; ring_index++)
	{
	  const uet_vpp_svm_spsc_ring_t *ring = rings[ring_index];
	  uint32_t consumer = __atomic_load_n (&ring->consumer, __ATOMIC_RELAXED);
	  uint32_t producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
	  uint32_t pending = producer - consumer;

	  if (pending > ring->size)
	    return -EPROTO;
	  if (pending)
	    return 0;
	}
    }
  if (client->control_request_ring && client->control_completion_ring)
    {
      const uet_vpp_svm_spsc_ring_t *rings[] = {
	client->control_request_ring,
	client->control_completion_ring,
      };

      for (uint32_t i = 0; i < 2; i++)
	{
	  uint32_t consumer = __atomic_load_n (&rings[i]->consumer, __ATOMIC_RELAXED);
	  uint32_t producer = __atomic_load_n (&rings[i]->producer, __ATOMIC_ACQUIRE);
	  uint32_t pending = producer - consumer;

	  if (pending > rings[i]->size)
	    return -EPROTO;
	  if (pending)
	    return 0;
	}
    }
  return 1;
}

static int
uet_vpp_client_lock_segment (uet_vpp_client_t *client, const char *segment_name)
{
  int fd = -1;

  for (uint32_t attempt = 0; attempt < UET_VPP_CLIENT_ATTACH_ATTEMPTS; attempt++)
    {
      fd = shm_open (segment_name, O_RDWR, 0);
      if (fd >= 0)
	break;
      if (errno != ENOENT)
	return -errno;
      sleep (1);
    }
  if (fd < 0)
    return -ECONNREFUSED;

  if (flock (fd, LOCK_EX | LOCK_NB) < 0)
    {
      int rv = errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;

      close (fd);
      return rv;
    }

  client->owner_fd = fd;
  return 0;
}

static int
uet_vpp_client_claim (uet_vpp_client_t *client)
{
  uint32_t expected = 0;

  client->owner_pid = (uint32_t) getpid ();
  if (!client->owner_pid)
    return -EINVAL;
  /*
   * Holding the SHM lock excludes a live supported client.  A nonzero value
   * therefore belongs to a client that exited without safely draining its
   * rings; only deleting and recreating the channel may clear that state.
   */
  if (!__atomic_compare_exchange_n (&client->header->owner_pid, &expected, client->owner_pid, 0,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return -EOWNERDEAD;

  client->owner_claimed = 1;
  __atomic_store_n (&client->segment.sh->client_pid, client->owner_pid, __ATOMIC_RELEASE);
  return 0;
}

static void
uet_vpp_client_channel_states_free (uet_vpp_client_t *client)
{
  if (!client || !client->channels)
    return;

  for (uint32_t i = 0; i < client->channel_count; i++)
    {
      free (client->channels[i].dma_slot_request_ids);
      free (client->channels[i].dma_slot_states);
      free (client->channels[i].rx_token_ids);
      client->channels[i].dma_slot_request_ids = 0;
      client->channels[i].dma_slot_states = 0;
      client->channels[i].rx_token_ids = 0;
    }
}

static void
uet_vpp_client_channels_free (uet_vpp_client_t *client)
{
  uet_vpp_client_channel_states_free (client);
  if (!client || !client->channels)
    return;
  free (client->channels);
  client->channels = 0;
}

static void
uet_vpp_client_unmap (uet_vpp_client_t *client)
{
  if (client->owner_claimed && client->header)
    {
      uint32_t expected = client->owner_pid;

      __atomic_compare_exchange_n (&client->header->owner_pid, &expected, 0, 0, __ATOMIC_RELEASE,
				   __ATOMIC_RELAXED);
      client->owner_claimed = 0;
    }
  if (client->segment.sh)
    {
      uint32_t expected = client->owner_pid;

      __atomic_compare_exchange_n (&client->segment.sh->client_pid, &expected, 0, 0,
				   __ATOMIC_RELEASE, __ATOMIC_RELAXED);
      munmap (client->segment.sh, client->segment.ssvm_size);
    }
  if (client->owner_fd >= 0)
    close (client->owner_fd);
  vec_free (client->segment.name);
}

int
uet_vpp_client_open (uet_vpp_client_t **client_out, const char *segment_name,
		     uet_vpp_client_info_t *info)
{
  uet_vpp_client_t *client;
  uint64_t header_offset;
  int rv;

  if (!client_out || !segment_name || !segment_name[0])
    return -EINVAL;
  *client_out = 0;

  if (pthread_once (&uet_vpp_heap_once, uet_vpp_heap_init) != 0 || uet_vpp_heap_error)
    return -ENOMEM;

  client = calloc (1, sizeof (*client));
  if (!client)
    return -ENOMEM;
  client->dma_fd = -1;
  client->owner_fd = -1;

  client->segment.name = format (0, "%s%c", segment_name, 0);
  client->segment.my_pid = getpid ();
  client->segment.attach_timeout = 5;
  rv = uet_vpp_client_lock_segment (client, segment_name);
  if (rv)
    {
      vec_free (client->segment.name);
      free (client);
      return rv;
    }
  if (ssvm_client_init (&client->segment, SSVM_SEGMENT_SHM) != 0)
    {
      uet_vpp_client_unmap (client);
      free (client);
      return -ECONNREFUSED;
    }

  header_offset = (uintptr_t) client->segment.sh->opaque[0];
  if (client->segment.ssvm_size < sizeof (*client->segment.sh) ||
      header_offset < sizeof (*client->segment.sh) ||
      header_offset % UET_VPP_SVM_SHARED_ALIGNMENT != 0 ||
      !uet_vpp_range_is_valid (header_offset, sizeof (*client->header), client->segment.ssvm_size))
    {
      uet_vpp_client_unmap (client);
      free (client);
      return -EPROTO;
    }
  client->header = (uet_vpp_svm_shared_header_t *) ((uint8_t *) client->segment.sh + header_offset);
  if (!uet_vpp_header_is_compatible (client->header, header_offset, client->segment.ssvm_size))
    {
      uet_vpp_client_unmap (client);
      free (client);
      return -EPROTO;
    }

  rv = uet_vpp_client_claim (client);
  if (rv)
    {
      uet_vpp_client_unmap (client);
      free (client);
      return rv;
    }

  client->channel_count = client->header->worker_count;
  client->shared_channels =
    (uet_vpp_svm_worker_channel_t *) ((uint8_t *) client->segment.sh +
				      client->header->worker_channel_table_offset);
  client->control_request_ring =
    (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
				 client->header->control_request_ring_offset);
  client->control_requests = (uet_vpp_svm_control_request_t *) (client->control_request_ring + 1);
  client->control_completion_ring =
    (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
				 client->header->control_completion_ring_offset);
  client->control_completions =
    (uet_vpp_svm_control_completion_t *) (client->control_completion_ring + 1);
  if (!uet_vpp_ring_is_compatible (client->control_request_ring,
				   client->header->control_ring_size) ||
      !uet_vpp_ring_is_compatible (client->control_completion_ring,
				   client->header->control_ring_size))
    goto incompatible;
  rv = posix_memalign ((void **) &client->channels, UET_VPP_SVM_SHARED_ALIGNMENT,
		       client->channel_count * sizeof (*client->channels));
  if (rv)
    {
      uet_vpp_client_unmap (client);
      free (client);
      return rv == ENOMEM ? -ENOMEM : -EINVAL;
    }
  memset (client->channels, 0, client->channel_count * sizeof (*client->channels));

  for (uint32_t i = 0; i < client->channel_count; i++)
    {
      uet_vpp_svm_worker_channel_t *shared = client->shared_channels + i;
      uet_vpp_client_channel_t *channel = client->channels + i;

      if (!uet_vpp_channel_is_compatible (client->header, shared, i))
	goto incompatible;
      channel->shared = shared;
      channel->dma_slots =
	(uet_vpp_svm_dma_slot_t *) ((uint8_t *) client->segment.sh + shared->dma_slot_table_offset);
      channel->tx_ring =
	(uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh + shared->tx_ring_offset);
      channel->tx_descs = (uet_vpp_svm_tx_desc_t *) (channel->tx_ring + 1);
      channel->tx_completion_ring = (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
								 shared->tx_completion_ring_offset);
      channel->tx_completion_entries =
	(uet_vpp_svm_tx_completion_t *) (channel->tx_completion_ring + 1);
      channel->rx_ring =
	(uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh + shared->rx_ring_offset);
      channel->rx_descs = (uet_vpp_svm_rx_desc_t *) (channel->rx_ring + 1);
      channel->rx_release_ring = (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
							      shared->rx_release_ring_offset);
      channel->rx_release_entries = (uet_vpp_svm_rx_release_t *) (channel->rx_release_ring + 1);

      if (!uet_vpp_ring_is_compatible (channel->tx_ring, shared->tx_ring_size) ||
	  !uet_vpp_ring_is_compatible (channel->tx_completion_ring, shared->tx_ring_size) ||
	  !uet_vpp_ring_is_compatible (channel->rx_ring, shared->rx_ring_size) ||
	  !uet_vpp_ring_is_compatible (channel->rx_release_ring, shared->rx_ring_size))
	goto incompatible;
    }
  rv = pthread_mutex_init (&client->control_mutex, 0);
  if (rv)
    goto incompatible;
  client->control_mutex_initialized = 1;
  client->next_control_request_id = 1;
  if (info)
    {
      info->abi_major = client->header->abi_major;
      info->abi_minor = client->header->abi_minor;
      info->channel_count = client->channel_count;
      info->client_namespace = client->header->client_namespace;
      info->queue_depth = client->header->queue_depth;
      info->dma_slot_count = client->header->queue_depth;
      info->dma_buffer_data_size = client->header->dma_buffer_data_size;
      info->dma_map_size = client->header->dma_map_size;
      info->tx_ring_size = client->header->queue_depth;
      info->rx_ring_size = client->header->queue_depth;
      info->generation = client->header->generation;
    }

  *client_out = client;
  return 0;

incompatible:
  if (client->control_mutex_initialized)
    pthread_mutex_destroy (&client->control_mutex);
  uet_vpp_client_channels_free (client);
  uet_vpp_client_unmap (client);
  free (client);
  return -EPROTO;
}

int
uet_vpp_client_close (uet_vpp_client_t *client)
{
  int drained, rv;

  if (!client)
    return -EINVAL;
  drained = uet_vpp_client_is_drained (client, 0);
  if (drained <= 0)
    return drained ? drained : -EBUSY;

  if (client->dma_map)
    {
      __atomic_fetch_and (&client->header->client_flags, ~UET_VPP_SVM_CLIENT_F_DMA_READY,
			  __ATOMIC_RELEASE);
      rv = uet_vpp_client_wait_dma_ack (client, 0);

      drained = rv ? 0 : uet_vpp_client_is_drained (client, 1);
      if (!rv && drained < 0)
	rv = drained;
      else if (!rv && !drained)
	rv = -EBUSY;
      if (rv)
	{
	  __atomic_fetch_or (&client->header->client_flags, UET_VPP_SVM_CLIENT_F_DMA_READY,
			     __ATOMIC_RELEASE);
	  {
	    int ack_rv = uet_vpp_client_wait_dma_ack (client, 1);

	    if (ack_rv)
	      return ack_rv;
	  }
	  return rv;
	}
    }

  if (client->dma_map)
    munmap (client->dma_map, client->header->dma_map_size);
  if (client->dma_fd >= 0)
    close (client->dma_fd);
  if (client->control_mutex_initialized)
    pthread_mutex_destroy (&client->control_mutex);
  uet_vpp_client_channels_free (client);
  uet_vpp_client_unmap (client);
  free (client);
  return 0;
}

int
uet_vpp_client_set_pds_sng (uet_vpp_client_t *client, int enabled)
{
  if (!client)
    return -EINVAL;
  if (client->dma_map)
    return -EBUSY;
  if (enabled)
    __atomic_fetch_or (&client->header->client_flags, UET_VPP_SVM_CLIENT_F_PDS_SNG,
		       __ATOMIC_RELEASE);
  else
    __atomic_fetch_and (&client->header->client_flags, ~UET_VPP_SVM_CLIENT_F_PDS_SNG,
			__ATOMIC_RELEASE);
  return 0;
}

int
uet_vpp_client_map_dma (uet_vpp_client_t *client, const char *socket_path)
{
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  uet_vpp_dma_reply_t reply;
  char control[256] = { 0 };
  struct iovec iov = { .iov_base = &reply, .iov_len = sizeof (reply) };
  struct msghdr message = {
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = control,
    .msg_controllen = sizeof (control),
  };
  struct cmsghdr *cmsg;
  ssize_t received;
  int invalid_control = 0;
  int socket_fd, received_fd = -1;

  if (!client || !socket_path || !socket_path[0])
    return -EINVAL;
  if (client->dma_map)
    return -EALREADY;
  if (strlen (socket_path) >= sizeof (address.sun_path))
    return -ENAMETOOLONG;

  socket_fd = socket (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket_fd < 0)
    return -errno;
  memcpy (address.sun_path, socket_path, strlen (socket_path) + 1);
  if (connect (socket_fd, (struct sockaddr *) &address, sizeof (address)) != 0)
    {
      int error = -errno;

      close (socket_fd);
      return error;
    }

  received = recvmsg (socket_fd, &message, MSG_CMSG_CLOEXEC);
  if (received != sizeof (reply) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
    {
      int error = received < 0 ? -errno : -EPROTO;

      close (socket_fd);
      return error;
    }

  for (cmsg = CMSG_FIRSTHDR (&message); cmsg; cmsg = CMSG_NXTHDR (&message, cmsg))
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
      {
	if (received_fd >= 0 || cmsg->cmsg_len != CMSG_LEN (sizeof (received_fd)))
	  invalid_control = 1;
	else
	  memcpy (&received_fd, CMSG_DATA (cmsg), sizeof (received_fd));
      }
  close (socket_fd);

  if (reply.magic != UET_VPP_DMA_ABI_MAGIC || reply.version != UET_VPP_DMA_ABI_VERSION ||
      reply.status != 0 || invalid_control || received_fd < 0 ||
      reply.generation != client->header->generation ||
      reply.map_size != client->header->dma_map_size ||
      reply.slot_count != client->channel_count * client->header->queue_depth ||
      reply.buffer_data_size != client->header->dma_buffer_data_size ||
      reply.buffer_pool_index != client->header->dma_buffer_pool_index)
    {
      if (received_fd >= 0)
	close (received_fd);
      return reply.status ? reply.status : -EPROTO;
    }

  client->dma_map = mmap (0, reply.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, received_fd, 0);
  if (client->dma_map == MAP_FAILED)
    {
      int error = -errno;

      client->dma_map = 0;
      close (received_fd);
      return error;
    }

  for (uint32_t channel_index = 0; channel_index < client->channel_count; channel_index++)
    {
      uet_vpp_client_channel_t *channel = client->channels + channel_index;

      for (uint32_t slot = 0; slot < channel->shared->dma_slot_count; slot++)
	if (channel->dma_slots[slot].capacity == 0 ||
	    channel->dma_slots[slot].capacity > client->header->dma_buffer_data_size ||
	    channel->dma_slots[slot].data_offset > reply.map_size ||
	    channel->dma_slots[slot].capacity >
	      reply.map_size - channel->dma_slots[slot].data_offset)
	  goto invalid_dma_slot;

      channel->dma_slot_states =
	calloc (channel->shared->dma_slot_count, sizeof (*channel->dma_slot_states));
      channel->dma_slot_request_ids =
	calloc (channel->shared->dma_slot_count, sizeof (*channel->dma_slot_request_ids));
      channel->rx_token_ids =
	calloc (channel->shared->rx_ring_size, sizeof (*channel->rx_token_ids));
      if (!channel->dma_slot_states || !channel->dma_slot_request_ids || !channel->rx_token_ids)
	{
	  uet_vpp_client_channel_states_free (client);
	  munmap (client->dma_map, reply.map_size);
	  client->dma_map = 0;
	  close (received_fd);
	  return -ENOMEM;
	}
    }

  client->dma_fd = received_fd;
  __atomic_fetch_or (&client->header->client_flags, UET_VPP_SVM_CLIENT_F_DMA_READY,
		     __ATOMIC_RELEASE);
  {
    int rv = uet_vpp_client_wait_dma_ack (client, 1);

    if (rv)
      {
	int rollback_rv;

	__atomic_fetch_and (&client->header->client_flags, ~UET_VPP_SVM_CLIENT_F_DMA_READY,
			    __ATOMIC_RELEASE);
	rollback_rv = uet_vpp_client_wait_dma_ack (client, 0);
	if (rollback_rv)
	  return rollback_rv;
	uet_vpp_client_channel_states_free (client);
	munmap (client->dma_map, reply.map_size);
	client->dma_map = 0;
	close (client->dma_fd);
	client->dma_fd = -1;
	return rv;
      }
  }
  return 0;

invalid_dma_slot:
  uet_vpp_client_channel_states_free (client);
  munmap (client->dma_map, reply.map_size);
  client->dma_map = 0;
  close (received_fd);
  return -EPROTO;
}

static int
uet_vpp_client_endpoint_control (uet_vpp_client_t *client, uint16_t operation,
				 const uet_vpp_client_endpoint_t *endpoint)
{
  uet_vpp_svm_spsc_ring_t *request_ring, *completion_ring;
  uet_vpp_svm_control_request_t *request;
  uint32_t request_producer, request_consumer, completion_consumer;
  uint64_t request_id;
  int lock_rv, rv = -ETIMEDOUT;

  if (!client || !endpoint || (endpoint->ip_version != 4 && endpoint->ip_version != 6) ||
      endpoint->absolute > 1 || endpoint->pid_on_fep > 0x0fff ||
      endpoint->resource_index > 0x0fff || endpoint->job_id > 0x00ffffff)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;

  lock_rv = pthread_mutex_lock (&client->control_mutex);
  if (lock_rv)
    return -lock_rv;

  request_ring = client->control_request_ring;
  completion_ring = client->control_completion_ring;
  request_producer = __atomic_load_n (&request_ring->producer, __ATOMIC_RELAXED);
  request_consumer = __atomic_load_n (&request_ring->consumer, __ATOMIC_ACQUIRE);
  completion_consumer = __atomic_load_n (&completion_ring->consumer, __ATOMIC_RELAXED);
  if (request_producer - request_consumer >= request_ring->size)
    {
      rv = -EAGAIN;
      goto unlock;
    }

  request_id = client->next_control_request_id++;
  if (!request_id)
    request_id = client->next_control_request_id++;
  request = client->control_requests + (request_ring->mask ? request_producer & request_ring->mask :
							     request_producer % request_ring->size);
  memset (request, 0, sizeof (*request));
  request->request_id = request_id;
  request->operation = operation;
  request->endpoint.ip_version = endpoint->ip_version;
  request->endpoint.absolute = endpoint->absolute;
  request->endpoint.pid_on_fep = endpoint->pid_on_fep;
  request->endpoint.resource_index = endpoint->resource_index;
  request->endpoint.job_id = endpoint->absolute ? 0 : endpoint->job_id;
  memcpy (request->endpoint.ip_address, endpoint->ip_address,
	  sizeof (request->endpoint.ip_address));
  __atomic_store_n (&request_ring->producer, request_producer + 1, __ATOMIC_RELEASE);

  for (uint32_t spin = 0; spin < UET_VPP_CLIENT_ACK_SPINS; spin++)
    {
      uint32_t completion_producer = __atomic_load_n (&completion_ring->producer, __ATOMIC_ACQUIRE);
      uint32_t available = completion_producer - completion_consumer;

      if (available > completion_ring->size)
	{
	  rv = -EPROTO;
	  break;
	}
      if (available)
	{
	  uet_vpp_svm_control_completion_t *completion =
	    client->control_completions + (completion_ring->mask ?
					     completion_consumer & completion_ring->mask :
					     completion_consumer % completion_ring->size);

	  if (completion->request_id != request_id || completion->reserved0 ||
	      completion->reserved1[0] || completion->reserved1[1])
	    rv = -EPROTO;
	  else
	    rv = completion->status;
	  __atomic_store_n (&completion_ring->consumer, completion_consumer + 1, __ATOMIC_RELEASE);
	  break;
	}
      if (!__atomic_load_n (&client->segment.sh->ready, __ATOMIC_ACQUIRE))
	{
	  rv = -ECONNRESET;
	  break;
	}
      sched_yield ();
    }

unlock:
  pthread_mutex_unlock (&client->control_mutex);
  return rv;
}

int
uet_vpp_client_endpoint_add (uet_vpp_client_t *client, const uet_vpp_client_endpoint_t *endpoint)
{
  return uet_vpp_client_endpoint_control (client, UET_VPP_SVM_CONTROL_ENDPOINT_ADD, endpoint);
}

int
uet_vpp_client_endpoint_del (uet_vpp_client_t *client, const uet_vpp_client_endpoint_t *endpoint)
{
  return uet_vpp_client_endpoint_control (client, UET_VPP_SVM_CONTROL_ENDPOINT_DEL, endpoint);
}

int
uet_vpp_client_acquire_dma (uet_vpp_client_t *client, uint32_t channel_index, uint32_t *dma_slot,
			    void **data, size_t *capacity)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);
  uint32_t i;

  if (!channel || !dma_slot || !data || !capacity)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;

  for (i = 0; i < channel->shared->dma_slot_count; i++)
    {
      uint32_t slot = (channel->dma_next_slot + i) % channel->shared->dma_slot_count;

      if (channel->dma_slot_states[slot] != UET_VPP_SLOT_FREE)
	continue;

      channel->dma_slot_states[slot] = UET_VPP_SLOT_RESERVED;
      channel->dma_next_slot = (slot + 1) % channel->shared->dma_slot_count;
      *dma_slot = slot;
      *data = (uint8_t *) client->dma_map + channel->dma_slots[slot].data_offset;
      *capacity = channel->dma_slots[slot].capacity;
      return 0;
    }

  return -EAGAIN;
}

int
uet_vpp_client_release_dma (uet_vpp_client_t *client, uint32_t channel_index, uint32_t dma_slot)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);

  if (!channel || dma_slot >= channel->shared->dma_slot_count)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;
  if (channel->dma_slot_states[dma_slot] != UET_VPP_SLOT_RESERVED)
    return -EPERM;

  channel->dma_slot_states[dma_slot] = UET_VPP_SLOT_FREE;
  return 0;
}

int
uet_vpp_client_submit_ip_batch (uet_vpp_client_t *client, uint32_t channel_index,
				const uet_vpp_client_tx_request_t *requests, size_t request_count)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, i;

  if (!channel || !requests || request_count == 0 || request_count > UINT32_MAX)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;
  ring = channel->tx_ring;
  producer = __atomic_load_n (&ring->producer, __ATOMIC_RELAXED);
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);
  if (producer - consumer > ring->size)
    return -EPROTO;
  if (request_count > ring->size - (producer - consumer))
    return -EAGAIN;

  for (i = 0; i < request_count; i++)
    {
      uint32_t slot = requests[i].dma_slot;

      if (slot >= channel->shared->dma_slot_count)
	return -EINVAL;
      if (channel->dma_slot_states[slot] != UET_VPP_SLOT_RESERVED)
	return -EPERM;
      if (requests[i].packet_length < 20 ||
	  requests[i].packet_length > channel->dma_slots[slot].capacity)
	return -EMSGSIZE;
    }

  for (i = 0; i < request_count; i++)
    {
      const uet_vpp_client_tx_request_t *request = requests + i;
      uint32_t slot = request->dma_slot;
      uint32_t index = ring->mask ? (producer + i) & ring->mask : (producer + i) % ring->size;
      uet_vpp_svm_tx_desc_t *desc = channel->tx_descs + index;

      if (channel->dma_slot_states[slot] != UET_VPP_SLOT_RESERVED)
	{
	  while (i)
	    {
	      slot = requests[--i].dma_slot;
	      channel->dma_slot_request_ids[slot] = 0;
	      channel->dma_slot_states[slot] = UET_VPP_SLOT_RESERVED;
	    }
	  return -EINVAL;
	}
      desc->dma_slot = slot;
      desc->packet_length = request->packet_length;
      desc->request_id = request->request_id;
      desc->user_context = request->user_context;
      desc->reserved = 0;
      channel->dma_slot_request_ids[slot] = request->request_id;
      channel->dma_slot_states[slot] = UET_VPP_SLOT_INFLIGHT;
    }
  channel->dma_inflight += request_count;
  __atomic_store_n (&ring->producer, producer + (uint32_t) request_count, __ATOMIC_RELEASE);
  return 0;
}

int
uet_vpp_client_submit_ip (uet_vpp_client_t *client, uint32_t channel_index, uint32_t dma_slot,
			  uint32_t packet_length, uint64_t request_id, uint64_t user_context)
{
  uet_vpp_client_tx_request_t request = {
    .dma_slot = dma_slot,
    .packet_length = packet_length,
    .request_id = request_id,
    .user_context = user_context,
  };

  return uet_vpp_client_submit_ip_batch (client, channel_index, &request, 1);
}

int
uet_vpp_client_poll_batch (uet_vpp_client_t *client, uint32_t channel_index,
			   uet_vpp_client_completion_t *completions, size_t completion_capacity)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, available, i;

  if (!channel || !completions || completion_capacity == 0 || completion_capacity > INT32_MAX)
    return -EINVAL;

  ring = channel->tx_completion_ring;
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_RELAXED);
  producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
  available = producer - consumer;
  if (available > ring->size)
    return -EPROTO;
  available = available < completion_capacity ? available : completion_capacity;

  for (i = 0; i < available; i++)
    {
      uint32_t index = ring->mask ? (consumer + i) & ring->mask : (consumer + i) % ring->size;
      const uet_vpp_svm_tx_completion_t *wire = channel->tx_completion_entries + index;
      uint32_t slot = wire->dma_slot;

      if (wire->reserved || !client->dma_map || slot >= channel->shared->dma_slot_count ||
	  channel->dma_slot_states[slot] != UET_VPP_SLOT_INFLIGHT ||
	  channel->dma_slot_request_ids[slot] != wire->request_id)
	return -EPROTO;
    }

  for (i = 0; i < available; i++)
    {
      uint32_t index = ring->mask ? (consumer + i) & ring->mask : (consumer + i) % ring->size;
      const uet_vpp_svm_tx_completion_t *wire = channel->tx_completion_entries + index;
      uet_vpp_client_completion_t *completion = completions + i;
      uint32_t slot = wire->dma_slot;

      completion->status = wire->status;
      completion->completed_length = wire->completed_length;
      completion->dma_slot = slot;
      completion->request_id = wire->request_id;
      completion->user_context = wire->user_context;
      channel->dma_slot_request_ids[slot] = 0;
      channel->dma_slot_states[slot] = UET_VPP_SLOT_FREE;
    }
  if (available)
    {
      channel->dma_inflight -= available;
      __atomic_store_n (&ring->consumer, consumer + available, __ATOMIC_RELEASE);
      return available;
    }
  return 0;
}

int
uet_vpp_client_poll (uet_vpp_client_t *client, uint32_t channel_index,
		     uet_vpp_client_completion_t *completion)
{
  return uet_vpp_client_poll_batch (client, channel_index, completion, 1);
}

static int
uet_vpp_client_rx_decode (uet_vpp_client_t *client, uet_vpp_client_channel_t *channel,
			  const uet_vpp_svm_rx_desc_t *desc, uet_vpp_client_rx_t *rx)
{
  uint64_t total = 0;

  if (!desc->rx_id || desc->reserved || desc->release_token >= channel->rx_ring->size ||
      !desc->segment_count || desc->segment_count > UET_VPP_SVM_MAX_RX_SEGS ||
      !desc->packet_length ||
      !((desc->flags & UET_VPP_SVM_RX_F_IP4) ^ (desc->flags & UET_VPP_SVM_RX_F_IP6)) ||
      desc->flags & ~(UET_VPP_SVM_RX_F_IP4 | UET_VPP_SVM_RX_F_IP6 | UET_VPP_SVM_RX_F_UDP))
    return -EPROTO;

  memset (rx, 0, sizeof (*rx));
  for (uint32_t i = 0; i < desc->segment_count; i++)
    {
      const uet_vpp_svm_rx_segment_t *segment = desc->segments + i;

      if (!segment->length || segment->reserved ||
	  segment->data_offset > client->header->dma_map_size ||
	  segment->length > client->header->dma_map_size - segment->data_offset)
	return -EPROTO;
      rx->iov[i].base = (uint8_t *) client->dma_map + segment->data_offset;
      rx->iov[i].length = segment->length;
      total += segment->length;
    }
  if (total != desc->packet_length)
    return -EPROTO;

  rx->rx_id = desc->rx_id;
  rx->release_token = desc->release_token;
  rx->packet_length = desc->packet_length;
  rx->rx_sw_if_index = desc->rx_sw_if_index;
  rx->flags = desc->flags;
  rx->iov_count = desc->segment_count;
  return 0;
}

int
uet_vpp_client_poll_rx_batch (uet_vpp_client_t *client, uint32_t channel_index,
			      uet_vpp_client_rx_t *rx, size_t rx_capacity)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, available;

  if (!channel || !rx || !rx_capacity || rx_capacity > UINT32_MAX)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;

  ring = channel->rx_ring;
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_RELAXED);
  producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
  available = producer - consumer;
  if (available > ring->size)
    return -EPROTO;
  if (available > rx_capacity)
    available = rx_capacity;

  for (uint32_t i = 0; i < available; i++)
    {
      uint32_t counter = consumer + i;
      uint32_t index = ring->mask ? counter & ring->mask : counter % ring->size;
      int rv = uet_vpp_client_rx_decode (client, channel, channel->rx_descs + index, rx + i);

      if (rv)
	return rv;
    }
  for (uint32_t i = 0; i < available; i++)
    {
      uint32_t token = rx[i].release_token;

      if (channel->rx_token_ids[token])
	{
	  while (i)
	    channel->rx_token_ids[rx[--i].release_token] = 0;
	  return -EPROTO;
	}
      channel->rx_token_ids[token] = rx[i].rx_id;
    }
  if (available)
    {
      channel->rx_inflight += available;
      __atomic_store_n (&ring->consumer, consumer + available, __ATOMIC_RELEASE);
    }
  return available;
}

int
uet_vpp_client_poll_rx (uet_vpp_client_t *client, uint32_t channel_index, uet_vpp_client_rx_t *rx)
{
  return uet_vpp_client_poll_rx_batch (client, channel_index, rx, 1);
}

int
uet_vpp_client_release_rx_batch (uet_vpp_client_t *client, uint32_t channel_index,
				 const uet_vpp_client_rx_t *rx, size_t rx_count)
{
  uet_vpp_client_channel_t *channel = uet_vpp_client_channel (client, channel_index);
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer;

  if (!channel || !rx || !rx_count || rx_count > UINT32_MAX || rx_count > channel->rx_inflight)
    return -EINVAL;

  ring = channel->rx_release_ring;
  producer = __atomic_load_n (&ring->producer, __ATOMIC_RELAXED);
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);
  if (producer - consumer > ring->size)
    return -EPROTO;
  if (rx_count > ring->size - (producer - consumer))
    return -EAGAIN;

  for (uint32_t i = 0; i < rx_count; i++)
    if (!rx[i].rx_id || rx[i].release_token >= channel->shared->rx_ring_size ||
	channel->rx_token_ids[rx[i].release_token] != rx[i].rx_id)
      return -EINVAL;

  for (uint32_t i = 0; i < rx_count; i++)
    {
      uint32_t counter = producer + i;
      uint32_t index = ring->mask ? counter & ring->mask : counter % ring->size;
      uet_vpp_svm_rx_release_t *release = channel->rx_release_entries + index;
      uint32_t token = rx[i].release_token;

      if (channel->rx_token_ids[token] != rx[i].rx_id)
	{
	  while (i)
	    {
	      i--;
	      channel->rx_token_ids[rx[i].release_token] = rx[i].rx_id;
	    }
	  return -EINVAL;
	}

      release->rx_id = rx[i].rx_id;
      release->release_token = token;
      release->reserved = 0;
      channel->rx_token_ids[token] = 0;
    }
  channel->rx_inflight -= rx_count;
  __atomic_store_n (&ring->producer, producer + (uint32_t) rx_count, __ATOMIC_RELEASE);
  return 0;
}

int
uet_vpp_client_release_rx (uet_vpp_client_t *client, uint32_t channel_index,
			   const uet_vpp_client_rx_t *rx)
{
  return uet_vpp_client_release_rx_batch (client, channel_index, rx, 1);
}
