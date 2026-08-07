/* SPDX-License-Identifier: Apache-2.0 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uet_vpp_client.h>

#define UET_ENDPOINT_SMOKE_RI	   15
#define UET_ENDPOINT_SMOKE_PORT	   49150
#define UET_ENDPOINT_SMOKE_WAIT_NS UINT64_C (5000000000)

#define UET_ENDPOINT_SMOKE_PDS_TYPE_SHIFT 11
#define UET_ENDPOINT_SMOKE_PDS_TYPE_MASK  0x1f
#define UET_ENDPOINT_SMOKE_PDS_SYN	  0x04
#define UET_ENDPOINT_SMOKE_PDS_NACK_RUDI  0x08
#define UET_ENDPOINT_SMOKE_PDC_BITS	  6
#define UET_ENDPOINT_SMOKE_RUDI_BITS	  22

typedef enum
{
  UET_ENDPOINT_ROUTE_RUD_SYN = 1U << 0,
  UET_ENDPOINT_ROUTE_RUD_ESTABLISHED = 1U << 1,
  UET_ENDPOINT_ROUTE_ACK = 1U << 2,
  UET_ENDPOINT_ROUTE_NACK_PDC = 1U << 3,
  UET_ENDPOINT_ROUTE_CTRL = 1U << 4,
  UET_ENDPOINT_ROUTE_RUDI_REQUEST = 1U << 5,
  UET_ENDPOINT_ROUTE_RUDI_RESPONSE = 1U << 6,
  UET_ENDPOINT_ROUTE_NACK_RUDI = 1U << 7,
  UET_ENDPOINT_ROUTE_UUD_REQUEST = 1U << 8,
  UET_ENDPOINT_ROUTE_SNG_REQUEST = 1U << 9,
  UET_ENDPOINT_ROUTE_SNG_ACK = 1U << 10,
} uet_endpoint_route_t;

#define UET_ENDPOINT_ROUTE_ALL ((1U << 11) - 1)

static int
usage (const char *program)
{
  fprintf (stderr, "usage: %s <segment-name> <dma-socket>\n", program);
  fprintf (stderr,
	   "       %s <segment-name> <dma-socket> "
	   "<hold|expect-collision|control> <pid-on-fep>\n",
	   program);
  return 2;
}

static uint64_t
monotonic_ns (void)
{
  struct timespec now;

  if (clock_gettime (CLOCK_MONOTONIC, &now))
    return 0;
  return (uint64_t) now.tv_sec * UINT64_C (1000000000) + now.tv_nsec;
}

static size_t
copy_rx_prefix (const uet_vpp_client_rx_t *rx, uint8_t *dst, size_t capacity)
{
  size_t copied = 0;

  for (uint16_t i = 0; i < rx->iov_count && copied < capacity; i++)
    {
      size_t length = rx->iov[i].length;

      if (length > capacity - copied)
	length = capacity - copied;
      memcpy (dst + copied, rx->iov[i].base, length);
      copied += length;
    }
  return copied;
}

static int
validate_endpoint (const uint8_t *packet, size_t copied, size_t ses_offset, uint16_t expected_pid)
{
  uint16_t value;

  if (copied < ses_offset + 12)
    return -EPROTO;
  memcpy (&value, packet + ses_offset + 8, sizeof (value));
  if ((ntohs (value) & 0x0fff) != expected_pid)
    return -EPROTO;
  memcpy (&value, packet + ses_offset + 10, sizeof (value));
  if ((ntohs (value) & 0x0fff) != UET_ENDPOINT_SMOKE_RI)
    return -EPROTO;
  return 0;
}

static int
validate_pdc (const uint8_t *packet, size_t copied, size_t pds_offset, uint16_t expected_namespace)
{
  uint16_t value;

  if (copied < pds_offset + 12)
    return -EPROTO;
  memcpy (&value, packet + pds_offset + 10, sizeof (value));
  return (ntohs (value) >> UET_ENDPOINT_SMOKE_PDC_BITS) == expected_namespace ? 0 : -EPROTO;
}

static int
validate_sng (const uint8_t *packet, size_t copied, size_t pds_offset, uint16_t expected_namespace,
	      int ack)
{
  uint16_t pid, index;

  if (copied < pds_offset + 12)
    return -EPROTO;
  memcpy (&pid, packet + pds_offset + 8, sizeof (pid));
  memcpy (&index, packet + pds_offset + 10, sizeof (index));
  pid = ntohs (pid);
  index = ntohs (index);

  if (pid != (ack ? expected_namespace : 0) || index != UET_ENDPOINT_SMOKE_RI)
    return -EPROTO;
  return 0;
}

static int
validate_pkt_id (const uint8_t *packet, size_t copied, size_t pds_offset,
		 uint16_t expected_namespace)
{
  uint32_t value;

  if (copied < pds_offset + 8)
    return -EPROTO;
  memcpy (&value, packet + pds_offset + 4, sizeof (value));
  return (ntohl (value) >> UET_ENDPOINT_SMOKE_RUDI_BITS) == expected_namespace ? 0 : -EPROTO;
}

static int
validate_rx (const uet_vpp_client_rx_t *rx, uint16_t expected_namespace, uint32_t *route)
{
  uint8_t packet[96];
  uint16_t prologue, value;
  uint8_t flags, type;
  size_t copied, ip_header_size, pds_offset;

  if (!(rx->flags & UET_VPP_CLIENT_RX_F_IP4) || !(rx->flags & UET_VPP_CLIENT_RX_F_UDP) ||
      (rx->flags & UET_VPP_CLIENT_RX_F_IP6))
    return -EPROTO;
  copied = copy_rx_prefix (rx, packet, sizeof (packet));
  if (copied < 20 || (packet[0] >> 4) != 4)
    return -EPROTO;
  ip_header_size = (packet[0] & 0x0f) * 4;
  pds_offset = ip_header_size + 8;
  if (ip_header_size < 20 || copied < pds_offset + sizeof (prologue))
    return -EPROTO;
  memcpy (&value, packet + ip_header_size + 2, sizeof (value));
  if (ntohs (value) != UET_ENDPOINT_SMOKE_PORT)
    return -EPROTO;
  memcpy (&prologue, packet + pds_offset, sizeof (prologue));
  prologue = ntohs (prologue);
  type = (prologue >> UET_ENDPOINT_SMOKE_PDS_TYPE_SHIFT) & UET_ENDPOINT_SMOKE_PDS_TYPE_MASK;
  flags = prologue & 0x7f;

  switch (type)
    {
    case 2:  /* RUD request */
    case 3:  /* ROD request */
    case 13: /* RUD request with CC */
    case 14: /* ROD request with CC */
      if (flags & UET_ENDPOINT_SMOKE_PDS_SYN)
	{
	  size_t ses_offset = pds_offset + 12 + ((type == 13 || type == 14) ? 4 : 0);

	  *route = UET_ENDPOINT_ROUTE_RUD_SYN;
	  return validate_endpoint (packet, copied, ses_offset, expected_namespace);
	}
      memcpy (&value, packet + pds_offset + 10, sizeof (value));
      if (!(ntohs (value) >> UET_ENDPOINT_SMOKE_PDC_BITS))
	{
	  *route = UET_ENDPOINT_ROUTE_SNG_REQUEST;
	  return validate_sng (packet, copied, pds_offset, expected_namespace, 0);
	}
      *route = UET_ENDPOINT_ROUTE_RUD_ESTABLISHED;
      return validate_pdc (packet, copied, pds_offset, expected_namespace);
    case 4: /* RUDI request */
      *route = UET_ENDPOINT_ROUTE_RUDI_REQUEST;
      return validate_endpoint (packet, copied, pds_offset + 8, expected_namespace);
    case 5: /* RUDI response */
      *route = UET_ENDPOINT_ROUTE_RUDI_RESPONSE;
      return validate_pkt_id (packet, copied, pds_offset, expected_namespace);
    case 6: /* UUD request */
      *route = UET_ENDPOINT_ROUTE_UUD_REQUEST;
      return validate_endpoint (packet, copied, pds_offset + 4, expected_namespace);
    case 7: /* ACK */
    case 8: /* ACK with CC */
    case 9: /* ACK with CCX */
      memcpy (&value, packet + pds_offset + 8, sizeof (value));
      if (ntohs (value) == expected_namespace)
	{
	  *route = UET_ENDPOINT_ROUTE_SNG_ACK;
	  return validate_sng (packet, copied, pds_offset, expected_namespace, 1);
	}
      *route = UET_ENDPOINT_ROUTE_ACK;
      return validate_pdc (packet, copied, pds_offset, expected_namespace);
    case 10: /* NACK */
    case 12: /* NACK with CCX */
      if (flags & UET_ENDPOINT_SMOKE_PDS_NACK_RUDI)
	{
	  *route = UET_ENDPOINT_ROUTE_NACK_RUDI;
	  return validate_pkt_id (packet, copied, pds_offset, expected_namespace);
	}
      *route = UET_ENDPOINT_ROUTE_NACK_PDC;
      return validate_pdc (packet, copied, pds_offset, expected_namespace);
    case 11: /* PDC control */
      *route = UET_ENDPOINT_ROUTE_CTRL;
      return validate_pdc (packet, copied, pds_offset, expected_namespace);
    default:
      return -EPROTO;
    }
}

