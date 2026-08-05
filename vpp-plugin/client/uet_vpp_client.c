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

#define UET_VPP_CLIENT_HEAP_SIZE (64U << 20)
#define UET_VPP_CLIENT_ACK_SPINS 1000000U
#define UET_VPP_CLIENT_ATTACH_ATTEMPTS 5

typedef enum
{
  UET_VPP_SLOT_FREE = 0,
  UET_VPP_SLOT_RESERVED,
  UET_VPP_SLOT_INFLIGHT,
} uet_vpp_slot_state_t;

struct uet_vpp_client
{
  ssvm_private_t segment;
  uet_vpp_svm_shared_header_t *header;
  uet_vpp_svm_dma_slot_t *dma_slots;
  uet_vpp_svm_spsc_ring_t *tx_ring;
  uet_vpp_svm_tx_desc_t *tx_descs;
  uet_vpp_svm_spsc_ring_t *tx_completion_ring;
  uet_vpp_svm_tx_completion_t *tx_completion_entries;
  uet_vpp_svm_spsc_ring_t *rx_ring;
  uet_vpp_svm_rx_desc_t *rx_descs;
  uet_vpp_svm_spsc_ring_t *rx_release_ring;
  uet_vpp_svm_rx_release_t *rx_release_entries;
  void *dma_map;
  uint8_t *dma_slot_states;
  uint64_t *dma_slot_request_ids;
  uint64_t *rx_token_ids;
  uint32_t dma_next_slot;
  uint32_t dma_inflight;
  uint32_t rx_inflight;
  uint32_t owner_pid;
  int dma_fd;
  int owner_fd;
  int owner_claimed;
};

static pthread_once_t uet_vpp_heap_once = PTHREAD_ONCE_INIT;
static int uet_vpp_heap_error = -ENOMEM;

