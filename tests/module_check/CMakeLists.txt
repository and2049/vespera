set(packages "${CMAKE_CURRENT_BINARY_DIR}/packages")

function(module_check_fixture name)
  add_library(module_check_fixture_${name} MODULE fixture_module.cpp)
  target_include_directories(module_check_fixture_${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(module_check_fixture_${name} PRIVATE cxx_std_20)
  target_compile_definitions(module_check_fixture_${name} PRIVATE
    "FIXTURE_ID=\"test.module-check.${name}\"" ${ARGN})
  set_target_properties(module_check_fixture_${name} PROPERTIES PREFIX "" CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  vespera_warnings(module_check_fixture_${name})
endfunction()

module_check_fixture(drive)
module_check_fixture(pass_through FIXTURE_PASS_THROUGH)
module_check_fixture(allocates FIXTURE_ALLOCATES)
module_check_fixture(block_dependent FIXTURE_BLOCK_DEPENDENT)
module_check_fixture(inexact_bypass FIXTURE_INEXACT_BYPASS)
module_check_fixture(bad_state FIXTURE_ACCEPTS_BAD_STATE)
module_check_fixture(emits_nan FIXTURE_EMITS_NAN)
module_check_fixture(bad_capabilities FIXTURE_BAD_CAPABILITIES)
module_check_fixture(metadata_skew FIXTURE_METADATA_SKEW)
module_check_fixture(metadata_undeclared FIXTURE_METADATA_UNDECLARED)
module_check_fixture(metadata_bypass FIXTURE_METADATA_BYPASS)
module_check_fixture(metadata_labels FIXTURE_METADATA_LABELS)
module_check_fixture(blocks_on_work FIXTURE_BLOCKS_ON_WORK)
module_check_fixture(ignores_cancel FIXTURE_IGNORES_CANCEL)
module_check_fixture(frees_result FIXTURE_FREES_RESULT)
module_check_fixture(own_thread FIXTURE_OWN_THREAD)
module_check_fixture(logs_in_process FIXTURE_LOGS_IN_PROCESS)
module_check_fixture(status_waits FIXTURE_STATUS_WAITS)
module_check_fixture(foreign_request FIXTURE_FOREIGN_REQUEST)
module_check_fixture(bad_status FIXTURE_BAD_STATUS)

# The routed Special FX fixture, conforming or with one defect.
function(module_check_routed name)
  add_library(module_check_routed_${name} MODULE routed_module.cpp)
  target_include_directories(module_check_routed_${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(module_check_routed_${name} PRIVATE cxx_std_20)
  target_compile_definitions(module_check_routed_${name} PRIVATE ${ARGN})
  set_target_properties(module_check_routed_${name} PROPERTIES PREFIX "" CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  vespera_warnings(module_check_routed_${name})
endfunction()

module_check_routed(selected)
module_check_routed(static FIXTURE_STATIC_BRANCH)
module_check_routed(follows_mix FIXTURE_FOLLOWS_MIX)
module_check_routed(leaks_dry FIXTURE_LEAKS_DRY)
module_check_routed(stepped_mix FIXTURE_STEPPED_MIX)
module_check_routed(unsnapped_mix FIXTURE_UNSNAPPED_MIX)

# Lays out <name>.vespera as discovery expects: module.json, binaries, icon and resources. With
# EQ the package is an amp whose companion is that binary.
function(module_check_package name)
  cmake_parse_arguments(PACKAGE "" "BINARY;ID;LAYOUT;EQ;EQ_ID;EQ_LAYOUT;CATEGORY;SCHEMA;EXTRA" ""
    ${ARGN})
  set(directory "${packages}/${name}.vespera")
  set(outputs "${directory}/module.so")
  add_custom_command(OUTPUT "${directory}/module.so"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PACKAGE_BINARY}> "${directory}/module.so"
    DEPENDS ${PACKAGE_BINARY} VERBATIM)
  set(ui "")
  if(PACKAGE_LAYOUT)
    set(ui ", \"ui\": {\"layout\": \"resources/${PACKAGE_LAYOUT}\"}")
  endif()
  set(category pre-fx)
  if(PACKAGE_CATEGORY)
    set(category ${PACKAGE_CATEGORY})
  endif()
  set(schema 1)
  if(PACKAGE_SCHEMA)
    set(schema ${PACKAGE_SCHEMA})
  endif()
  set(extra "")
  if(PACKAGE_EXTRA)
    set(extra ", ${PACKAGE_EXTRA}")
  endif()
  set(eq "")
  if(PACKAGE_EQ)
    set(category amp)
    list(APPEND outputs "${directory}/eq.so")
    add_custom_command(OUTPUT "${directory}/eq.so"
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PACKAGE_EQ}> "${directory}/eq.so"
      DEPENDS ${PACKAGE_EQ} VERBATIM)
    set(eq ", \"eq\": {\"id\": \"${PACKAGE_EQ_ID}\", \"binary\": \"eq.so\"")
    if(PACKAGE_EQ_LAYOUT)
      string(APPEND eq ", \"ui\": {\"layout\": \"resources/${PACKAGE_EQ_LAYOUT}\"}")
    endif()
    string(APPEND eq "}")
  endif()
  file(WRITE "${directory}/module.json"
    "{\"schemaVersion\": ${schema}, \"id\": \"${PACKAGE_ID}\", \"category\": \"${category}\", "
    "\"binary\": \"module.so\", \"icon\": \"icon.svg\", \"resources\": \"resources\"${ui}${eq}${extra}}\n")
  configure_file(icon.svg "${directory}/icon.svg" COPYONLY)
  file(GLOB resources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/resources" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
  foreach(resource IN LISTS resources)
    configure_file("resources/${resource}" "${directory}/resources/${resource}" COPYONLY)
  endforeach()
  add_custom_target(module_check_package_${name} ALL DEPENDS ${outputs})
endfunction()

# Runs the tool on a package. FAIL lists the target:check results that must fail and no others;
# PASS lists results that must pass rather than be skipped, WARN results that must warn.
function(module_check_test name)
  cmake_parse_arguments(TEST "" "" "FAIL;PASS;WARN" ${ARGN})
  string(REPLACE ";" "," fail "${TEST_FAIL}")
  string(REPLACE ";" "," pass "${TEST_PASS}")
  string(REPLACE ";" "," warn "${TEST_WARN}")
  # CPU policy is tested by module_check_cli; wall-clock load is not a fixture correctness defect.
  set(cpu_limit 10000)
  add_test(NAME module_check_${name} COMMAND ${CMAKE_COMMAND}
    "-DTOOL=$<TARGET_FILE:vespera-module-check>" "-DPACKAGE=${packages}/${name}.vespera"
    "-DFAIL=${fail}" "-DPASS=${pass}" "-DWARN=${warn}" "-DCPU_LIMIT=${cpu_limit}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/expect.cmake")
endfunction()

# The layout check runs on macOS only, and the realtime check needs macOS libmalloc without a
# sanitizer replacing the allocator; elsewhere both are skipped.
set(module_layout "")
set(eq_layout "")
set(module_realtime "")
set(eq_realtime "")
if(APPLE)
  set(module_layout module:layout)
  set(eq_layout eq:layout)
  if(NOT VESPERA_SANITIZERS AND NOT VESPERA_THREAD_SANITIZER AND NOT VESPERA_FUZZING)
    set(module_realtime module:realtime)
    set(eq_realtime eq:realtime)
  endif()
endif()

module_check_package(gain BINARY vespera_fixture_gain ID dev.vespera.gain LAYOUT gain.json)
module_check_package(file_work BINARY vespera_fixture_file ID test.file.convolution LAYOUT file.json)
module_check_test(file_work PASS module:files module:abi module:state)
module_check_package(rejects_missing BINARY vespera_fixture_rejects_missing ID test.file.convolution)
module_check_test(rejects_missing FAIL module:files)
module_check_package(file_undeclared BINARY vespera_fixture_file ID test.file.convolution LAYOUT file_undeclared.json)
if(APPLE)
  module_check_test(file_undeclared FAIL module:layout)
endif()
module_check_test(gain PASS module:abi module:lifecycle module:blocks module:automation
  module:state module:robustness module:latency module:services module:threads ${module_realtime}
  ${module_layout})

module_check_package(drive BINARY module_check_fixture_drive ID test.module-check.drive
  LAYOUT drive.json EQ vespera_fixture_gain EQ_ID dev.vespera.gain EQ_LAYOUT gain.json)
module_check_test(drive PASS package:manifest module:abi module:lifecycle module:blocks
  module:automation module:bypass module:state module:robustness module:latency module:services
  module:pending module:cancel module:offline module:threads ${module_realtime}
  eq:abi eq:state ${eq_realtime} ${module_layout} ${eq_layout})

module_check_package(pass_through BINARY module_check_fixture_pass_through
  ID test.module-check.pass_through)
module_check_test(pass_through PASS module:bypass module:robustness module:latency)

module_check_package(allocates BINARY module_check_fixture_allocates
  ID test.module-check.allocates)
if(module_realtime)
  module_check_test(allocates FAIL module:pending ${module_realtime})
else()
  module_check_test(allocates)
endif()

module_check_package(block_dependent BINARY module_check_fixture_block_dependent
  ID test.module-check.block_dependent)
module_check_test(block_dependent FAIL module:blocks module:automation module:offline)

module_check_package(inexact_bypass BINARY module_check_fixture_inexact_bypass
  ID test.module-check.inexact_bypass)
module_check_test(inexact_bypass FAIL module:bypass module:latency)

module_check_package(bad_state BINARY module_check_fixture_bad_state ID test.module-check.bad_state)
module_check_test(bad_state FAIL module:state)

module_check_package(emits_nan BINARY module_check_fixture_emits_nan ID test.module-check.emits_nan)
module_check_test(emits_nan FAIL module:robustness)

module_check_package(bad_capabilities BINARY module_check_fixture_bad_capabilities
  ID test.module-check.bad_capabilities)
module_check_test(bad_capabilities FAIL module:abi module:bypass)

module_check_package(undeclared_parameter BINARY module_check_fixture_drive
  ID test.module-check.drive LAYOUT undeclared.json)
module_check_test(undeclared_parameter FAIL ${module_layout})

module_check_package(rescaled_readout BINARY module_check_fixture_drive
  ID test.module-check.drive LAYOUT rescaled.json)
module_check_test(rescaled_readout FAIL ${module_layout})

foreach(defect skew undeclared bypass labels)
  module_check_package(metadata_${defect} BINARY module_check_fixture_metadata_${defect}
    ID test.module-check.metadata_${defect})
  module_check_test(metadata_${defect} FAIL module:abi)
endforeach()

# Layouts whose explicit formats disagree with the drive fixture's parameter metadata.
foreach(layout disagreeing_range disagreeing_off numbered_choice)
  module_check_package(${layout} BINARY module_check_fixture_drive
    ID test.module-check.drive LAYOUT ${layout}.json)
  module_check_test(${layout} FAIL ${module_layout})
endforeach()
module_check_package(abbreviated_readout BINARY module_check_fixture_drive
  ID test.module-check.drive LAYOUT abbreviated_readout.json)
module_check_test(abbreviated_readout WARN ${module_layout})

# Defects in the use of host services, each failing exactly the listed checks.
foreach(defect blocks_on_work ignores_cancel frees_result own_thread logs_in_process
    status_waits foreign_request bad_status)
  module_check_package(${defect} BINARY module_check_fixture_${defect}
    ID test.module-check.${defect})
endforeach()
module_check_test(blocks_on_work FAIL module:pending)
module_check_test(ignores_cancel FAIL module:cancel)
module_check_test(frees_result FAIL module:cancel ${module_realtime})
# Its own thread makes adoption depend on block boundaries, which the automation check sees too,
# and starting the thread allocates inside process.
if(module_realtime)
  module_check_test(own_thread FAIL module:automation module:offline module:pending ${module_realtime})
else()
  module_check_test(own_thread FAIL module:automation module:offline)
endif()
module_check_test(logs_in_process FAIL module:threads)
module_check_test(status_waits FAIL module:services)
module_check_test(foreign_request FAIL module:threads)
module_check_test(bad_status FAIL module:services)

# Routed Special FX: a selector choosing a serial and a branch route, a static branch, and
# defects that make the branch output follow its mix or bypass parameter.
set(branch_route "{\"input\": \"chain-input\", \"mix\": {\"parameter\": 2, \"law\": \"crossfade\"}}")
set(selected_routes "\"routingSelector\": {\"parameter\": 3, \"routes\": [{\"placement\": \"before-pre-fx\"}, {\"placement\": \"after-eq\", \"routing\": ${branch_route}}]}")
foreach(variant selected follows_mix leaks_dry stepped_mix unsnapped_mix)
  module_check_package(routed_${variant} BINARY module_check_routed_${variant}
    ID test.module-check.routed CATEGORY special-fx SCHEMA 2 EXTRA "${selected_routes}")
endforeach()
module_check_package(routed_static BINARY module_check_routed_static
  ID test.module-check.routed CATEGORY special-fx SCHEMA 2
  EXTRA "\"placement\": \"after-eq\", \"routing\": ${branch_route}")
# Route has two values, so it cannot choose among three routes.
module_check_package(routed_mismatched_selector BINARY module_check_routed_selected
  ID test.module-check.routed CATEGORY special-fx SCHEMA 2
  EXTRA "\"routingSelector\": {\"parameter\": 3, \"routes\": [{\"placement\": \"before-pre-fx\"}, {\"placement\": \"after-eq\", \"routing\": ${branch_route}}, {\"placement\": \"after-post-fx\"}]}")
module_check_test(routed_selected PASS package:manifest module:abi module:routing module:bypass
  module:latency module:state module:automation)
module_check_test(routed_static PASS package:manifest module:routing)
module_check_test(routed_follows_mix FAIL module:routing)
module_check_test(routed_stepped_mix PASS package:manifest module:abi module:lifecycle
  module:blocks module:automation module:bypass module:state module:robustness module:latency
  module:services module:threads module:routing ${module_realtime})
module_check_test(routed_unsnapped_mix FAIL module:routing)
module_check_test(routed_leaks_dry FAIL module:routing)
module_check_test(routed_mismatched_selector FAIL module:routing)

add_test(NAME module_check_window_background COMMAND ${CMAKE_COMMAND}
  "-DTOOL=$<TARGET_FILE:vespera-module-check>" "-DPACKAGE=${packages}/gain.vespera"
  "-DFIXTURES=${PROJECT_SOURCE_DIR}/tests/fixtures/window_background"
  "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/background"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/background.cmake")

add_test(NAME module_check_manifest_values COMMAND ${CMAKE_COMMAND}
  "-DTOOL=$<TARGET_FILE:vespera-module-check>" "-DPACKAGE=${packages}/gain.vespera"
  "-DVALUES=${PROJECT_SOURCE_DIR}/tests/fixtures/manifest_values.json"
  "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/manifest_values"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/manifest_values.cmake")

add_test(NAME module_check_cli COMMAND ${CMAKE_COMMAND}
  "-DTOOL=$<TARGET_FILE:vespera-module-check>" "-DPACKAGE=${packages}/gain.vespera"
  "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli" -P "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")

add_executable(module_check_route_index route_index_test.cpp)
target_link_libraries(module_check_route_index PRIVATE vespera_module_check nlohmann_json::nlohmann_json)
vespera_warnings(module_check_route_index)
add_test(NAME module_check_route_index COMMAND module_check_route_index
  "${PROJECT_SOURCE_DIR}/tests/fixtures/manifest_values.json")

if(VESPERA_SANITIZERS)
  # Dlopened fixture modules stand in for external plugins, which are never built with this
  # tree's sanitizers; AddressSanitizer mis-flags their in-bounds global reads after dlclose
  # and dlopen reuse the mapping, so the fixtures opt out like the ones in tests/.
  get_property(fixtures DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(fixture IN LISTS fixtures)
    get_property(kind TARGET ${fixture} PROPERTY TYPE)
    if(kind STREQUAL "MODULE_LIBRARY")
      target_compile_options(${fixture} PRIVATE -fno-sanitize=address,undefined)
      target_link_options(${fixture} PRIVATE -fno-sanitize=address,undefined)
    endif()
  endforeach()
endif()
