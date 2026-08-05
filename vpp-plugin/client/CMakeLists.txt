# SPDX-License-Identifier: Apache-2.0

find_library(UET_CLIENT_SVM_LIBRARY NAMES svm)
find_library(UET_CLIENT_VPPINFRA_LIBRARY NAMES vppinfra)

if(NOT UET_CLIENT_SVM_LIBRARY OR NOT UET_CLIENT_VPPINFRA_LIBRARY)
  message(FATAL_ERROR "libsvm and libvppinfra are required for libuet_vpp_client")
endif()

add_library(uet_vpp_client SHARED uet_vpp_client.c)
target_include_directories(uet_vpp_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uet_vpp_client PRIVATE
  ${UET_CLIENT_SVM_LIBRARY}
  ${UET_CLIENT_VPPINFRA_LIBRARY}
  pthread
  rt
  m
  dl
)
set_target_properties(uet_vpp_client PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
)

install(TARGETS uet_vpp_client
  LIBRARY DESTINATION lib
  COMPONENT vpp-plugin-uet
)
install(FILES uet_vpp_client.h
  DESTINATION include
  COMPONENT vpp-plugin-uet-dev
)