static int
uet_vpp_client_wait_dma_ack (uet_vpp_client_t *client, int expected)
{
  for (uint32_t i = 0; i < UET_VPP_CLIENT_ACK_SPINS; i++)
    {
      uint32_t flags = __atomic_load_n (&client->header->server_flags, __ATOMIC_ACQUIRE);

      if (!!(flags & UET_VPP_SVM_SERVER_F_DMA_READY_ACK) == !!expected)
	return 0;
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

  if (header->dma_slot_count != header->queue_depth ||
      header->dma_slot_desc_size != sizeof (uet_vpp_svm_dma_slot_t) ||
      header->dma_buffer_data_size == 0 || header->dma_map_size == 0 ||
      !uet_vpp_range_is_valid (header->dma_slot_table_offset,
				(uint64_t) header->dma_slot_count * header->dma_slot_desc_size,
				header->segment_size))
    return 0;

  if (header->tx_ring_size != header->queue_depth ||
      header->tx_desc_size != sizeof (uet_vpp_svm_tx_desc_t) ||
      header->tx_completion_size != sizeof (uet_vpp_svm_tx_completion_t) ||
      !uet_vpp_range_is_valid (
	header->tx_ring_offset,
	sizeof (uet_vpp_svm_spsc_ring_t) +
	  (uint64_t) header->tx_ring_size * header->tx_desc_size,
	header->segment_size) ||
      !uet_vpp_range_is_valid (
	header->tx_completion_ring_offset,
	sizeof (uet_vpp_svm_spsc_ring_t) +
	  (uint64_t) header->tx_ring_size * header->tx_completion_size,
	header->segment_size))
    return 0;

  if (header->rx_ring_size != header->queue_depth ||
      header->rx_desc_size != sizeof (uet_vpp_svm_rx_desc_t) ||
      header->rx_release_size != sizeof (uet_vpp_svm_rx_release_t) ||
      !uet_vpp_range_is_valid (
	header->rx_ring_offset,
	sizeof (uet_vpp_svm_spsc_ring_t) +
	  (uint64_t) header->rx_ring_size * header->rx_desc_size,
	header->segment_size) ||
      !uet_vpp_range_is_valid (
	header->rx_release_ring_offset,
	sizeof (uet_vpp_svm_spsc_ring_t) +
	  (uint64_t) header->rx_ring_size * header->rx_release_size,
	header->segment_size))
    return 0;

  return 1;
}

static int
uet_vpp_ring_is_compatible (const uet_vpp_svm_spsc_ring_t *ring, uint32_t expected_size)
{
  uint32_t expected_mask =
    (expected_size & (expected_size - 1)) == 0 ? expected_size - 1 : 0;
  uint32_t producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
  uint32_t consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);

  return ring->size == expected_size && ring->mask == expected_mask &&
	 producer - consumer <= expected_size;
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
  client->header =
    (uet_vpp_svm_shared_header_t *) ((uint8_t *) client->segment.sh + header_offset);
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

  client->dma_slots = (uet_vpp_svm_dma_slot_t *) ((uint8_t *) client->segment.sh +
						  client->header->dma_slot_table_offset);
  client->tx_ring =
    (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh + client->header->tx_ring_offset);
  client->tx_descs = (uet_vpp_svm_tx_desc_t *) (client->tx_ring + 1);
  client->tx_completion_ring =
    (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
				 client->header->tx_completion_ring_offset);
  client->tx_completion_entries = (uet_vpp_svm_tx_completion_t *) (client->tx_completion_ring + 1);
  client->rx_ring =
    (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh + client->header->rx_ring_offset);
  client->rx_descs = (uet_vpp_svm_rx_desc_t *) (client->rx_ring + 1);
  client->rx_release_ring = (uet_vpp_svm_spsc_ring_t *) ((uint8_t *) client->segment.sh +
							 client->header->rx_release_ring_offset);
  client->rx_release_entries = (uet_vpp_svm_rx_release_t *) (client->rx_release_ring + 1);
  if (!uet_vpp_ring_is_compatible (client->tx_ring, client->header->tx_ring_size) ||
      !uet_vpp_ring_is_compatible (client->tx_completion_ring, client->header->tx_ring_size) ||
      !uet_vpp_ring_is_compatible (client->rx_ring, client->header->rx_ring_size) ||
      !uet_vpp_ring_is_compatible (client->rx_release_ring, client->header->rx_ring_size))
    {
      uet_vpp_client_unmap (client);
      free (client);
      return -EPROTO;
    }
  if (info)
    {
      info->abi_major = client->header->abi_major;
      info->abi_minor = client->header->abi_minor;
      info->queue_depth = client->header->queue_depth;
      info->dma_slot_count = client->header->dma_slot_count;
      info->dma_buffer_data_size = client->header->dma_buffer_data_size;
      info->dma_map_size = client->header->dma_map_size;
      info->tx_ring_size = client->header->tx_ring_size;
      info->rx_ring_size = client->header->rx_ring_size;
      info->generation = client->header->generation;
    }

  *client_out = client;
  return 0;
}

