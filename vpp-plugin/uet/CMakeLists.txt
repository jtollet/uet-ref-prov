# SPDX-License-Identifier: Apache-2.0

add_vpp_plugin(uet
  SOURCES
    dma_socket.c
    node.c
    uet.c

  MULTIARCH_SOURCES
    node.c

  API_FILES
    uet.api

  INSTALL_HEADERS
    dma_abi.h
    svm_abi.h

  COMPONENT
    vpp-plugin-uet
)

install(FILES
  dma_abi.h
  svm_abi.h
  DESTINATION include/uet
  COMPONENT vpp-plugin-uet-dev
)
