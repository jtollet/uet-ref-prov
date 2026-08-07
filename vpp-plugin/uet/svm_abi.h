/* SPDX-License-Identifier: Apache-2.0 */

#ifndef included_uet_vpp_svm_abi_h
#define included_uet_vpp_svm_abi_h

#include <stddef.h>
#include <stdint.h>

#define UET_VPP_SVM_ABI_MAGIC 0x53544555U
#define UET_VPP_SVM_ABI_MAJOR 4
#define UET_VPP_SVM_ABI_MINOR 2

#define UET_VPP_SVM_DEFAULT_QUEUE_DEPTH 256
#define UET_VPP_SVM_MIN_QUEUE_DEPTH	8
#define UET_VPP_SVM_MAX_QUEUE_DEPTH	4096
#define UET_VPP_SVM_SHARED_ALIGNMENT	64
#define UET_VPP_SVM_INVALID_DMA_SLOT	UINT32_MAX

#define UET_VPP_SVM_CAP_DMA_SLOTS	     (1U << 0)
#define UET_VPP_SVM_CAP_TX_SPSC		     (1U << 1)
#define UET_VPP_SVM_CAP_RX_SPSC		     (1U << 2)
#define UET_VPP_SVM_CAP_TX_GRAPH_COMPLETION  (1U << 3)
#define UET_VPP_SVM_CAP_EXCLUSIVE_OWNER	     (1U << 4)
#define UET_VPP_SVM_CAP_MULTI_WORKER_SEGMENT (1U << 5)
#define UET_VPP_SVM_CAP_ENDPOINT_DEMUX	     (1U << 6)
#define UET_VPP_SVM_CAP_SNG_DEMUX	     (1U << 7)
#define UET_VPP_SVM_REQUIRED_CAPABILITIES                                                          \
  (UET_VPP_SVM_CAP_DMA_SLOTS | UET_VPP_SVM_CAP_TX_SPSC | UET_VPP_SVM_CAP_RX_SPSC |                 \
   UET_VPP_SVM_CAP_TX_GRAPH_COMPLETION | UET_VPP_SVM_CAP_EXCLUSIVE_OWNER |                         \
   UET_VPP_SVM_CAP_MULTI_WORKER_SEGMENT | UET_VPP_SVM_CAP_ENDPOINT_DEMUX |                         \
   UET_VPP_SVM_CAP_SNG_DEMUX)

#define UET_VPP_SVM_CLIENT_NAMESPACE_BITS 10
#define UET_VPP_SVM_CLIENT_NAMESPACE_MAX  ((1U << UET_VPP_SVM_CLIENT_NAMESPACE_BITS) - 1)
#define UET_VPP_SVM_PDC_LOCAL_BITS	  6
#define UET_VPP_SVM_PDC_LOCAL_MASK	  ((1U << UET_VPP_SVM_PDC_LOCAL_BITS) - 1)
#define UET_VPP_SVM_RUDI_LOCAL_BITS	  (32 - UET_VPP_SVM_CLIENT_NAMESPACE_BITS)
#define UET_VPP_SVM_RUDI_LOCAL_MASK	  ((1U << UET_VPP_SVM_RUDI_LOCAL_BITS) - 1)

#define UET_VPP_SVM_CLIENT_F_DMA_READY	   (1U << 0)
#define UET_VPP_SVM_CLIENT_F_PDS_SNG	   (1U << 1)
#define UET_VPP_SVM_SERVER_F_DMA_READY_ACK (1U << 0)

#define UET_VPP_SVM_RX_F_IP4	(1U << 0)
#define UET_VPP_SVM_RX_F_IP6	(1U << 1)
#define UET_VPP_SVM_RX_F_UDP	(1U << 2)
#define UET_VPP_SVM_MAX_RX_SEGS 8