int
uet_vpp_client_close (uet_vpp_client_t *client)
{
  uint32_t producer, consumer;
  int rv;

  if (!client)
    return -EINVAL;
  if (client->dma_inflight || client->rx_inflight)
    return -EBUSY;

  consumer = __atomic_load_n (&client->rx_ring->consumer, __ATOMIC_RELAXED);
  producer = __atomic_load_n (&client->rx_ring->producer, __ATOMIC_ACQUIRE);
  if (producer != consumer)
    return -EBUSY;

  if (client->dma_map)
    {
      __atomic_fetch_and (&client->header->client_flags, ~UET_VPP_SVM_CLIENT_F_DMA_READY,
			  __ATOMIC_RELEASE);
      rv = uet_vpp_client_wait_dma_ack (client, 0);
      consumer = __atomic_load_n (&client->rx_ring->consumer, __ATOMIC_RELAXED);
      producer = __atomic_load_n (&client->rx_ring->producer, __ATOMIC_ACQUIRE);
      if (rv || producer != consumer)
	{
	  __atomic_fetch_or (&client->header->client_flags, UET_VPP_SVM_CLIENT_F_DMA_READY,
			     __ATOMIC_RELEASE);
	  return rv ? rv : -EBUSY;
	}
    }

  if (client->dma_map)
    munmap (client->dma_map, client->header->dma_map_size);
  if (client->dma_fd >= 0)
    close (client->dma_fd);
  free (client->dma_slot_request_ids);
  free (client->dma_slot_states);
  free (client->rx_token_ids);
  uet_vpp_client_unmap (client);
  free (client);
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
      reply.slot_count != client->header->dma_slot_count ||
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

  for (uint32_t i = 0; i < client->header->dma_slot_count; i++)
    if (client->dma_slots[i].capacity == 0 ||
	client->dma_slots[i].capacity > client->header->dma_buffer_data_size ||
	client->dma_slots[i].data_offset > reply.map_size ||
	client->dma_slots[i].capacity > reply.map_size - client->dma_slots[i].data_offset)
      {
	munmap (client->dma_map, reply.map_size);
	client->dma_map = 0;
	close (received_fd);
	return -EPROTO;
      }

  client->dma_slot_states =
    calloc (client->header->dma_slot_count, sizeof (*client->dma_slot_states));
  client->dma_slot_request_ids =
    calloc (client->header->dma_slot_count, sizeof (*client->dma_slot_request_ids));
  client->rx_token_ids = calloc (client->header->rx_ring_size, sizeof (*client->rx_token_ids));
  if (!client->dma_slot_states || !client->dma_slot_request_ids || !client->rx_token_ids)
    {
      free (client->rx_token_ids);
      free (client->dma_slot_request_ids);
      free (client->dma_slot_states);
      client->rx_token_ids = 0;
      client->dma_slot_request_ids = 0;
      client->dma_slot_states = 0;
      munmap (client->dma_map, reply.map_size);
      client->dma_map = 0;
      close (received_fd);
      return -ENOMEM;
    }

  client->dma_fd = received_fd;
  __atomic_fetch_or (&client->header->client_flags, UET_VPP_SVM_CLIENT_F_DMA_READY,
		     __ATOMIC_RELEASE);
  {
    int rv = uet_vpp_client_wait_dma_ack (client, 1);

    if (rv)
      {
	__atomic_fetch_and (&client->header->client_flags, ~UET_VPP_SVM_CLIENT_F_DMA_READY,
			    __ATOMIC_RELEASE);
	free (client->rx_token_ids);
	free (client->dma_slot_request_ids);
	free (client->dma_slot_states);
	client->rx_token_ids = 0;
	client->dma_slot_request_ids = 0;
	client->dma_slot_states = 0;
	munmap (client->dma_map, reply.map_size);
	client->dma_map = 0;
	close (client->dma_fd);
	client->dma_fd = -1;
	return rv;
      }
  }
  return 0;
}

int
uet_vpp_client_acquire_dma (uet_vpp_client_t *client, uint32_t *dma_slot, void **data,
			    size_t *capacity)
{
  uint32_t i;

  if (!client || !dma_slot || !data || !capacity)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;

  for (i = 0; i < client->header->dma_slot_count; i++)
    {
      uint32_t slot = (client->dma_next_slot + i) % client->header->dma_slot_count;

      if (client->dma_slot_states[slot] != UET_VPP_SLOT_FREE)
	continue;

      client->dma_slot_states[slot] = UET_VPP_SLOT_RESERVED;
      client->dma_next_slot = (slot + 1) % client->header->dma_slot_count;
      *dma_slot = slot;
      *data = (uint8_t *) client->dma_map + client->dma_slots[slot].data_offset;
      *capacity = client->dma_slots[slot].capacity;
      return 0;
    }

  return -EAGAIN;
}

int
uet_vpp_client_release_dma (uet_vpp_client_t *client, uint32_t dma_slot)
{
  if (!client || dma_slot >= client->header->dma_slot_count)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;
  if (client->dma_slot_states[dma_slot] != UET_VPP_SLOT_RESERVED)
    return -EPERM;

  client->dma_slot_states[dma_slot] = UET_VPP_SLOT_FREE;
  return 0;
}

