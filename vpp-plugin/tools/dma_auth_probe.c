/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <uet/dma_abi.h>

int
main (int argc, char **argv)
{
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  uet_vpp_dma_reply_t reply = { 0 };
  char control[CMSG_SPACE (sizeof (int))] = { 0 };
  struct iovec iov = { .iov_base = &reply, .iov_len = sizeof (reply) };
  struct msghdr message = {
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = control,
    .msg_controllen = sizeof (control),
  };
  int socket_fd;
  int received_fd = -1;
  ssize_t received;

  if (argc != 2 || !argv[1][0] || strlen (argv[1]) >= sizeof (address.sun_path))
    {
      fprintf (stderr, "usage: %s <dma-socket>\n", argv[0]);
      return 2;
    }

  socket_fd = socket (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket_fd < 0)
    {
      perror ("socket");
      return 1;
    }
  memcpy (address.sun_path, argv[1], strlen (argv[1]) + 1);
  if (connect (socket_fd, (struct sockaddr *) &address, sizeof (address)) != 0)
    {
      perror ("connect");
      close (socket_fd);
      return 1;
    }

  received = recvmsg (socket_fd, &message, MSG_CMSG_CLOEXEC);
  if (received != sizeof (reply) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
    {
      if (received < 0)
	perror ("recvmsg");
      else
	fprintf (stderr, "invalid DMA reply length %zd\n", received);
      close (socket_fd);
      return 1;
    }

  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR (&message); cmsg; cmsg = CMSG_NXTHDR (&message, cmsg))
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
	cmsg->cmsg_len >= CMSG_LEN (sizeof (received_fd)))
      {
	memcpy (&received_fd, CMSG_DATA (cmsg), sizeof (received_fd));
	break;
      }
  close (socket_fd);

  if (received_fd >= 0)
    close (received_fd);
  if (reply.magic != UET_VPP_DMA_ABI_MAGIC || reply.version != UET_VPP_DMA_ABI_VERSION ||
      reply.status != -EACCES || received_fd >= 0)
    {
      fprintf (stderr, "unauthorized DMA request returned status %d and fd %d\n", reply.status,
	       received_fd);
      return 1;
    }

  printf ("unauthorized DMA client rejected without a file descriptor\n");
  return 0;
}
