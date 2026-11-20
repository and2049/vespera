cmake_minimum_required(VERSION 3.25)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum supported macOS version")
endif()

project(Vespera VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(FetchContent)

option(VESPERA_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(VESPERA_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(VESPERA_FUZZING "Build libFuzzer targets (requires Clang)" OFF)
option(VESPERA_UPSTREAM_TESTS "Build audio-io-core tests alongside runtime tests" OFF)
option(VESPERA_BUILD_DSP "Build the optional standalone DSP toolkit" ON)

if(VESPERA_THREAD_SANITIZER AND (VESPERA_SANITIZERS OR VESPERA_FUZZING))
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with address sanitizers or fuzzing")
endif()

if(VESPERA_THREAD_SANITIZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "VESPERA_THREAD_SANITIZER requires Clang or GCC")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

if(VESPERA_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "VESPERA_FUZZING requires Clang with libFuzzer")
  endif()
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("#include <cstddef>
    extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, std::size_t) { return 0; }"
    VESPERA_HAS_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT VESPERA_HAS_LIBFUZZER)
    message(FATAL_ERROR "Compiler has no libFuzzer runtime; select LLVM Clang (e.g. Homebrew llvm on macOS)")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=fuzzer-no-link,address,undefined)
endif()

if(VESPERA_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "VESPERA_SANITIZERS requires Clang or GCC")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(AUDIO_ENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AUDIO_ENGINE_BUILD_TESTS ${VESPERA_UPSTREAM_TESTS} CACHE BOOL "" FORCE)
FetchContent_Declare(audio_io_core
  GIT_REPOSITORY https://github.com/and2049/audio-io-core.git
  GIT_TAG 99da7c5f20f0fc1af5ffd4e5c8553dfebfeee5b8
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(audio_io_core)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  URL https://codeload.github.com/nlohmann/json/tar.gz/55f93686c01528224f448c19128836e7df245f72
  URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

function(vespera_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

find_package(Threads REQUIRED)
add_library(vespera_runtime STATIC src/runtime.cpp src/module_loader.cpp src/graph_branch.cpp
  src/parameter_queue.cpp src/instance_cache.cpp
  src/host_services.cpp src/graph_loader.cpp)
target_include_directories(vespera_runtime PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vespera_runtime PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
vespera_warnings(vespera_runtime)

add_library(vespera_presets STATIC src/preset.cpp)
target_link_libraries(vespera_presets PUBLIC vespera_runtime PRIVATE nlohmann_json::nlohmann_json)
vespera_warnings(vespera_presets)

add_library(vespera_core_io STATIC src/core_io.cpp)
target_include_directories(vespera_core_io PUBLIC include)
vespera_warnings(vespera_core_io)

add_library(vespera_audio STATIC src/audio_session.cpp)
target_link_libraries(vespera_audio PUBLIC vespera_runtime vespera_core_io audio_engine::audio_engine)
vespera_warnings(vespera_audio)

add_library(vespera_app_audio SHARED src/app_audio.cpp)
target_link_libraries(vespera_app_audio PRIVATE vespera_audio nlohmann_json::nlohmann_json)
if(APPLE)
  target_link_libraries(vespera_app_audio PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()
set_target_properties(vespera_app_audio PROPERTIES CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
vespera_warnings(vespera_app_audio)

add_executable(vespera apps/headless/main.cpp)
target_link_libraries(vespera PRIVATE vespera_audio vespera_presets)
vespera_warnings(vespera)

add_subdirectory(apps/module-check)

if(VESPERA_BUILD_DSP)
  add_subdirectory(dsp)
endif()

include(cmake/VesperaSDK.cmake)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(VESPERA_FUZZING)
  add_subdirectory(tests/fuzz)
endif()