int
uet_vpp_client_submit_ip_batch (uet_vpp_client_t *client,
				const uet_vpp_client_tx_request_t *requests, size_t request_count)
{
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, i;

  if (!client || !requests || request_count == 0 || request_count > UINT32_MAX)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;
  ring = client->tx_ring;
  producer = __atomic_load_n (&ring->producer, __ATOMIC_RELAXED);
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);
  if (producer - consumer > ring->size)
    return -EPROTO;
  if (request_count > ring->size - (producer - consumer))
    return -EAGAIN;

  for (i = 0; i < request_count; i++)
    {
      uint32_t slot = requests[i].dma_slot;

      if (slot >= client->header->dma_slot_count)
	return -EINVAL;
      if (client->dma_slot_states[slot] != UET_VPP_SLOT_RESERVED)
	return -EPERM;
      if (requests[i].packet_length < 20 ||
	  requests[i].packet_length > client->dma_slots[slot].capacity)
	return -EMSGSIZE;
    }

  for (i = 0; i < request_count; i++)
    {
      const uet_vpp_client_tx_request_t *request = requests + i;
      uint32_t slot = request->dma_slot;
      uint32_t index = ring->mask ? (producer + i) & ring->mask : (producer + i) % ring->size;
      uet_vpp_svm_tx_desc_t *desc = client->tx_descs + index;

      if (client->dma_slot_states[slot] != UET_VPP_SLOT_RESERVED)
	{
	  while (i)
	    {
	      slot = requests[--i].dma_slot;
	      client->dma_slot_request_ids[slot] = 0;
	      client->dma_slot_states[slot] = UET_VPP_SLOT_RESERVED;
	    }
	  return -EINVAL;
	}
      desc->dma_slot = slot;
      desc->packet_length = request->packet_length;
      desc->request_id = request->request_id;
      desc->user_context = request->user_context;
      desc->reserved = 0;
      client->dma_slot_request_ids[slot] = request->request_id;
      client->dma_slot_states[slot] = UET_VPP_SLOT_INFLIGHT;
    }
  client->dma_inflight += request_count;
  __atomic_store_n (&ring->producer, producer + (uint32_t) request_count, __ATOMIC_RELEASE);
  return 0;
}

int
uet_vpp_client_submit_ip (uet_vpp_client_t *client, uint32_t dma_slot, uint32_t packet_length,
			  uint64_t request_id, uint64_t user_context)
{
  uet_vpp_client_tx_request_t request = {
    .dma_slot = dma_slot,
    .packet_length = packet_length,
    .request_id = request_id,
    .user_context = user_context,
  };

  return uet_vpp_client_submit_ip_batch (client, &request, 1);
}

int
uet_vpp_client_poll_batch (uet_vpp_client_t *client, uet_vpp_client_completion_t *completions,
			   size_t completion_capacity)
{
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, available, i;

  if (!client || !completions || completion_capacity == 0 || completion_capacity > INT32_MAX)
    return -EINVAL;

  ring = client->tx_completion_ring;
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_RELAXED);
  producer = __atomic_load_n (&ring->producer, __ATOMIC_ACQUIRE);
  available = producer - consumer;
  if (available > ring->size)
    return -EPROTO;
  available = available < completion_capacity ? available : completion_capacity;

  for (i = 0; i < available; i++)
    {
      uint32_t index = ring->mask ? (consumer + i) & ring->mask : (consumer + i) % ring->size;
      const uet_vpp_svm_tx_completion_t *wire = client->tx_completion_entries + index;
      uint32_t slot = wire->dma_slot;

      if (wire->reserved || !client->dma_map || slot >= client->header->dma_slot_count ||
	  client->dma_slot_states[slot] != UET_VPP_SLOT_INFLIGHT ||
	  client->dma_slot_request_ids[slot] != wire->request_id)
	return -EPROTO;
    }

  for (i = 0; i < available; i++)
    {
      uint32_t index = ring->mask ? (consumer + i) & ring->mask : (consumer + i) % ring->size;
      const uet_vpp_svm_tx_completion_t *wire = client->tx_completion_entries + index;
      uet_vpp_client_completion_t *completion = completions + i;
      uint32_t slot = wire->dma_slot;

      completion->status = wire->status;
      completion->completed_length = wire->completed_length;
      completion->dma_slot = slot;
      completion->request_id = wire->request_id;
      completion->user_context = wire->user_context;
      client->dma_slot_request_ids[slot] = 0;
      client->dma_slot_states[slot] = UET_VPP_SLOT_FREE;
    }
  if (available)
    {
      client->dma_inflight -= available;
      __atomic_store_n (&ring->consumer, consumer + available, __ATOMIC_RELEASE);
      return available;
    }
  return 0;
}

