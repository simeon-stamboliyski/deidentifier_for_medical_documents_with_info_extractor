cmake_minimum_required(VERSION 3.20)

project(epicrisis_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    target_compile_options(epicrisis_pipeline PRIVATE /W4)
else()
    target_compile_options(epicrisis_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()