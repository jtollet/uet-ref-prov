/* SPDX-License-Identifier: Apache-2.0 */

#ifndef included_uet_vpp_client_h
#define included_uet_vpp_client_h

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct uet_vpp_client uet_vpp_client_t;

#define UET_VPP_CLIENT_NAMESPACE_BITS 10
#define UET_VPP_CLIENT_NAMESPACE_MAX  ((1U << UET_VPP_CLIENT_NAMESPACE_BITS) - 1)
#define UET_VPP_CLIENT_PDC_LOCAL_BITS  6
#define UET_VPP_CLIENT_PDC_LOCAL_MASK  ((1U << UET_VPP_CLIENT_PDC_LOCAL_BITS) - 1)
#define UET_VPP_CLIENT_RUDI_LOCAL_BITS (32 - UET_VPP_CLIENT_NAMESPACE_BITS)
#define UET_VPP_CLIENT_RUDI_LOCAL_MASK ((1U << UET_VPP_CLIENT_RUDI_LOCAL_BITS) - 1)

  typedef enum
  {
    UET_VPP_CLIENT_STATUS_OK = 0,
    UET_VPP_CLIENT_STATUS_INVALID_PACKET = -1,
    UET_VPP_CLIENT_STATUS_TX_NOT_CONFIGURED = -2,
    UET_VPP_CLIENT_STATUS_SLOT_BUSY = -3,
    UET_VPP_CLIENT_STATUS_NO_BUFFERS = -4,
  } uet_vpp_client_status_t;

  typedef struct
  {
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t channel_count;
    uint32_t client_namespace;
    uint32_t queue_depth;
    /* Slot and ring depths below are per channel. */
    uint32_t dma_slot_count;
    uint32_t dma_buffer_data_size;
    uint64_t dma_map_size;
    uint32_t tx_ring_size;
    uint32_t rx_ring_size;
    uint64_t generation;
  } uet_vpp_client_info_t;

#define UET_VPP_CLIENT_MAX_RX_IOV 8

  typedef enum
  {
    UET_VPP_CLIENT_RX_F_IP4 = 1U << 0,
    UET_VPP_CLIENT_RX_F_IP6 = 1U << 1,
    UET_VPP_CLIENT_RX_F_UDP = 1U << 2,
  } uet_vpp_client_rx_flags_t;

  typedef struct
  {
    const void *base;
    size_t length;
  } uet_vpp_client_rx_iov_t;

  typedef struct
  {
    uint64_t rx_id;
    uint32_t release_token;
    uint32_t packet_length;
    uint32_t rx_sw_if_index;
    uint16_t flags;
    uint16_t iov_count;
    uet_vpp_client_rx_iov_t iov[UET_VPP_CLIENT_MAX_RX_IOV];
  } uet_vpp_client_rx_t;

  typedef struct
  {
    uint32_t dma_slot;
    uint32_t packet_length;
    uint64_t request_id;
    uint64_t user_context;
  } uet_vpp_client_tx_request_t;

  typedef struct
  {
    uint8_t ip_version;
    uint8_t absolute;
    uint16_t pid_on_fep;
    uint16_t resource_index;
    uint32_t job_id;
    uint8_t ip_address[16];
  } uet_vpp_client_endpoint_t;

  /*
   * A successful TX completion means that VPP owns the submitted buffer and
   * dma_slot now refers to its replacement. It is not a physical device TX
   * completion.
   */
  typedef struct
  {
    int32_t status;
    uint32_t completed_length;
    uint32_t dma_slot;
    uint64_t request_id;
    uint64_t user_context;
  } uet_vpp_client_completion_t;

  /*
   * One client object owns one application segment containing one lockless
   * SPSC channel per VPP worker.  Lifecycle calls must not overlap datapath
   * calls.  A channel has one progress owner and must not be called
   * concurrently; different channel indices may be used concurrently.
   * Endpoint add/delete operations are serialized internally on their
   * low-rate control ring and may run concurrently with datapath operations.
   *
   * Functions return 0 on success or a negative errno value, except poll(),
   * which returns 1 when a completion was consumed, 0 when the CQ is empty, or
   * a negative errno value.
   */
  int uet_vpp_client_open (uet_vpp_client_t **client, const char *segment_name,
			   uet_vpp_client_info_t *info);
  int uet_vpp_client_close (uet_vpp_client_t *client);
  int uet_vpp_client_set_pds_sng (uet_vpp_client_t *client, int enabled);
  int uet_vpp_client_map_dma (uet_vpp_client_t *client, const char *socket_path);
  int uet_vpp_client_endpoint_add (uet_vpp_client_t *client,
				   const uet_vpp_client_endpoint_t *endpoint);
  int uet_vpp_client_endpoint_del (uet_vpp_client_t *client,
				   const uet_vpp_client_endpoint_t *endpoint);

  int uet_vpp_client_acquire_dma (uet_vpp_client_t *client, uint32_t channel_index,
				  uint32_t *dma_slot, void **data, size_t *capacity);
  int uet_vpp_client_release_dma (uet_vpp_client_t *client, uint32_t channel_index,
				  uint32_t dma_slot);
  int uet_vpp_client_submit_ip (uet_vpp_client_t *client, uint32_t channel_index, uint32_t dma_slot,
				uint32_t packet_length, uint64_t request_id, uint64_t user_context);
  int uet_vpp_client_submit_ip_batch (uet_vpp_client_t *client, uint32_t channel_index,
				      const uet_vpp_client_tx_request_t *requests,
				      size_t request_count);
  int uet_vpp_client_poll (uet_vpp_client_t *client, uint32_t channel_index,
			   uet_vpp_client_completion_t *completion);
  int uet_vpp_client_poll_batch (uet_vpp_client_t *client, uint32_t channel_index,
				 uet_vpp_client_completion_t *completions,
				 size_t completion_capacity);
  int uet_vpp_client_poll_rx (uet_vpp_client_t *client, uint32_t channel_index,
			      uet_vpp_client_rx_t *rx);
  int uet_vpp_client_poll_rx_batch (uet_vpp_client_t *client, uint32_t channel_index,
				    uet_vpp_client_rx_t *rx, size_t rx_capacity);

  /* A polled RX must be released exactly once with its unchanged ID and token. */
  int uet_vpp_client_release_rx (uet_vpp_client_t *client, uint32_t channel_index,
				 const uet_vpp_client_rx_t *rx);
  int uet_vpp_client_release_rx_batch (uet_vpp_client_t *client, uint32_t channel_index,
				       const uet_vpp_client_rx_t *rx, size_t rx_count);

#ifdef __cplusplus
}
#endif

#endif /* included_uet_vpp_client_h */
