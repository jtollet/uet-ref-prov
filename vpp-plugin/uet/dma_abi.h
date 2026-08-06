/* SPDX-License-Identifier: Apache-2.0 */

#ifndef included_uet_vpp_dma_abi_h
#define included_uet_vpp_dma_abi_h

#include <stdint.h>

#define UET_VPP_DMA_ABI_MAGIC	0x414d4455U
#define UET_VPP_DMA_ABI_VERSION 2

typedef struct
{
  uint32_t magic;
  uint16_t version;
  int16_t status;
  uint64_t generation;
  uint64_t map_size;
  uint32_t slot_count;
  uint32_t buffer_data_size;
  uint16_t buffer_pool_index;
  uint16_t reserved0;
  uint32_t reserved1;
} uet_vpp_dma_reply_t;

#ifdef __cplusplus
#define UET_VPP_DMA_STATIC_ASSERT static_assert
#else
#define UET_VPP_DMA_STATIC_ASSERT _Static_assert
#endif

UET_VPP_DMA_STATIC_ASSERT (sizeof (uet_vpp_dma_reply_t) == 40, "unexpected UET DMA reply size");

#undef UET_VPP_DMA_STATIC_ASSERT

#endif /* included_uet_vpp_dma_abi_h */
