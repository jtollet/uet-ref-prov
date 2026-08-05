/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <uet/uet.h>

#include <vlib/file.h>
#include <vlib/physmem_funcs.h>
#include <vlib/unix/unix.h>

typedef struct
{
  pid_t pid;
  uid_t uid;
  gid_t gid;
} uet_dma_peer_credentials_t;

static int
uet_dma_peer_authorize (clib_socket_t *client, uet_dma_peer_credentials_t *credentials)
{
  uet_main_t *um = &uet_main;
  socklen_t credentials_length = sizeof (*credentials);

  if (getsockopt (client->fd, SOL_SOCKET, SO_PEERCRED, credentials, &credentials_length) < 0)
    return -errno;
  if (credentials_length != sizeof (*credentials) || credentials->pid <= 0)
    return -EACCES;

  for (u32 worker = 0; worker < um->svm_channel_count; worker++)
    {
      u32 thread_index = vlib_get_worker_thread_index (worker);
      uet_worker_t *uw = vec_elt_at_index (um->workers, thread_index);

      if (uw->svm_attached && uw->svm_segment.sh &&
	  clib_atomic_load_acq_n (&uw->svm_segment.sh->ready) &&
	  clib_atomic_load_acq_n (&uw->svm_header->owner_pid) == credentials->pid)
	return 0;
    }

  return -EACCES;
}

static clib_error_t *
uet_dma_accept_ready (clib_file_t *uf)
{
  uet_main_t *um = &uet_main;
  clib_socket_t client = { 0 };
  uet_vpp_dma_reply_t reply = {
    .magic = UET_VPP_DMA_ABI_MAGIC,
    .version = UET_VPP_DMA_ABI_VERSION,
    .status = -ENOENT,
  };
  clib_error_t *err;
  clib_error_t *close_err;
  uet_dma_peer_credentials_t credentials = { 0 };
  int authorization_status;
  int fd = -1;

  err = clib_socket_accept (um->dma_listener, &client);
  if (err)
    {
      uet_log_warn ("failed to accept DMA client: %U", format_clib_error, err);
      return err;
    }

  authorization_status = uet_dma_peer_authorize (&client, &credentials);
  if (authorization_status)
    {
      reply.status = authorization_status;
      um->dma_rejected_clients++;
      uet_log_warn ("rejected DMA client pid %d uid %u gid %u: no matching active SSVM channel",
		    credentials.pid, credentials.uid, credentials.gid);
    }
  else if (um->svm_channel_count && um->dma_map_base)
    {
      vlib_buffer_pool_t *bp = vlib_get_buffer_pool (um->vlib_main, um->dma_buffer_pool_index);
      vlib_physmem_map_t *pm = vlib_physmem_get_map (um->vlib_main, bp->physmem_map_index);

      reply.status = 0;
      reply.generation = um->svm_generation;
      reply.map_size = um->dma_map_size;
      reply.slot_count = um->svm_queue_depth;
      reply.buffer_data_size = um->dma_buffer_data_size;
      reply.buffer_pool_index = um->dma_buffer_pool_index;
      fd = pm->fd;
      um->dma_authorized_clients++;
    }
  else
    uet_log_warn ("DMA client connected before worker channels were ready");

  err = clib_socket_sendmsg (&client, &reply, sizeof (reply), fd >= 0 ? &fd : 0, fd >= 0 ? 1 : 0);
  if (err)
    uet_log_warn ("failed to send DMA mapping metadata: %U", format_clib_error, err);
  else if (fd >= 0)
    uet_log_debug ("exported DMA map generation %llu, size %llu to pid %d uid %u", reply.generation,
		   reply.map_size, credentials.pid, credentials.uid);
  close_err = clib_socket_close (&client);
  clib_socket_free (&client);
  if (close_err)
    {
      if (!err)
	err = close_err;
      else
	clib_error_free (close_err);
    }
  return err;
}

clib_error_t *
uet_dma_listener_init (void)
{
  uet_main_t *um = &uet_main;
  clib_file_t file = { 0 };
  clib_socket_t *listener;
  clib_error_t *err;

  if (um->dma_listener)
    return 0;

  um->dma_socket_name = format (0, "%s/%s%c", vlib_unix_get_runtime_dir (), "uet-dma.sock", 0);
  listener = clib_mem_alloc (sizeof (*listener));
  clib_memset (listener, 0, sizeof (*listener));
  listener->config = (char *) um->dma_socket_name;
  listener->is_server = 1;
  listener->allow_group_write = 1;
  listener->is_seqpacket = 1;
  listener->passcred = 1;

  if ((err = clib_socket_init (listener)))
    {
      clib_socket_free (listener);
      clib_mem_free (listener);
      um->dma_socket_name = 0;
      return err;
    }

  file.read_function = uet_dma_accept_ready;
  file.file_descriptor = listener->fd;
  /* The socket object owns the descriptor and unlinks the Unix socket on
   * close.  Keep clib_file_del_by_index() from closing the descriptor first.
   */
  file.dont_close = 1;
  file.description = format (0, "UET DMA listener %s", listener->config);
  um->dma_listener_file_index = clib_file_add (&file_main, &file);
  um->dma_listener = listener;
  uet_log_notice ("listening for trusted DMA clients on %s", listener->config);
  return 0;
}

void
uet_dma_listener_delete (void)
{
  uet_main_t *um = &uet_main;
  clib_error_t *err;

  if (!um->dma_listener)
    return;

  clib_file_del_by_index (&file_main, um->dma_listener_file_index);
  err = clib_socket_close (um->dma_listener);
  if (err)
    clib_error_free (err);
  clib_socket_free (um->dma_listener);
  clib_mem_free (um->dma_listener);
  um->dma_listener = 0;
  um->dma_listener_file_index = ~0;
  um->dma_socket_name = 0;
  uet_log_debug ("DMA listener deleted");
}
