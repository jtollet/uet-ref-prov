/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SHARED_ENDPOINT_RDMA_H
#define SHARED_ENDPOINT_RDMA_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { PHYSICAL_RX_BATCH = 16 };

struct physical_rx {
  void *data;
  size_t len;
  unsigned slot;
};

int physical_open (uint32_t peer_ipv4);
ssize_t physical_receive (int queue, void *frame, size_t len);
void physical_open_worker_tx (unsigned worker);
void physical_select_worker_tx (unsigned worker);
unsigned physical_poll_frames (int queue, struct physical_rx frames[PHYSICAL_RX_BATCH]);
void physical_repost_frames (int queue, const struct physical_rx *frames, unsigned count);

#endif
