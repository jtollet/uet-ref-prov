/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vppinfra/mem.h>

#include <uet_vpp_client.h>

static int
expected_result (const char *mode, int *hold)
{
  *hold = 0;
  if (!strcmp (mode, "expect-success"))
    return 0;
  if (!strcmp (mode, "expect-existing-heap"))
    return 0;
  if (!strcmp (mode, "expect-busy"))
    return -EBUSY;
  if (!strcmp (mode, "expect-owner-dead"))
    return -EOWNERDEAD;
  if (!strcmp (mode, "expect-double-busy"))
    return 0;
  if (!strcmp (mode, "hold") || !strcmp (mode, "hold-dma"))
    {
      *hold = 1;
      return 0;
    }
  return 1;
}

int
main (int argc, char **argv)
{
  uet_vpp_client_t *client = 0;
  int expected, hold, rc;

  if ((argc != 3 && argc != 4) || (expected = expected_result (argv[2], &hold)) > 0 ||
      (argc == 4 && strcmp (argv[2], "hold-dma")) || (argc == 3 && !strcmp (argv[2], "hold-dma")))
    {
      fprintf (stderr,
	       "usage: %s <segment-name> "
	       "<expect-success|expect-existing-heap|expect-busy|expect-owner-dead|"
	       "expect-double-busy|hold|hold-dma> "
	       "[dma-socket]\n",
	       argv[0]);
      return 2;
    }

  if (!strcmp (argv[2], "expect-existing-heap") && !clib_mem_init (0, 1U << 20))
    {
      fprintf (stderr, "failed to initialize the existing VPP heap\n");
      return 1;
    }

  rc = uet_vpp_client_open (&client, argv[1], 0);
  if (rc != expected)
    {
      fprintf (stderr, "uet_vpp_client_open returned %d, expected %d\n", rc, expected);
      if (!rc)
	uet_vpp_client_close (client);
      return 1;
    }
  if (rc)
    return 0;

  if (!strcmp (argv[2], "expect-double-busy"))
    {
      uet_vpp_client_t *second = 0;

      rc = uet_vpp_client_open (&second, argv[1], 0);
      if (rc != -EBUSY)
	{
	  fprintf (stderr, "second uet_vpp_client_open returned %d, expected %d\n", rc, -EBUSY);
	  if (!rc)
	    uet_vpp_client_close (second);
	  uet_vpp_client_close (client);
	  return 1;
	}
    }

  if (argc == 4)
    {
      rc = uet_vpp_client_map_dma (client, argv[3]);
      if (rc)
	{
	  fprintf (stderr, "uet_vpp_client_map_dma failed: %d\n", rc);
	  uet_vpp_client_close (client);
	  return 1;
	}
    }

  if (hold)
    {
      printf ("owner ready: pid %d\n", getpid ());
      fflush (stdout);
      for (;;)
	pause ();
    }

  rc = uet_vpp_client_close (client);
  if (rc)
    {
      fprintf (stderr, "uet_vpp_client_close failed: %d\n", rc);
      return 1;
    }
  return 0;
}