int
main (int argc, char **argv)
{
  uet_vpp_client_endpoint_t endpoint = {
    .ip_version = 4,
    .absolute = 0,
    .resource_index = UET_ENDPOINT_SMOKE_RI,
    .job_id = 0,
  };
  const struct timespec pause_time = {
    .tv_nsec = 1000000,
  };
  uet_vpp_client_t *client = 0;
  uet_vpp_client_info_t info = { 0 };
  uet_vpp_client_rx_t rx;
  uint64_t deadline;
  uint32_t received_routes = 0;
  uint32_t rx_channel = 0;
  unsigned long requested_pid = 0;
  enum
  {
    MODE_RECEIVE,
    MODE_HOLD,
    MODE_EXPECT_COLLISION,
    MODE_CONTROL,
  } mode = MODE_RECEIVE;
  int endpoint_added = 0;
  int rc, rv = 1;

  if (argc == 5)
    {
      char *end = 0;

      if (!strcmp (argv[3], "hold"))
	mode = MODE_HOLD;
      else if (!strcmp (argv[3], "expect-collision"))
	mode = MODE_EXPECT_COLLISION;
      else if (!strcmp (argv[3], "control"))
	mode = MODE_CONTROL;
      else
	return usage (argv[0]);
      errno = 0;
      requested_pid = strtoul (argv[4], &end, 0);
      if (errno || end == argv[4] || *end || requested_pid > 0x0fff)
	return usage (argv[0]);
    }
  else if (argc != 3)
    return usage (argv[0]);
  rc = uet_vpp_client_open (&client, argv[1], &info);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_open failed: %d\n", rc);
      return 1;
    }
  rc = uet_vpp_client_set_pds_sng (client, mode == MODE_RECEIVE);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_set_pds_sng failed: %d\n", rc);
      goto done;
    }
  rc = uet_vpp_client_map_dma (client, argv[2]);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_map_dma failed: %d\n", rc);
      goto done;
    }
  endpoint.pid_on_fep = mode == MODE_RECEIVE ? info.client_namespace : requested_pid;
  if (inet_pton (AF_INET, "198.18.0.1", endpoint.ip_address) != 1)
    goto done;
  rc = uet_vpp_client_endpoint_add (client, &endpoint);
  if (mode == MODE_EXPECT_COLLISION)
    {
      if (rc != -EADDRINUSE)
	{
	  fprintf (stderr, "endpoint add returned %d, expected %d\n", rc, -EADDRINUSE);
	  if (!rc)
	    endpoint_added = 1;
	  goto done;
	}
      printf ("endpoint collision pid %u passed\n", endpoint.pid_on_fep);
      rv = 0;
      goto done;
    }
  if (rc)
    {
      fprintf (stderr, "endpoint add failed: %d\n", rc);
      goto done;
    }
  endpoint_added = 1;
  if (mode == MODE_CONTROL)
    {
      printf ("endpoint control pid %u passed\n", endpoint.pid_on_fep);
      rv = 0;
      goto done;
    }
  if (mode == MODE_HOLD)
    {
      printf ("endpoint control ready: pid %u\n", endpoint.pid_on_fep);
      fflush (stdout);
      for (;;)
	pause ();
    }
  printf ("endpoint ready: namespace %u\n", info.client_namespace);
  fflush (stdout);

  deadline = monotonic_ns () + UET_ENDPOINT_SMOKE_WAIT_NS;
  do
    {
      for (uint32_t channel = 0; channel < info.channel_count; channel++)
	{
	  rc = uet_vpp_client_poll_rx (client, channel, &rx);
	  if (rc < 0)
	    {
	      fprintf (stderr, "RX poll failed on channel %u: %d\n", channel, rc);
	      goto done;
	    }
	  if (rc == 1)
	    {
	      uint32_t route = 0;

	      rx_channel = channel;
	      rc = validate_rx (&rx, info.client_namespace, &route);
	      if (rc)
		{
		  fprintf (stderr, "received packet did not match endpoint namespace %u\n",
			   info.client_namespace);
		  uet_vpp_client_release_rx (client, rx_channel, &rx);
		  goto done;
		}
	      if (received_routes & route)
		{
		  fprintf (stderr, "received duplicate endpoint route 0x%x\n", route);
		  uet_vpp_client_release_rx (client, rx_channel, &rx);
		  goto done;
		}
	      rc = uet_vpp_client_release_rx (client, rx_channel, &rx);
	      if (rc)
		{
		  fprintf (stderr, "RX release failed: %d\n", rc);
		  goto done;
		}
	      received_routes |= route;
	      if (received_routes == UET_ENDPOINT_ROUTE_ALL)
		{
		  rv = 0;
		  goto done;
		}
	    }
	}
      nanosleep (&pause_time, 0);
    }
  while (monotonic_ns () < deadline);
  fprintf (stderr, "timed out waiting for endpoint routes: received 0x%x, expected 0x%x\n",
	   received_routes, UET_ENDPOINT_ROUTE_ALL);

done:
  if (endpoint_added)
    {
      rc = uet_vpp_client_endpoint_del (client, &endpoint);
      if (rc)
	{
	  fprintf (stderr, "endpoint delete failed: %d\n", rc);
	  rv = 1;
	}
    }
  rc = uet_vpp_client_close (client);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_close failed: %d\n", rc);
      rv = 1;
    }
  if (!rv && mode == MODE_RECEIVE)
    printf ("endpoint RX namespace %u passed\n", info.client_namespace);
  return rv;
}
