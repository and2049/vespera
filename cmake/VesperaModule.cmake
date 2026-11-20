if(APPLE)
  enable_language(OBJCXX)
endif()

set(VESPERA_SDK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" CACHE PATH
    "Directory containing the Vespera module SDK")
set(VESPERA_MODULE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/modules" CACHE PATH
    "Directory for independently loadable .vespera packages")

function(_vespera_module_library target output package audio_unit sources macos_sources libraries)
  add_library(${target} MODULE ${sources})
  target_include_directories(${target} PRIVATE "${VESPERA_SDK_INCLUDE_DIR}")
  target_compile_features(${target} PRIVATE cxx_std_20)
  if(APPLE AND audio_unit)
    target_link_libraries(${target} PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
  endif()
  if(libraries)
    target_link_libraries(${target} PRIVATE ${libraries})
  endif()
  set_target_properties(${target} PROPERTIES
    CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    PREFIX "" OUTPUT_NAME "${output}"
    LIBRARY_OUTPUT_DIRECTORY "${package}" RUNTIME_OUTPUT_DIRECTORY "${package}")
  foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${config} "${package}"
      RUNTIME_OUTPUT_DIRECTORY_${config} "${package}")
  endforeach()
  if(APPLE AND macos_sources)
    target_sources(${target} PRIVATE ${macos_sources})
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(${target} PRIVATE "-framework AppKit")
  endif()
  if(COMMAND vespera_warnings)
    vespera_warnings(${target})
  endif()
endfunction()

function(vespera_add_module name)
  cmake_parse_arguments(MODULE "AUDIO_UNIT" "" "SOURCES;MACOS_SOURCES;LIBRARIES" ${ARGN})
  # Packages are laid out as the host discovers them: <output>/<category folder>/<name>.vespera.
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/module.json.in" manifest)
  string(JSON category ERROR_VARIABLE error GET "${manifest}" category)
  if(category STREQUAL "amp")
    set(folder amps)
  elseif(category STREQUAL "cab")
    set(folder cabs)
  elseif(category MATCHES "^(pre-fx|post-fx|special-fx)$")
    set(folder ${category})
  else()
    message(FATAL_ERROR "vespera_add_module(${name}): module.json.in declares no known category")
  endif()
  set(package "${VESPERA_MODULE_OUTPUT_DIR}/${folder}/${name}.vespera")
  _vespera_module_library(vespera_${name} ${name} "${package}" "${MODULE_AUDIO_UNIT}"
    "${MODULE_SOURCES}" "${MODULE_MACOS_SOURCES}" "${MODULE_LIBRARIES}")
  configure_file(module.json.in "${package}/module.json" @ONLY)
  file(GLOB_RECURSE resources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
  foreach(resource IN LISTS resources)
    configure_file("resources/${resource}" "${package}/resources/${resource}" COPYONLY)
  endforeach()
  set_target_properties(vespera_${name} PROPERTIES VESPERA_PACKAGE_DIRECTORY "${package}")
  install(DIRECTORY "${package}" DESTINATION "modules/${folder}")
endfunction()

# Adds a package-internal companion binary (for example an amp's EQ) to the package created by
# vespera_add_module(<package>). Companions use the ordinary module ABI but are declared by the
# owning manifest, never discovered or selected on their own.
function(vespera_add_companion package name)
  cmake_parse_arguments(COMPANION "AUDIO_UNIT" "" "SOURCES;MACOS_SOURCES;LIBRARIES" ${ARGN})
  if(NOT TARGET vespera_${package})
    message(FATAL_ERROR "vespera_add_companion: add package ${package} first")
  endif()
  get_target_property(directory vespera_${package} VESPERA_PACKAGE_DIRECTORY)
  _vespera_module_library(vespera_${name} ${name} "${directory}"
    "${COMPANION_AUDIO_UNIT}" "${COMPANION_SOURCES}" "${COMPANION_MACOS_SOURCES}"
    "${COMPANION_LIBRARIES}")
  add_dependencies(vespera_${package} vespera_${name})
endfunction()
