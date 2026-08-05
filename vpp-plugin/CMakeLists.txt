# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(uet-vpp-plugin VERSION 0.10.0 LANGUAGES C)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(VPP REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})

add_subdirectory(uet)
add_subdirectory(client)
add_subdirectory(tools)

option(UET_VPP_ENABLE_PACKAGING "Generate native VPP plugin packages" OFF)

if(UET_VPP_ENABLE_PACKAGING)
  add_vpp_packaging(
    NAME "uet-vpp-plugin"
    VENDOR "Ultra Ethernet Consortium"
    DESCRIPTION "UET host dataplane plugin for VPP"
  )
endif()
