# Install rules for the module SDK: `cmake --install <build> --prefix <prefix>` produces a tree
# that out-of-tree modules use through find_package(Vespera CONFIG). Host internals (runtime,
# audio engine, presets, the app) are deliberately not installed.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(VESPERA_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Vespera")

add_library(vespera_sdk INTERFACE)
add_library(vespera::sdk ALIAS vespera_sdk)
set_target_properties(vespera_sdk PROPERTIES EXPORT_NAME sdk)
target_compile_features(vespera_sdk INTERFACE cxx_std_20)
target_include_directories(vespera_sdk INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(FILES
    include/vespera/binary32.hpp
    include/vespera/module.h
    include/vespera/module_ui.h
    include/vespera/ui_tokens.h
    include/vespera/module_controls_macos.h
    include/vespera/parameter_display.hpp
    include/vespera/files.hpp
    include/vespera/file_content.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vespera)
install(DIRECTORY include/vespera/kit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vespera)
install(FILES
    include/vespera/sdk/audio_unit.hpp
    include/vespera/sdk/main_thread.hpp
    include/vespera/sdk/plugin_lifecycle.hpp
    include/vespera/sdk/layout_view_macos.h
    include/vespera/sdk/artwork_view_macos.h
    include/vespera/sdk/shared_policy.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vespera/sdk)
if(NOT VESPERA_BUILD_DSP)
  # The kit includes the DSP toolkit; dsp/ installs it when it is part of the build.
  install(DIRECTORY include/vespera/dsp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vespera)
endif()

install(TARGETS vespera_sdk vespera-module-check EXPORT VesperaTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT VesperaTargets NAMESPACE vespera:: DESTINATION ${VESPERA_INSTALL_CMAKEDIR})
configure_package_config_file(cmake/VesperaConfig.cmake.in
  ${PROJECT_BINARY_DIR}/VesperaConfig.cmake
  INSTALL_DESTINATION ${VESPERA_INSTALL_CMAKEDIR}
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
# Before 1.0 a minor release may change the SDK incompatibly, so only patch releases match.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/VesperaConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    ${PROJECT_BINARY_DIR}/VesperaConfig.cmake
    ${PROJECT_BINARY_DIR}/VesperaConfigVersion.cmake
    cmake/VesperaModule.cmake
  DESTINATION ${VESPERA_INSTALL_CMAKEDIR})
