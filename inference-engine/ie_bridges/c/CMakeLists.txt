# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

project(InferenceEngine_C_API)

add_subdirectory(src)

if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()
