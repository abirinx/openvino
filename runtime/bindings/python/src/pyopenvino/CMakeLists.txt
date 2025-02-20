# Copyright (C) 2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

project (pyopenvino)

if(NOT DEFINED OpenVINO_SOURCE_DIR)
    find_package(InferenceEngineDeveloperPackage REQUIRED)
endif()

# PYTHON_VERSION_MAJOR and PYTHON_VERSION_MINOR are defined inside pybind11
set(PYTHON_VERSION python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR})
message(STATUS "Python version=${PYTHON_VERSION}")

if(OpenVINO_SOURCE_DIR)
    if(WIN32)
        set(PYTHON_BRIDGE_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<CONFIG>/python_api/${PYTHON_VERSION}/openvino)
    else()
        set(PYTHON_BRIDGE_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/python_api/${PYTHON_VERSION}/openvino)
    endif()

    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PYTHON_BRIDGE_OUTPUT_DIRECTORY})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PYTHON_BRIDGE_OUTPUT_DIRECTORY})
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${PYTHON_BRIDGE_OUTPUT_DIRECTORY})
    set(CMAKE_PDB_OUTPUT_DIRECTORY ${PYTHON_BRIDGE_OUTPUT_DIRECTORY})
endif()

# create target

file(GLOB_RECURSE SOURCES *.cpp)

pybind11_add_module(${PROJECT_NAME} MODULE ${SOURCES})

if(TARGET offline_transformations)
    set(OFFLINE_TRANSFORMATIONS_LIB offline_transformations)
else()
    set(OFFLINE_TRANSFORMATIONS_LIB IE::offline_transformations)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(${PROJECT_NAME} PRIVATE openvino::runtime ${OFFLINE_TRANSFORMATIONS_LIB})

# perform copy
if(OpenVINO_SOURCE_DIR)
    add_custom_command(TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../openvino ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            )
endif()

if(OpenVINO_SOURCE_DIR OR InferenceEngineDeveloperPackage_FOUND)
    if(COMMAND ie_python_minimal_api)
        ie_python_minimal_api(${PROJECT_NAME})
    endif()

    add_clang_format_target(${PROJECT_NAME}_clang FOR_TARGETS ${PROJECT_NAME})

    ie_cpack_add_component(pyopenvino_${PYTHON_VERSION})

    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../openvino
            DESTINATION python/${PYTHON_VERSION}
            COMPONENT pyopenvino_${PYTHON_VERSION}
            USE_SOURCE_PERMISSIONS)

    install(TARGETS ${PROJECT_NAME}
            DESTINATION python/${PYTHON_VERSION}/openvino
            COMPONENT pyopenvino_${PYTHON_VERSION})

    ie_cpack(pyopenvino_${PYTHON_VERSION})
endif()

if(TARGET ie_wheel)
    add_dependencies(ie_wheel ${PROJECT_NAME})
endif()
