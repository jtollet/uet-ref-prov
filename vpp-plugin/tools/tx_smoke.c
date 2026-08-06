/* SPDX-License-Identifier: Apache-2.0 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uet_vpp_client.h>

#define UET_TX_SMOKE_IP_PROTOCOL 253
#define UET_TX_SMOKE_UDP_PORT	 49150
#define UET_TX_SMOKE_TIMEOUT_NS	 (2ULL * 1000 * 1000 * 1000)

static uint64_t
monotonic_ns (void)
{
  struct timespec ts;

  if (clock_gettime (CLOCK_MONOTONIC, &ts) != 0)
    {
      perror ("clock_gettime");
      exit (1);
    }
  return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static uint16_t
ip4_checksum (const uint8_t *header, size_t length)
{
  uint32_t sum = 0;

  for (size_t i = 0; i < length; i += 2)
    sum += ((uint16_t) header[i] << 8) | header[i + 1];
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return htons ((uint16_t) ~sum);
}

static void
store_u16 (void *destination, uint16_t value)
{
  memcpy (destination, &value, sizeof (value));
}

static void
store_u32 (void *destination, uint32_t value)
{
  memcpy (destination, &value, sizeof (value));
}

static uint32_t
packet_build (uint8_t *packet, size_t capacity, uint64_t request_id)
{
  static const uint8_t src4[4] = { 192, 0, 2, 1 };
  static const uint8_t dst4[4] = { 198, 51, 100, 1 };
  static const uint8_t src6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
  static const uint8_t dst6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 };
  const int is_ip6 = request_id & 1;
  const int is_udp = request_id & 2;
  const uint32_t ip_header_length = is_ip6 ? 40 : 20;
  const uint32_t l4_length = is_udp ? 12 : 4;
  const uint32_t packet_length = ip_header_length + l4_length;
  uint8_t *l4;

  if (capacity < packet_length)
    return 0;
  memset (packet, 0, packet_length);

  if (is_ip6)
    {
      packet[0] = 0x60;
      store_u16 (packet + 4, htons (l4_length));
      packet[6] = is_udp ? IPPROTO_UDP : UET_TX_SMOKE_IP_PROTOCOL;
      packet[7] = 64;
      memcpy (packet + 8, src6, sizeof (src6));
      memcpy (packet + 24, dst6, sizeof (dst6));
    }
  else
    {
      packet[0] = 0x45;
      store_u16 (packet + 2, htons (packet_length));
      packet[8] = 64;
      packet[9] = is_udp ? IPPROTO_UDP : UET_TX_SMOKE_IP_PROTOCOL;
      memcpy (packet + 12, src4, sizeof (src4));
      memcpy (packet + 16, dst4, sizeof (dst4));
      store_u16 (packet + 10, ip4_checksum (packet, ip_header_length));
    }

  l4 = packet + ip_header_length;
  if (is_udp)
    {
      store_u16 (l4, htons ((uint16_t) (10000 + request_id % 1000)));
      store_u16 (l4 + 2, htons (UET_TX_SMOKE_UDP_PORT));
      store_u16 (l4 + 4, htons (l4_length));
      store_u16 (l4 + 6, htons (0x1234)); /* The plugin normalizes it to zero. */
      store_u32 (l4 + 8, htonl ((uint32_t) request_id));
    }
  else
    store_u32 (l4, htonl ((uint32_t) request_id));

  return packet_length;
}

typedef struct
{
  uet_vpp_client_t *client;
  uint64_t count;
  uint32_t channel;
  int result;
} uet_tx_smoke_thread_t;

