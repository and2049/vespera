cmake_minimum_required(VERSION 3.25)
project(VesperaDSP VERSION 0.1.1 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(VESPERA_DSP_BENCH "Build the offline DSP bench" ON)

function(vespera_dsp_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

add_library(vespera_dsp INTERFACE)
add_library(vespera::dsp ALIAS vespera_dsp)
set_target_properties(vespera_dsp PROPERTIES EXPORT_NAME dsp)
target_compile_features(vespera_dsp INTERFACE cxx_std_20)
target_include_directories(vespera_dsp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS vespera_dsp EXPORT VesperaDSPTargets)
install(DIRECTORY ../include/vespera/dsp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vespera)
install(EXPORT VesperaDSPTargets NAMESPACE vespera:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VesperaDSP)
configure_package_config_file(VesperaDSPConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/VesperaDSPConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VesperaDSP)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/VesperaDSPConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/VesperaDSPConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/VesperaDSPConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VesperaDSP)

if(BUILD_TESTING)
  foreach(suite smoothing filters svf crossover oversampling time history allpass bench realtime
      utility dynamics shaping cascade equalizer fft convolution response ownership allocation)
    add_executable(vespera_dsp_${suite}_test tests/${suite}_test.cpp)
    target_link_libraries(vespera_dsp_${suite}_test PRIVATE vespera::dsp)
    vespera_dsp_warnings(vespera_dsp_${suite}_test)
    add_test(NAME dsp_${suite} COMMAND vespera_dsp_${suite}_test)
  endforeach()
endif()

if(VESPERA_DSP_BENCH)
  add_executable(vespera_dsp_bench bench/main.cpp)
  target_link_libraries(vespera_dsp_bench PRIVATE vespera::dsp)
  vespera_dsp_warnings(vespera_dsp_bench)
  target_compile_definitions(vespera_dsp_bench PRIVATE
    VESPERA_DSP_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
endif()
