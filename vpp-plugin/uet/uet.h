/* SPDX-License-Identifier: Apache-2.0 */

#ifndef included_uet_vpp_plugin_h
#define included_uet_vpp_plugin_h

#include <svm/ssvm.h>
#include <vlib/vlib.h>
#include <vnet/fib/fib_source.h>
#include <vnet/ip/ip.h>
#include <vnet/vnet.h>
#include <vppinfra/socket.h>
#include <vppinfra/bihash_40_8.h>

#include <uet/dma_abi.h>
#include <uet/svm_abi.h>

#define UET_PLUGIN_BUILD_VERSION "0.10.0"
#define UET_INVALID_THREAD_INDEX ((u32) ~0)
#define UET_IP_PROTOCOL		 253
#define UET_UDP_PORT		 49150
#define UET_INVALID_CLIENT_INDEX ((u32) ~0)

typedef enum
{
  UET_RX_PATH_IP4_NATIVE,
  UET_RX_PATH_IP6_NATIVE,
  UET_RX_PATH_IP4_UDP,
  UET_RX_PATH_IP6_UDP,
  UET_RX_N_PATHS,
} uet_rx_path_t;

typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  u8 active;
  u8 dma_ready_ack;
  u32 client_index;
  u32 client_namespace;
  uet_vpp_svm_shared_header_t *svm_header;

  u32 *dma_buffer_indices;
  uet_vpp_svm_dma_slot_t *dma_slots;
  u32 dma_slot_count;

  u64 next_rx_id;
  u32 *rx_buffer_indices;
  u64 *rx_ids;
  u16 *rx_free_slots;
  u32 rx_free_count;
  u32 rx_outstanding;

  uet_vpp_svm_spsc_ring_t *tx_ring;
  uet_vpp_svm_tx_desc_t *tx_descs;
  uet_vpp_svm_spsc_ring_t *tx_completion_ring;
  uet_vpp_svm_tx_completion_t *tx_completion_entries;
  uet_vpp_svm_spsc_ring_t *rx_ring;
  uet_vpp_svm_rx_desc_t *rx_descs;
  uet_vpp_svm_spsc_ring_t *rx_release_ring;
  uet_vpp_svm_rx_release_t *rx_release_entries;
  uet_vpp_svm_spsc_ring_t *control_request_ring;
  uet_vpp_svm_control_request_t *control_requests;
  uet_vpp_svm_spsc_ring_t *control_completion_ring;
  uet_vpp_svm_control_completion_t *control_completions;
} uet_worker_channel_t;

/*
 * Each worker owns one independent channel for every application segment.
 * The main thread may change this vector only while holding the worker
 * barrier. No datapath lock is allowed in this structure.
 */
typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  u32 thread_index;
  u64 poll_calls;
  u64 tx_requests;
  u64 invalid_requests;
  u64 tx_completion_ring_full;
  u64 tx_packets;
  u64 tx_bytes;
  u64 tx_completions;
  u64 rx_ip4_packets;
  u64 rx_ip4_bytes;
  u64 rx_ip6_packets;
  u64 rx_ip6_bytes;
  u64 rx_udp4_packets;
  u64 rx_udp4_bytes;
  u64 rx_udp6_packets;
  u64 rx_udp6_bytes;
  u64 rx_delivered;
  u64 rx_ambiguous;
  u64 rx_ring_full;
  u64 rx_bad_chain;
  u64 rx_releases;
  u64 rx_invalid_releases;
  u64 rx_outstanding;
  uet_worker_channel_t *channels;

  /* FIB indices copied from main-thread control state under the barrier. */
  u32 tx_ip4_fib_index;
  u32 tx_ip6_fib_index;
  u32 tx_ip4_table_id;
  u32 tx_ip6_table_id;
  u8 tx_configured;

} uet_worker_t;

/* One application process owns one segment with one channel per VPP worker. */
typedef struct
{
  u32 index;
  ssvm_private_t segment;
  uet_vpp_svm_shared_header_t *header;
  uet_vpp_svm_worker_channel_t *shared_channels;
  uet_vpp_svm_spsc_ring_t *control_request_ring;
  uet_vpp_svm_control_request_t *control_requests;
  uet_vpp_svm_spsc_ring_t *control_completion_ring;
  uet_vpp_svm_control_completion_t *control_completions;
  u32 channel_count;
  u32 queue_depth;
  u64 generation;
  u32 client_namespace;
} uet_client_t;

/* Main-thread-owned control state. */
typedef struct
{
  u16 msg_id_base;
  u8 enabled;
  u8 protocols_registered;

  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;

  /* Indexed by VPP thread index; index zero is the main thread. */
  uet_worker_t *workers;

  /* Optional generic RX placement by native EV or UDP source port. */
  u32 rx_handoff_queue_indices[UET_RX_N_PATHS];
  u8 rx_entropy_handoff;

  /* Main-thread-owned pool; pool indices also index worker channel vectors. */
  uet_client_t *clients;
  u64 svm_generation;
  u32 next_client_namespace;
  u32 client_by_namespace[UET_VPP_SVM_CLIENT_NAMESPACE_MAX + 1];
  clib_bihash_40_8_t endpoint_hash;
  u64 endpoint_registrations;
  u64 endpoint_collisions;

  u8 dma_buffer_pool_index;
  u32 dma_buffer_data_size;
  u64 dma_map_size;
  u8 *dma_map_base;
  u64 dma_authorized_clients;
  u64 dma_rejected_clients;

  /* Main-thread-owned TX routing configuration and FIB locks. */
  u32 tx_ip4_fib_index;
  u32 tx_ip6_fib_index;
  u32 tx_ip4_table_id;
  u32 tx_ip6_table_id;
  u8 tx_configured;
  fib_source_t fib_source;

  clib_socket_t *dma_listener;
  u32 dma_listener_file_index;
  u8 *dma_socket_name;
} uet_main_t;

extern uet_main_t uet_main;
extern vlib_log_class_registration_t uet_log;
extern vlib_node_registration_t uet_input_node;
extern vlib_node_registration_t uet4_ip_input_node;
extern vlib_node_registration_t uet6_ip_input_node;
extern vlib_node_registration_t uet4_udp_input_node;
extern vlib_node_registration_t uet6_udp_input_node;
extern vlib_node_registration_t uet4_ip_handoff_node;
extern vlib_node_registration_t uet6_ip_handoff_node;
extern vlib_node_registration_t uet4_udp_handoff_node;
extern vlib_node_registration_t uet6_udp_handoff_node;

int uet_enable_disable (u8 enable);
int uet_svm_create (const char *segment_name, u32 queue_depth);
int uet_svm_delete (const char *segment_name);
void uet_counters_clear (void);
clib_error_t *uet_dma_listener_init (void);
void uet_dma_listener_delete (void);
int uet_tx_set_fib_tables (u32 ip4_table_id, u32 ip6_table_id);

#define uet_log_debug(...)  vlib_log_debug (uet_log.class, __VA_ARGS__)
#define uet_log_info(...)   vlib_log_info (uet_log.class, __VA_ARGS__)
#define uet_log_notice(...) vlib_log_notice (uet_log.class, __VA_ARGS__)
#define uet_log_warn(...)   vlib_log_warn (uet_log.class, __VA_ARGS__)
#define uet_log_err(...)    vlib_log_err (uet_log.class, __VA_ARGS__)

#endif /* included_uet_vpp_plugin_h */