typedef enum
{
  UET_VPP_SVM_STATUS_OK = 0,
  UET_VPP_SVM_STATUS_INVALID_PACKET = -1,
  UET_VPP_SVM_STATUS_TX_NOT_CONFIGURED = -2,
  UET_VPP_SVM_STATUS_SLOT_BUSY = -3,
  UET_VPP_SVM_STATUS_NO_BUFFERS = -4,
} uet_vpp_svm_status_t;

/*
 * All pointers crossing the process boundary are offsets from the SSVM
 * segment base (the ssvm_shared_header_t address).  New fields may only be
 * appended and require a minor ABI bump.
 */
typedef struct
{
  uint32_t magic;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint16_t header_size;
  uint16_t reserved0;
  uint32_t capabilities;
  uint32_t queue_depth;
  uint32_t worker_count;
  uint64_t segment_size;
  uint64_t generation;
  uint64_t worker_channel_table_offset;
  uint64_t dma_map_size;
  uint32_t dma_buffer_data_size;
  uint16_t dma_buffer_pool_index;
  uint16_t worker_channel_desc_size;
  uint32_t tx_desc_size;
  uint32_t tx_completion_size;
  uint32_t rx_desc_size;
  uint32_t rx_release_size;
  uint32_t client_flags;
  uint32_t server_flags;
  /* PID holding the lifetime lock on this application's SHM object. */
  uint32_t owner_pid;
  uint32_t server_dma_ready_count;
  uint32_t client_namespace;
  uint32_t control_ring_size;
  uint64_t control_request_ring_offset;
  uint64_t control_completion_ring_offset;
  uint64_t reserved2;
} uet_vpp_svm_shared_header_t;

/*
 * One descriptor per VPP worker.  Every offset addresses an independent
 * SPSC object in the common application segment.  The descriptor is padded
 * to two cache lines so future ABI-minor fields can be appended in place.
 */
typedef struct
{
  uint32_t worker_index;
  uint32_t thread_index;
  uint64_t dma_slot_table_offset;
  uint32_t dma_slot_count;
  uint32_t reserved0;
  uint64_t tx_ring_offset;
  uint64_t tx_completion_ring_offset;
  uint32_t tx_ring_size;
  uint32_t reserved1;
  uint64_t rx_ring_offset;
  uint64_t rx_release_ring_offset;
  uint32_t rx_ring_size;
  uint32_t reserved2;
  uint64_t reserved3[7];
} uet_vpp_svm_worker_channel_t;

typedef struct
{
  uint64_t data_offset;
  uint32_t capacity;
  uint32_t reserved;
} uet_vpp_svm_dma_slot_t;

/*
 * Dedicated SPSC rings keep producer and consumer indices on independent
 * cache lines. Indices are monotonically increasing u32 counters; the ring
 * size is at most 4096, so unsigned producer-consumer arithmetic is safe.
 * Descriptor storage immediately follows this header.
 */
typedef struct
{
  uint32_t size;
  uint32_t mask;
  uint8_t reserved0[56];
  uint32_t producer;
  uint8_t producer_pad[60];
  uint32_t consumer;
  uint8_t consumer_pad[60];
} uet_vpp_svm_spsc_ring_t;

typedef struct
{
  uint32_t dma_slot;
  uint32_t packet_length;
  uint64_t request_id;
  uint64_t user_context;
  uint64_t reserved;
} uet_vpp_svm_tx_desc_t;

/*
 * A successful completion transfers ownership of the submitted buffer to
 * the VPP graph and makes dma_slot reusable through a replacement buffer.
 * It does not report physical transmission by a device.
 */
typedef struct
{
  uint32_t dma_slot;
  int32_t status;
  uint32_t completed_length;
  uint32_t reserved;
  uint64_t request_id;
  uint64_t user_context;
} uet_vpp_svm_tx_completion_t;

typedef struct
{
  uint64_t data_offset;
  uint32_t length;
  uint32_t reserved;
} uet_vpp_svm_rx_segment_t;

