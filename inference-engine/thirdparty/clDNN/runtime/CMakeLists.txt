# Copyright (C) 2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

# =================================== Name / Output settings from parent ===============================

set(CLDNN_BUILD__PROJ             "cldnn_runtime")
set(CLDNN_BUILD__PROJ_LABEL       "${CLDNN_BUILD__PROJ}")

# ========================================= Source/Header files ========================================

set(__CLDNN_Directory__main           "${CMAKE_CURRENT_SOURCE_DIR}")
set(__CLDNN_Label__main                "")
file(GLOB __CLDNN_Sources__main
    "${__CLDNN_Directory__main}/*.h"
    "${__CLDNN_Directory__main}/*.hpp"
    "${__CLDNN_Directory__main}/*.cpp"
  )

file(GLOB __CLDNN_Sources__ocl
    "${__CLDNN_Directory__main}/ocl/*.h"
    "${__CLDNN_Directory__main}/ocl/*.hpp"
    "${__CLDNN_Directory__main}/ocl/*.cpp"
)

set(__CLDNN_AllSources
    ${__CLDNN_Sources__main}
    ${__CLDNN_Sources__ocl}
  )

# =============================================== Filters ==============================================

source_group("${__CLDNN_Label__main}"             FILES ${__CLDNN_Sources__main})

# ===================================== Include/Link directories =======================================

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${__CLDNN_Directory__main}"
    $<TARGET_PROPERTY:inference_engine_plugin_api,INTERFACE_INCLUDE_DIRECTORIES>
  )

# =================================== Link targets and dependencies ====================================

# Main static library.
add_library("${CLDNN_BUILD__PROJ}" STATIC
    ${__CLDNN_AllSources}
  )

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(${CLDNN_BUILD__PROJ} PRIVATE -Os)
endif()

set_property(TARGET "${CLDNN_BUILD__PROJ}" PROPERTY PROJECT_LABEL "${CLDNN_BUILD__PROJ_LABEL}")

if(COMMAND add_cpplint_target)
  add_cpplint_target("${CLDNN_BUILD__PROJ}_cpplint" FOR_TARGETS "${CLDNN_BUILD__PROJ}")
endif()

if(COMMAND set_ie_threading_interface_for)
  set_ie_threading_interface_for("${CLDNN_BUILD__PROJ}")
endif()

target_link_libraries("${CLDNN_BUILD__PROJ}" PRIVATE
    OpenCL
    openvino::itt
  )

if(ENABLE_ONEDNN_FOR_GPU)
  get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if (isMultiConfig)
    add_dependencies(${CLDNN_BUILD__PROJ} onednn_gpu_build)
	foreach(dir IN LISTS ONEDNN_INCLUDE_DIRS)
	  target_include_directories("${CLDNN_BUILD__PROJ}" PUBLIC $<BUILD_INTERFACE:${dir}>)
	endforeach()
	target_compile_definitions("${CLDNN_BUILD__PROJ}" PUBLIC ENABLE_ONEDNN_FOR_GPU)
  endif()
  target_link_libraries("${CLDNN_BUILD__PROJ}" PUBLIC ${ONEDNN_LIBRARY})
endif()

if(WIN32)
  target_link_libraries("${CLDNN_BUILD__PROJ}" PRIVATE setupapi)
elseif((NOT ANDROID) AND (UNIX))
  target_link_libraries("${CLDNN_BUILD__PROJ}" PRIVATE pthread)
endif()

target_link_libraries("${CLDNN_BUILD__PROJ}" PRIVATE inference_engine)

ov_install_static_lib("${CLDNN_BUILD__PROJ}" gpu)

# ======================================================================================================