static void *
tx_channel_run (void *opaque)
{
  uet_tx_smoke_thread_t *thread = opaque;
  uint64_t deadline = monotonic_ns () + UET_TX_SMOKE_TIMEOUT_NS;

  thread->result = 1;
  for (uint64_t sequence = 1; sequence <= thread->count; sequence++)
    {
      uet_vpp_client_completion_t completion;
      uint64_t request_id = ((uint64_t) thread->channel << 56) | sequence;
      uint32_t slot, packet_length;
      size_t capacity;
      void *packet;
      int rc;

      do
	rc =
	  uet_vpp_client_acquire_dma (thread->client, thread->channel, &slot, &packet, &capacity);
      while (rc == -EAGAIN && monotonic_ns () < deadline);
      if (rc || !(packet_length = packet_build (packet, capacity, request_id)))
	{
	  fprintf (stderr,
		   "channel %u TX slot acquisition/build failed for request %" PRIu64 ": %d\n",
		   thread->channel, request_id, rc);
	  return 0;
	}

      rc = uet_vpp_client_submit_ip (thread->client, thread->channel, slot, packet_length,
				     request_id, request_id ^ thread->count);
      if (rc)
	{
	  fprintf (stderr, "channel %u TX submit failed for request %" PRIu64 ": %d\n",
		   thread->channel, request_id, rc);
	  uet_vpp_client_release_dma (thread->client, thread->channel, slot);
	  return 0;
	}

      do
	{
	  rc = uet_vpp_client_poll (thread->client, thread->channel, &completion);
	  if (rc == 0)
	    usleep (50);
	}
      while (rc == 0 && monotonic_ns () < deadline);
      if (rc != 1 || completion.status != UET_VPP_CLIENT_STATUS_OK ||
	  completion.completed_length != packet_length || completion.dma_slot != slot ||
	  completion.request_id != request_id ||
	  completion.user_context != (request_id ^ thread->count))
	{
	  fprintf (stderr, "channel %u invalid TX completion for request %" PRIu64 "\n",
		   thread->channel, request_id);
	  return 0;
	}
      deadline = monotonic_ns () + UET_TX_SMOKE_TIMEOUT_NS;
    }

  thread->result = 0;
  return 0;
}

int
main (int argc, char **argv)
{
  uet_vpp_client_t *client = 0;
  uet_vpp_client_info_t info;
  uet_tx_smoke_thread_t *thread_contexts = 0;
  pthread_t *threads = 0;
  uint64_t count;
  uint32_t threads_started = 0;
  char *end = 0;
  int rc, rv = 1;

  if (argc != 4)
    {
      fprintf (stderr, "usage: %s <segment-name> <packet-count> <dma-socket>\n", argv[0]);
      return 2;
    }

  errno = 0;
  count = strtoull (argv[2], &end, 10);
  if (errno || !end || *end || !count || count >= (UINT64_C (1) << 56))
    {
      fprintf (stderr, "invalid packet count: %s\n", argv[2]);
      return 2;
    }

  rc = uet_vpp_client_open (&client, argv[1], &info);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_open failed: %d\n", rc);
      return 1;
    }
  rc = uet_vpp_client_map_dma (client, argv[3]);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_map_dma failed: %d\n", rc);
      goto done;
    }

  threads = calloc (info.channel_count, sizeof (*threads));
  thread_contexts = calloc (info.channel_count, sizeof (*thread_contexts));
  if (!threads || !thread_contexts)
    {
      perror ("calloc");
      goto done;
    }
  for (uint32_t channel = 0; channel < info.channel_count; channel++)
    {
      thread_contexts[channel] = (uet_tx_smoke_thread_t){
	.client = client,
	.count = count,
	.channel = channel,
      };
      rc = pthread_create (threads + channel, 0, tx_channel_run, thread_contexts + channel);
      if (rc)
	{
	  fprintf (stderr, "pthread_create for channel %u failed: %s\n", channel, strerror (rc));
	  break;
	}
      threads_started++;
    }
  for (uint32_t channel = 0; channel < threads_started; channel++)
    {
      int join_rc = pthread_join (threads[channel], 0);

      if (join_rc && !rc)
	rc = join_rc;
    }
  if (threads_started != info.channel_count || rc)
    goto done;
  for (uint32_t channel = 0; channel < info.channel_count; channel++)
    if (thread_contexts[channel].result)
      goto done;

  printf ("UET routed TX smoke passed: %" PRIu64
	  " packets on each of %u channels, IPv4/IPv6 native and UDP, %u replacement slots per "
	  "channel\n",
	  count, info.channel_count, info.dma_slot_count);
  rv = 0;

done:
  free (thread_contexts);
  free (threads);
  rc = uet_vpp_client_close (client);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_close failed: %d\n", rc);
      rv = 1;
    }
  return rv;
}