typedef struct
{
  uint64_t rx_id;
  uint32_t release_token;
  uint32_t packet_length;
  uint32_t rx_sw_if_index;
  uint16_t flags;
  uint16_t segment_count;
  uint64_t reserved;
  uet_vpp_svm_rx_segment_t segments[UET_VPP_SVM_MAX_RX_SEGS];
} uet_vpp_svm_rx_desc_t;

typedef struct
{
  uint64_t rx_id;
  uint32_t release_token;
  uint32_t reserved;
} uet_vpp_svm_rx_release_t;

typedef enum
{
  UET_VPP_SVM_CONTROL_ENDPOINT_ADD = 1,
  UET_VPP_SVM_CONTROL_ENDPOINT_DEL = 2,
} uet_vpp_svm_control_op_t;

typedef struct
{
  uint8_t ip_version;
  uint8_t absolute;
  uint16_t pid_on_fep;
  uint16_t resource_index;
  uint16_t reserved0;
  uint32_t job_id;
  uint8_t ip_address[16];
  uint32_t reserved1;
} uet_vpp_svm_endpoint_key_t;

typedef struct
{
  uint64_t request_id;
  uint16_t operation;
  uint16_t reserved0;
  uint32_t reserved1;
  uet_vpp_svm_endpoint_key_t endpoint;
  uint64_t reserved2[2];
} uet_vpp_svm_control_request_t;

typedef struct
{
  uint64_t request_id;
  int32_t status;
  uint32_t reserved0;
  uint64_t reserved1[2];
} uet_vpp_svm_control_completion_t;

#ifdef __cplusplus
#define UET_VPP_SVM_STATIC_ASSERT static_assert
#else
#define UET_VPP_SVM_STATIC_ASSERT _Static_assert
#endif

UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_shared_header_t) == 128,
			   "unexpected UET SVM shared header size");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, segment_size) == 24,
			   "unexpected UET SVM segment size field offset");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, worker_channel_table_offset) ==
			     40,
			   "unexpected UET SVM worker channel table offset field");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, client_flags) == 80,
			   "unexpected UET SVM client flags offset");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, owner_pid) == 88,
			   "unexpected UET SVM owner PID offset");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, server_dma_ready_count) == 92,
			   "unexpected UET SVM DMA ready count offset");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_worker_channel_t) == 128,
			   "unexpected UET SVM worker channel descriptor size");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_worker_channel_t, tx_ring_offset) == 24,
			   "unexpected UET SVM worker TX ring offset field");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_worker_channel_t, rx_ring_offset) == 48,
			   "unexpected UET SVM worker RX ring offset field");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_dma_slot_t) == 16,
			   "unexpected UET SVM DMA slot size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_spsc_ring_t) == 192,
			   "unexpected UET SVM SPSC ring header size");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_spsc_ring_t, producer) == 64,
			   "unexpected UET SVM SPSC producer offset");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_spsc_ring_t, consumer) == 128,
			   "unexpected UET SVM SPSC consumer offset");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_tx_desc_t) == 32,
			   "unexpected UET SVM TX descriptor size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_tx_completion_t) == 32,
			   "unexpected UET SVM TX completion size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_rx_segment_t) == 16,
			   "unexpected UET SVM RX segment size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_rx_desc_t) == 160,
			   "unexpected UET SVM RX descriptor size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_rx_release_t) == 16,
			   "unexpected UET SVM RX release size");
UET_VPP_SVM_STATIC_ASSERT (offsetof (uet_vpp_svm_shared_header_t, client_namespace) == 96,
			   "unexpected UET SVM client namespace offset");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_endpoint_key_t) == 32,
			   "unexpected UET SVM endpoint key size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_control_request_t) == 64,
			   "unexpected UET SVM control request size");
UET_VPP_SVM_STATIC_ASSERT (sizeof (uet_vpp_svm_control_completion_t) == 32,
			   "unexpected UET SVM control completion size");

#undef UET_VPP_SVM_STATIC_ASSERT

#endif /* included_uet_vpp_svm_abi_h */