int
uet_vpp_client_poll (uet_vpp_client_t *client, uet_vpp_client_completion_t *completion)
{
  return uet_vpp_client_poll_batch (client, completion, 1);
}

static int
uet_vpp_client_rx_decode (uet_vpp_client_t *client, const uet_vpp_svm_rx_desc_t *desc,
			  uet_vpp_client_rx_t *rx)
{
  uint64_t total = 0;

  if (!desc->rx_id || desc->reserved || desc->release_token >= client->rx_ring->size ||
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
uet_vpp_client_poll_rx_batch (uet_vpp_client_t *client, uet_vpp_client_rx_t *rx, size_t rx_capacity)
{
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer, available;

  if (!client || !rx || !rx_capacity || rx_capacity > UINT32_MAX)
    return -EINVAL;
  if (!client->dma_map)
    return -ENXIO;

  ring = client->rx_ring;
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
      int rv = uet_vpp_client_rx_decode (client, client->rx_descs + index, rx + i);

      if (rv)
	return rv;
    }
  for (uint32_t i = 0; i < available; i++)
    {
      uint32_t token = rx[i].release_token;

      if (client->rx_token_ids[token])
	{
	  while (i)
	    client->rx_token_ids[rx[--i].release_token] = 0;
	  return -EPROTO;
	}
      client->rx_token_ids[token] = rx[i].rx_id;
    }
  if (available)
    {
      client->rx_inflight += available;
      __atomic_store_n (&ring->consumer, consumer + available, __ATOMIC_RELEASE);
    }
  return available;
}

int
uet_vpp_client_poll_rx (uet_vpp_client_t *client, uet_vpp_client_rx_t *rx)
{
  return uet_vpp_client_poll_rx_batch (client, rx, 1);
}

int
uet_vpp_client_release_rx_batch (uet_vpp_client_t *client, const uet_vpp_client_rx_t *rx,
				 size_t rx_count)
{
  uet_vpp_svm_spsc_ring_t *ring;
  uint32_t producer, consumer;

  if (!client || !rx || !rx_count || rx_count > UINT32_MAX || rx_count > client->rx_inflight)
    return -EINVAL;

  ring = client->rx_release_ring;
  producer = __atomic_load_n (&ring->producer, __ATOMIC_RELAXED);
  consumer = __atomic_load_n (&ring->consumer, __ATOMIC_ACQUIRE);
  if (producer - consumer > ring->size)
    return -EPROTO;
  if (rx_count > ring->size - (producer - consumer))
    return -EAGAIN;

  for (uint32_t i = 0; i < rx_count; i++)
    if (!rx[i].rx_id || rx[i].release_token >= client->header->rx_ring_size ||
	client->rx_token_ids[rx[i].release_token] != rx[i].rx_id)
      return -EINVAL;

  for (uint32_t i = 0; i < rx_count; i++)
    {
      uint32_t counter = producer + i;
      uint32_t index = ring->mask ? counter & ring->mask : counter % ring->size;
      uet_vpp_svm_rx_release_t *release = client->rx_release_entries + index;
      uint32_t token = rx[i].release_token;

      if (client->rx_token_ids[token] != rx[i].rx_id)
	{
	  while (i)
	    {
	      i--;
	      client->rx_token_ids[rx[i].release_token] = rx[i].rx_id;
	    }
	  return -EINVAL;
	}

      release->rx_id = rx[i].rx_id;
      release->release_token = token;
      release->reserved = 0;
      client->rx_token_ids[token] = 0;
    }
  client->rx_inflight -= rx_count;
  __atomic_store_n (&ring->producer, producer + (uint32_t) rx_count, __ATOMIC_RELEASE);
  return 0;
}

int
uet_vpp_client_release_rx (uet_vpp_client_t *client, const uet_vpp_client_rx_t *rx)
{
  return uet_vpp_client_release_rx_batch (client, rx, 1);
}
