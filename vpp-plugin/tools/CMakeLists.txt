# SPDX-License-Identifier: Apache-2.0

add_executable(uet_vpp_tx_smoke tx_smoke.c)
target_link_libraries(uet_vpp_tx_smoke PRIVATE uet_vpp_client)

add_executable(uet_vpp_spsc_negative spsc_negative.c)
target_link_libraries(uet_vpp_spsc_negative PRIVATE uet_vpp_client)

add_executable(uet_vpp_owner_probe owner_probe.c)
target_link_libraries(uet_vpp_owner_probe PRIVATE uet_vpp_client)

add_executable(uet_dma_auth_probe dma_auth_probe.c)
