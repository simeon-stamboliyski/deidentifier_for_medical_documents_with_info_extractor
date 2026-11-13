cmake_minimum_required(VERSION 3.20)

project(epicrisis_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


add_executable(epicrisis_pipeline
    main.cpp
    deidentifier/deidentifier.cpp
    detectors/detectors.cpp
    file_processor/file_processor.cpp
    metadata/metadata.cpp
    extractor/extractor.cpp
)

target_include_directories(epicrisis_pipeline PRIVATE
    deidentifier
    detectors
    file_processor
    metadata
    extractor
)

if (MSVC)
    target_compile_options(epicrisis_pipeline PRIVATE
        /W4
        /utf-8           
        /permissive-     
        /Zc:__cplusplus 
    )
else()
    target_compile_options(epicrisis_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WIN32)
    target_compile_definitions(epicrisis_pipeline PRIVATE
        NOMINMAX             
        WIN32_LEAN_AND_MEAN 
    )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(epicrisis_pipeline PRIVATE stdc++fs)
endif()

find_package(Threads REQUIRED)
target_link_libraries(epicrisis_pipeline PRIVATE Threads::Threads)
