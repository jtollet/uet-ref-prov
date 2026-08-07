# SPDX-License-Identifier: Apache-2.0

add_executable(uet_vpp_tx_smoke tx_smoke.c)
target_link_libraries(uet_vpp_tx_smoke PRIVATE uet_vpp_client pthread)

add_executable(uet_vpp_spsc_negative spsc_negative.c)
target_link_libraries(uet_vpp_spsc_negative PRIVATE uet_vpp_client)

add_executable(uet_vpp_owner_probe owner_probe.c)
find_library(UET_TOOLS_VPPINFRA_LIBRARY NAMES vppinfra REQUIRED)
target_link_libraries(uet_vpp_owner_probe PRIVATE
  uet_vpp_client
  ${UET_TOOLS_VPPINFRA_LIBRARY}
)

add_executable(uet_vpp_endpoint_smoke endpoint_smoke.c)
target_link_libraries(uet_vpp_endpoint_smoke PRIVATE uet_vpp_client)

add_executable(uet_dma_auth_probe dma_auth_probe.c)
