/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uet_vpp_client.h>

#define UET_SPSC_NEGATIVE_WAIT_NS    (500ULL * 1000 * 1000)
#define UET_SPSC_NEGATIVE_TIMEOUT_NS (2ULL * 1000 * 1000 * 1000)

static uint64_t
monotonic_ns (void)
{
  struct timespec now;

  if (clock_gettime (CLOCK_MONOTONIC, &now) != 0)
    {
      perror ("clock_gettime");
      exit (1);
    }
  return (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
}

static int
expect_result (const char *operation, int actual, int expected)
{
  if (actual == expected)
    return 0;

  fprintf (stderr, "%s returned %d, expected %d\n", operation, actual, expected);
  return -1;
}

static int
completion_check (const uet_vpp_client_completion_t *completion,
		  const uet_vpp_client_tx_request_t *request)
{
  if (completion->status == UET_VPP_CLIENT_STATUS_INVALID_PACKET &&
      completion->completed_length == 0 && completion->dma_slot == request->dma_slot &&
      completion->request_id == request->request_id &&
      completion->user_context == request->user_context)
    return 0;

  fprintf (stderr, "invalid negative completion for request %" PRIu64 "\n", request->request_id);
  return -1;
}

static int
rx_ownership_check (uet_vpp_client_t *client, uint32_t rx_count, uint32_t ring_size)
{
  uet_vpp_client_rx_t *received = calloc (rx_count, sizeof (*received));
  uet_vpp_client_rx_t forged, duplicate[2];
  uint64_t deadline = monotonic_ns () + UET_SPSC_NEGATIVE_TIMEOUT_NS;
  uint32_t n_received = 0;
  int rc = -ENOMEM;

  if (!received)
    {
      perror ("calloc");
      return -1;
    }

  while (n_received < rx_count && monotonic_ns () < deadline)
    {
      rc = uet_vpp_client_poll_rx_batch (client, 0, received + n_received, rx_count - n_received);
      if (rc < 0)
	break;
      if (rc == 0)
	sched_yield ();
      else
	n_received += rc;
    }
  if (n_received != rx_count)
    {
      fprintf (stderr, "RX poll returned %u packets, expected %u (last result %d)\n", n_received,
	       rx_count, rc);
      goto failed;
    }
  for (uint32_t i = 0; i < rx_count; i++)
    if (!(received[i].flags & UET_VPP_CLIENT_RX_F_IP4) ||
	!(received[i].flags & UET_VPP_CLIENT_RX_F_UDP) ||
	(received[i].flags & UET_VPP_CLIENT_RX_F_IP6) || received[i].iov_count == 0 ||
	received[i].packet_length < 20)
      {
	fprintf (stderr, "invalid RX metadata at index %u\n", i);
	goto failed;
      }

  if (expect_result ("close with RX outstanding", uet_vpp_client_close (client), -EBUSY))
    goto failed;

  forged = received[0];
  forged.rx_id ^= UINT64_C (1) << 63;
  if (!forged.rx_id)
    forged.rx_id = 1;
  if (expect_result ("forged RX ID release", uet_vpp_client_release_rx (client, 0, &forged),
		     -EINVAL))
    goto failed;

  forged = received[0];
  forged.release_token = (forged.release_token + 1) % ring_size;
  if (expect_result ("forged RX token release", uet_vpp_client_release_rx (client, 0, &forged),
		     -EINVAL))
    goto failed;

  duplicate[0] = received[0];
  duplicate[1] = received[0];
  if (expect_result ("duplicate RX release batch",
		     uet_vpp_client_release_rx_batch (client, 0, duplicate, 2), -EINVAL) ||
      expect_result ("valid RX release batch",
		     uet_vpp_client_release_rx_batch (client, 0, received, rx_count), 0) ||
      expect_result ("stale RX release", uet_vpp_client_release_rx (client, 0, received), -EINVAL))
    goto failed;

  free (received);
  return 0;

failed:
  free (received);
  return -1;
}

int
main (int argc, char **argv)
{
  uet_vpp_client_t *client = 0;
  uet_vpp_client_info_t info;
  uet_vpp_client_tx_request_t duplicate[2];
  uet_vpp_client_tx_request_t *requests = 0;
  uet_vpp_client_completion_t completion;
  uet_vpp_client_completion_t *completions = 0;
  uet_vpp_client_rx_t rx = { .rx_id = 1 };
  struct timespec wait_time = {
    .tv_sec = 0,
    .tv_nsec = UET_SPSC_NEGATIVE_WAIT_NS,
  };
  uint32_t slot, unused_slot;
  uint32_t rx_count;
  uint64_t deadline;
  char *end = 0;
  unsigned long parsed_rx_count;
  size_t capacity, unused_capacity;
  void *data, *unused_data;
  int rc, rv = 1;

  if (argc != 4)
    {
      fprintf (stderr, "usage: %s <segment-name> <dma-socket> <rx-count>\n", argv[0]);
      return 2;
    }
  errno = 0;
  parsed_rx_count = strtoul (argv[3], &end, 10);
  if (errno || !end || *end || parsed_rx_count < 2 || parsed_rx_count > UINT32_MAX)
    {
      fprintf (stderr, "invalid RX count: %s\n", argv[3]);
      return 2;
    }
  rx_count = parsed_rx_count;

  rc = uet_vpp_client_open (&client, argv[1], &info);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_open failed: %d\n", rc);
      return 1;
    }
  if (info.queue_depth != info.dma_slot_count || info.queue_depth != info.tx_ring_size)
    {
      fprintf (stderr, "inconsistent SPSC and DMA depths\n");
      goto done;
    }
  if (rx_count > info.rx_ring_size)
    {
      fprintf (stderr, "RX count %u exceeds ring size %u\n", rx_count, info.rx_ring_size);
      goto done;
    }
  if (expect_result ("acquire before DMA map",
		     uet_vpp_client_acquire_dma (client, 0, &slot, &data, &capacity), -ENXIO) ||
      expect_result ("RX poll before DMA map", uet_vpp_client_poll_rx (client, 0, &rx), -ENXIO))
    goto done;

  rc = uet_vpp_client_map_dma (client, argv[2]);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_map_dma failed: %d\n", rc);
      goto done;
    }
  if (expect_result ("duplicate DMA map", uet_vpp_client_map_dma (client, argv[2]), -EALREADY) ||
      expect_result ("out-of-range release",
		     uet_vpp_client_release_dma (client, 0, info.dma_slot_count), -EINVAL) ||
      expect_result ("unowned submit", uet_vpp_client_submit_ip (client, 0, 0, 20, 1, 0), -EPERM) ||
      expect_result ("unowned release", uet_vpp_client_release_dma (client, 0, 0), -EPERM) ||
      expect_result ("unowned RX release", uet_vpp_client_release_rx (client, 0, &rx), -EINVAL))
    goto done;

  rc = uet_vpp_client_acquire_dma (client, 0, &slot, &data, &capacity);
  if (rc)
    {
      fprintf (stderr, "DMA acquisition failed: %d\n", rc);
      goto done;
    }
  if (expect_result ("undersized IP submit", uet_vpp_client_submit_ip (client, 0, slot, 19, 2, 0),
		     -EMSGSIZE) ||
      expect_result ("release after rejected submit", uet_vpp_client_release_dma (client, 0, slot),
		     0))
    goto done;

  rc = uet_vpp_client_acquire_dma (client, 0, &slot, &data, &capacity);
  if (rc || capacity < 20)
    {
      fprintf (stderr, "DMA acquisition for duplicate batch failed: %d\n", rc);
      goto done;
    }
  memset (data, 0, 20);
  duplicate[0] = (uet_vpp_client_tx_request_t){
    .dma_slot = slot,
    .packet_length = 20,
    .request_id = 3,
    .user_context = 0x33,
  };
  duplicate[1] = duplicate[0];
  duplicate[1].request_id++;
  if (expect_result ("duplicate-slot batch",
		     uet_vpp_client_submit_ip_batch (client, 0, duplicate, 2), -EINVAL) ||
      expect_result ("release after rejected batch", uet_vpp_client_release_dma (client, 0, slot),
		     0))
    goto done;

  rc = uet_vpp_client_acquire_dma (client, 0, &slot, &data, &capacity);
  if (rc || capacity < 20)
    {
      fprintf (stderr, "DMA acquisition for malformed packet failed: %d\n", rc);
      goto done;
    }
  memset (data, 0, 20);
  duplicate[0] = (uet_vpp_client_tx_request_t){
    .dma_slot = slot,
    .packet_length = 20,
    .request_id = 5,
    .user_context = 0x55,
  };
  if (expect_result ("malformed IP submit",
		     uet_vpp_client_submit_ip_batch (client, 0, duplicate, 1), 0) ||
      expect_result ("close with TX in flight", uet_vpp_client_close (client), -EBUSY) ||
      expect_result ("release with TX in flight", uet_vpp_client_release_dma (client, 0, slot),
		     -EPERM))
    goto done;

  deadline = monotonic_ns () + UET_SPSC_NEGATIVE_TIMEOUT_NS;
  do
    {
      rc = uet_vpp_client_poll (client, 0, &completion);
      if (rc == 0)
	sched_yield ();
    }
  while (rc == 0 && monotonic_ns () < deadline);
  if (rc != 1 || completion_check (&completion, duplicate))
    goto done;
  if (expect_result ("release after completion", uet_vpp_client_release_dma (client, 0, slot),
		     -EPERM))
    goto done;

  requests = calloc (info.queue_depth, sizeof (*requests));
  completions = calloc (info.queue_depth, sizeof (*completions));
  if (!requests || !completions)
    {
      perror ("calloc");
      goto done;
    }
  for (uint32_t i = 0; i < info.queue_depth; i++)
    {
      rc = uet_vpp_client_acquire_dma (client, 0, &slot, &data, &capacity);
      if (rc || capacity < 20)
	{
	  fprintf (stderr, "DMA pool exhaustion setup failed at slot %u: %d\n", i, rc);
	  goto done;
	}
      memset (data, 0, 20);
      requests[i] = (uet_vpp_client_tx_request_t){
	.dma_slot = slot,
	.packet_length = 20,
	.request_id = UINT64_C (0x100000000) + i,
	.user_context = UINT64_C (0x200000000) + i,
      };
    }
  if (expect_result (
	"DMA slot exhaustion",
	uet_vpp_client_acquire_dma (client, 0, &unused_slot, &unused_data, &unused_capacity),
	-EAGAIN) ||
      expect_result ("full-depth TX batch",
		     uet_vpp_client_submit_ip_batch (client, 0, requests, info.queue_depth), 0) ||
      expect_result ("close with full-depth TX batch", uet_vpp_client_close (client), -EBUSY))
    goto done;

  while (nanosleep (&wait_time, &wait_time) != 0 && errno == EINTR)
    ;
  rc = uet_vpp_client_poll_batch (client, 0, completions, info.queue_depth);
  if (rc != (int) info.queue_depth)
    {
      fprintf (stderr, "completion ring returned %d entries, expected %u\n", rc, info.queue_depth);
      goto done;
    }
  for (uint32_t i = 0; i < info.queue_depth; i++)
    if (completion_check (completions + i, requests + i))
      goto done;

  if (rx_ownership_check (client, rx_count, info.rx_ring_size))
    goto done;

  rc = uet_vpp_client_close (client);
  if (rc)
    {
      fprintf (stderr, "final client close failed: %d\n", rc);
      goto done;
    }
  client = 0;
  printf ("UET SPSC negative smoke passed: ownership checks, %u RX packets, and %u-entry "
	  "completion ring\n",
	  rx_count, info.queue_depth);
  rv = 0;

done:
  free (completions);
  free (requests);
  if (client)
    uet_vpp_client_close (client);
  return rv;
}
