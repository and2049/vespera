find_package(Threads REQUIRED)

# The gain module moved here from examples/; it is the shared-engine fixture behind many tests.
add_library(vespera_fixture_gain MODULE fixtures/gain_module.cpp)
target_include_directories(vespera_fixture_gain PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_gain PRIVATE cxx_std_20)
set_target_properties(vespera_fixture_gain PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
vespera_warnings(vespera_fixture_gain)

if(APPLE)
  add_executable(vespera_audio_unit_sdk_test audio_unit_sdk_test.cpp)
  target_link_libraries(vespera_audio_unit_sdk_test PRIVATE vespera_runtime
    "-framework AudioToolbox" "-framework CoreFoundation")
  vespera_warnings(vespera_audio_unit_sdk_test)
  add_test(NAME audio_unit_sdk COMMAND vespera_audio_unit_sdk_test)
endif()

if(APPLE)
  add_executable(vespera_main_thread_test main_thread_test.cpp)
  target_include_directories(vespera_main_thread_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(vespera_main_thread_test PRIVATE cxx_std_20)
  target_link_libraries(vespera_main_thread_test PRIVATE Threads::Threads "-framework CoreFoundation")
  vespera_warnings(vespera_main_thread_test)
  add_test(NAME main_thread COMMAND vespera_main_thread_test
    $<TARGET_FILE:vespera_fixture_lifecycle_a> $<TARGET_FILE:vespera_fixture_lifecycle_b>)
  set_tests_properties(main_thread PROPERTIES TIMEOUT 15)
endif()
add_executable(vespera_graph_loader_test graph_loader_test.cpp)
target_link_libraries(vespera_graph_loader_test PRIVATE vespera_runtime Threads::Threads)
vespera_warnings(vespera_graph_loader_test)
add_test(NAME graph_loader COMMAND vespera_graph_loader_test)
set_tests_properties(graph_loader PROPERTIES TIMEOUT 30)
add_library(vespera_fixture_file MODULE fixtures/file_module.cpp)
add_library(vespera_fixture_rejects_missing MODULE fixtures/file_module.cpp)
target_compile_definitions(vespera_fixture_rejects_missing PRIVATE FIXTURE_REJECTS_MISSING)
target_include_directories(vespera_fixture_rejects_missing PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_rejects_missing PRIVATE cxx_std_20)
vespera_warnings(vespera_fixture_rejects_missing)
target_include_directories(vespera_fixture_file PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_file PRIVATE cxx_std_20)
vespera_warnings(vespera_fixture_file)
add_executable(vespera_files_test files_test.cpp)
target_link_libraries(vespera_files_test PRIVATE vespera_presets Threads::Threads)
vespera_warnings(vespera_files_test)
add_dependencies(vespera_files_test vespera_fixture_file)
add_test(NAME files COMMAND vespera_files_test $<TARGET_FILE:vespera_fixture_file>)
add_library(vespera_fixture_midi MODULE fixtures/midi_module.cpp)
target_include_directories(vespera_fixture_midi PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_executable(vespera_midi_test midi_test.cpp fake_audio_engine.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_midi_test PRIVATE $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(vespera_midi_test PRIVATE vespera_runtime vespera_core_io Threads::Threads)
if(APPLE)
  target_link_libraries(vespera_midi_test PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()
vespera_warnings(vespera_midi_test)
add_dependencies(vespera_midi_test vespera_fixture_midi vespera_fixture_gain)
add_test(NAME midi COMMAND vespera_midi_test $<TARGET_FILE:vespera_fixture_midi> $<TARGET_FILE:vespera_fixture_gain>)
add_executable(vespera_tempo_test tempo_test.cpp)
target_link_libraries(vespera_tempo_test PRIVATE vespera_runtime)
vespera_warnings(vespera_tempo_test)
add_test(NAME tempo COMMAND vespera_tempo_test)
add_library(vespera_fixture_tempo MODULE fixtures/tempo_module.cpp)
target_include_directories(vespera_fixture_tempo PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_tempo PRIVATE cxx_std_20)

add_executable(vespera_app_audio_test app_audio_test.cpp fake_audio_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_app_audio_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(vespera_app_audio_test PRIVATE vespera_runtime vespera_core_io nlohmann_json::nlohmann_json)
if(APPLE)
  target_link_libraries(vespera_app_audio_test PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()
vespera_warnings(vespera_app_audio_test)
add_library(vespera_fixture_pedal MODULE fixtures/pedal_module.cpp)
target_include_directories(vespera_fixture_pedal PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_library(vespera_fixture_pedal_offset_bypass MODULE fixtures/pedal_module.cpp)
target_include_directories(vespera_fixture_pedal_offset_bypass PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(vespera_fixture_pedal_offset_bypass PRIVATE
  "PEDAL_ID=\"test.pedal.offset-bypass\"" PEDAL_BYPASS_PARAMETER=3)
set(special_fixture_files "")
foreach(name a b c)
  add_library(vespera_fixture_special_${name} MODULE fixtures/pedal_module.cpp)
  target_include_directories(vespera_fixture_special_${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_definitions(vespera_fixture_special_${name} PRIVATE
    "PEDAL_ID=\"test.special.${name}\"")
  list(APPEND special_fixture_files $<TARGET_FILE:vespera_fixture_special_${name}>)
  list(APPEND special_fixtures vespera_fixture_special_${name})
endforeach()
foreach(index RANGE 8)
  add_library(vespera_fixture_midi_special_${index} MODULE fixtures/midi_module.cpp)
  target_include_directories(vespera_fixture_midi_special_${index} PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_compile_definitions(vespera_fixture_midi_special_${index} PRIVATE
    "MIDI_ID=\"test.midi.special.${index}\"" MIDI_ADDITIVE=1)
  list(APPEND special_fixture_files $<TARGET_FILE:vespera_fixture_midi_special_${index}>)
  list(APPEND special_fixtures vespera_fixture_midi_special_${index})
endforeach()

add_executable(vespera_ui_tokens_test ui_tokens_test.cpp)
target_include_directories(vespera_ui_tokens_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
vespera_warnings(vespera_ui_tokens_test)
add_test(NAME ui_tokens COMMAND vespera_ui_tokens_test)
add_test(NAME ui_token_drift COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_token_drift.cmake)

add_executable(vespera_core_io_test core_io_test.cpp)
target_link_libraries(vespera_core_io_test PRIVATE vespera_core_io)
vespera_warnings(vespera_core_io_test)
add_test(NAME core_io COMMAND vespera_core_io_test)

add_executable(vespera_audio_fuzz_test audio_fuzz_test.cpp)
target_link_libraries(vespera_audio_fuzz_test PRIVATE vespera_runtime)
vespera_warnings(vespera_audio_fuzz_test)
add_dependencies(vespera_audio_fuzz_test vespera_fixture_gain)
add_test(NAME synthetic_audio COMMAND vespera_audio_fuzz_test $<TARGET_FILE:vespera_fixture_gain>)

add_executable(vespera_instance_cache_test instance_cache_test.cpp)
target_link_libraries(vespera_instance_cache_test PRIVATE vespera_runtime)
vespera_warnings(vespera_instance_cache_test)
add_test(NAME instance_cache COMMAND vespera_instance_cache_test)

add_executable(vespera_host_services_test host_services_test.cpp)
target_link_libraries(vespera_host_services_test PRIVATE vespera_runtime Threads::Threads)
vespera_warnings(vespera_host_services_test)
add_test(NAME host_services COMMAND vespera_host_services_test)
set_tests_properties(host_services PROPERTIES TIMEOUT 120)

add_library(vespera_fixture_services MODULE fixtures/services_module.cpp)
target_include_directories(vespera_fixture_services PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_services PRIVATE cxx_std_20)
set_target_properties(vespera_fixture_services PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
vespera_warnings(vespera_fixture_services)
add_executable(vespera_services_runtime_test services_runtime_test.cpp)
target_link_libraries(vespera_services_runtime_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_include_directories(vespera_services_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
vespera_warnings(vespera_services_runtime_test)
add_dependencies(vespera_services_runtime_test vespera_fixture_services vespera_fixture_probe)
add_test(NAME services_runtime COMMAND vespera_services_runtime_test
  $<TARGET_FILE:vespera_fixture_services> $<TARGET_FILE:vespera_fixture_probe>)
set_tests_properties(services_runtime PROPERTIES TIMEOUT 120)

add_executable(vespera_runtime_test runtime_test.cpp)
target_link_libraries(vespera_runtime_test PRIVATE vespera_runtime Threads::Threads)
vespera_warnings(vespera_runtime_test)
foreach(fixture probe no_entry state)
  add_library(vespera_fixture_${fixture} MODULE fixtures/${fixture}_module.c)
  target_include_directories(vespera_fixture_${fixture} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(vespera_fixture_${fixture} PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
endforeach()
target_include_directories(vespera_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vespera_runtime_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(vespera_runtime_test vespera_fixture_gain vespera_fixture_probe vespera_fixture_no_entry)
add_test(NAME runtime COMMAND vespera_runtime_test $<TARGET_FILE:vespera_fixture_gain>
         $<TARGET_FILE:vespera_fixture_probe> $<TARGET_FILE:vespera_fixture_no_entry>)

add_library(vespera_fixture_metadata MODULE fixtures/metadata_module.cpp)
target_include_directories(vespera_fixture_metadata PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_metadata PRIVATE cxx_std_20)
set_target_properties(vespera_fixture_metadata PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
vespera_warnings(vespera_fixture_metadata)
add_executable(vespera_parameter_display_test parameter_display_test.cpp)
target_link_libraries(vespera_parameter_display_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_include_directories(vespera_parameter_display_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
vespera_warnings(vespera_parameter_display_test)
add_dependencies(vespera_parameter_display_test vespera_fixture_metadata vespera_fixture_gain
  vespera_fixture_probe)
add_test(NAME parameter_display COMMAND vespera_parameter_display_test
  $<TARGET_FILE:vespera_fixture_metadata> $<TARGET_FILE:vespera_fixture_gain>
  $<TARGET_FILE:vespera_fixture_probe>)

set(shared_fixtures "")
function(vespera_shared_fixture name)
  set(target vespera_fixture_shared_${name})
  add_library(${target} MODULE fixtures/shared_module.cpp)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE "SHARED_ID=\"test.shared.${name}\"" ${ARGN})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  set(shared_fixtures ${shared_fixtures} ${target} PARENT_SCOPE)
endfunction()
vespera_shared_fixture(a SHARED_STAGE=1)
vespera_shared_fixture(b SHARED_STAGE=2)
vespera_shared_fixture(c SHARED_STAGE=3 SHARED_MODEL=1)
vespera_shared_fixture(d SHARED_STAGE=4 SHARED_MODEL=2)
vespera_shared_fixture(e SHARED_STAGE=5 SHARED_MODEL=1)
vespera_shared_fixture(lead SHARED_STAGE=6 SHARED_LEADING=1 SHARED_MIDI=1)
vespera_shared_fixture(late SHARED_STAGE=7)
vespera_shared_fixture(twin SHARED_STAGE=8 SHARED_SECTION=2)
vespera_shared_fixture(other SHARED_STAGE=9 "SHARED_ENGINE=\"test.other/1\"")
vespera_shared_fixture(renamed SHARED_STAGE=10 "SHARED_ENABLE_PREFIX=\"Part \"")
vespera_shared_fixture(bad SHARED_STAGE=11 SHARED_BAD_ROLE=1)
vespera_shared_fixture(retain SHARED_STAGE=13 SHARED_SECTION=11 SHARED_RETAINS=1)
vespera_shared_fixture(tempo SHARED_STAGE=14 SHARED_SECTION=10 SHARED_TEMPO=1)
vespera_shared_fixture(work SHARED_STAGE=0 SHARED_SECTION=4 SHARED_SERVICES=1)
add_executable(vespera_shared_test shared_test.cpp)
target_link_libraries(vespera_shared_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_include_directories(vespera_shared_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
vespera_warnings(vespera_shared_test)
add_dependencies(vespera_shared_test ${shared_fixtures})
set(shared_fixture_files "")
foreach(fixture IN LISTS shared_fixtures)
  list(APPEND shared_fixture_files $<TARGET_FILE:${fixture}>)
endforeach()
add_test(NAME shared_engine COMMAND vespera_shared_test ${shared_fixture_files})

add_dependencies(vespera_app_audio_test vespera_fixture_gain vespera_fixture_pedal vespera_fixture_tempo
  vespera_fixture_shared_a vespera_fixture_shared_b vespera_fixture_pedal_offset_bypass
  vespera_fixture_shared_lead ${special_fixtures})
add_test(NAME app_audio COMMAND vespera_app_audio_test $<TARGET_FILE:vespera_fixture_gain>
  $<TARGET_FILE:vespera_fixture_pedal> $<TARGET_FILE:vespera_fixture_tempo>
  $<TARGET_FILE:vespera_fixture_shared_a> $<TARGET_FILE:vespera_fixture_shared_b>
  $<TARGET_FILE:vespera_fixture_pedal_offset_bypass> $<TARGET_FILE:vespera_fixture_shared_lead>
  ${special_fixture_files})

add_executable(vespera_app_audio_services_test app_audio_services_test.cpp fake_audio_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_app_audio_services_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_features(vespera_app_audio_services_test PRIVATE cxx_std_20)
target_link_libraries(vespera_app_audio_services_test PRIVATE vespera_runtime vespera_core_io
  nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(vespera_app_audio_services_test PRIVATE "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
vespera_warnings(vespera_app_audio_services_test)
add_dependencies(vespera_app_audio_services_test vespera_fixture_services)
add_test(NAME app_audio_services COMMAND vespera_app_audio_services_test
  $<TARGET_FILE:vespera_fixture_services>)
set_tests_properties(app_audio_services PROPERTIES TIMEOUT 120)

add_library(vespera_fixture_opaque_state MODULE fixtures/opaque_state_module.cpp)
target_include_directories(vespera_fixture_opaque_state PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(vespera_fixture_opaque_state PRIVATE cxx_std_20)
set_target_properties(vespera_fixture_opaque_state PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
add_executable(vespera_app_audio_state_test app_audio_state_test.cpp fake_audio_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_app_audio_state_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_features(vespera_app_audio_state_test PRIVATE cxx_std_20)
target_link_libraries(vespera_app_audio_state_test PRIVATE vespera_runtime vespera_core_io
  nlohmann_json::nlohmann_json)
if(APPLE)
  target_link_libraries(vespera_app_audio_state_test PRIVATE "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
vespera_warnings(vespera_app_audio_state_test)
add_dependencies(vespera_app_audio_state_test vespera_fixture_opaque_state)
add_test(NAME app_audio_state COMMAND vespera_app_audio_state_test
  $<TARGET_FILE:vespera_fixture_opaque_state>)

foreach(copy a b unusable)
  add_library(vespera_fixture_lifecycle_${copy} MODULE fixtures/lifecycle_module.cpp)
  target_include_directories(vespera_fixture_lifecycle_${copy} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(vespera_fixture_lifecycle_${copy} PRIVATE cxx_std_20)
  set_target_properties(vespera_fixture_lifecycle_${copy} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  if(APPLE)
    target_link_libraries(vespera_fixture_lifecycle_${copy} PRIVATE "-framework CoreFoundation")
  endif()
endforeach()
set(unusable_lifecycle_directory "VESPERA_PLUGIN_LIFECYCLE_DIRECTORY=\"/nonexistent/vespera-lifecycle\"")
target_compile_definitions(vespera_fixture_lifecycle_unusable PRIVATE ${unusable_lifecycle_directory})
add_executable(vespera_lifecycle_test lifecycle_test.cpp)
target_include_directories(vespera_lifecycle_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vespera_lifecycle_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
vespera_warnings(vespera_lifecycle_test)
if(APPLE)
  add_dependencies(vespera_main_thread_test vespera_fixture_lifecycle_a vespera_fixture_lifecycle_b)
  foreach(copy usable unusable)
    add_library(vespera_fixture_lifecycle_au_${copy} MODULE fixtures/lifecycle_au_module.cpp)
    target_include_directories(vespera_fixture_lifecycle_au_${copy} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(vespera_fixture_lifecycle_au_${copy} PRIVATE cxx_std_20)
    target_link_libraries(vespera_fixture_lifecycle_au_${copy} PRIVATE "-framework AudioToolbox"
      "-framework CoreFoundation")
    set_target_properties(vespera_fixture_lifecycle_au_${copy} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  endforeach()
  target_compile_definitions(vespera_fixture_lifecycle_au_unusable PRIVATE ${unusable_lifecycle_directory})
  add_dependencies(vespera_lifecycle_test vespera_fixture_lifecycle_a vespera_fixture_lifecycle_b
    vespera_fixture_lifecycle_unusable vespera_fixture_lifecycle_au_unusable
    vespera_fixture_lifecycle_au_usable)
  add_test(NAME plugin_lifecycle COMMAND vespera_lifecycle_test
    $<TARGET_FILE:vespera_fixture_lifecycle_a> $<TARGET_FILE:vespera_fixture_lifecycle_b>
    $<TARGET_FILE:vespera_fixture_lifecycle_unusable>
    $<TARGET_FILE:vespera_fixture_lifecycle_au_unusable>
    $<TARGET_FILE:vespera_fixture_lifecycle_au_usable>)
endif()

add_executable(vespera_state_test state_test.cpp)
target_link_libraries(vespera_state_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_include_directories(vespera_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
vespera_warnings(vespera_state_test)
add_dependencies(vespera_state_test vespera_fixture_gain vespera_fixture_state vespera_fixture_probe)
add_test(NAME state COMMAND vespera_state_test $<TARGET_FILE:vespera_fixture_gain>
         $<TARGET_FILE:vespera_fixture_state> $<TARGET_FILE:vespera_fixture_probe>)

add_executable(vespera_preset_test preset_test.cpp)
target_link_libraries(vespera_preset_test PRIVATE vespera_presets)
vespera_warnings(vespera_preset_test)
add_test(NAME preset COMMAND vespera_preset_test)

add_executable(vespera_audio_session_test audio_session_test.cpp fake_audio_engine.cpp
               ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_audio_session_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(vespera_audio_session_test PRIVATE vespera_runtime vespera_core_io)
vespera_warnings(vespera_audio_session_test)
add_dependencies(vespera_audio_session_test vespera_fixture_gain)
add_test(NAME audio_session COMMAND vespera_audio_session_test $<TARGET_FILE:vespera_fixture_gain>)
set_tests_properties(audio_session PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_executable(vespera_live_graph_test live_graph_test.cpp fake_audio_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_live_graph_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(vespera_live_graph_test PRIVATE vespera_runtime vespera_core_io ${CMAKE_DL_LIBS})
vespera_warnings(vespera_live_graph_test)
add_dependencies(vespera_live_graph_test vespera_fixture_gain vespera_fixture_probe)
add_test(NAME live_graph COMMAND vespera_live_graph_test $<TARGET_FILE:vespera_fixture_gain>
  $<TARGET_FILE:vespera_fixture_probe>)
set_tests_properties(live_graph PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_executable(vespera_audio_engine_integration_test audio_engine_integration_test.cpp
  scripted_audio_backend.cpp pinned_audio_engine.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_audio_engine_integration_test PRIVATE
  ${audio_io_core_SOURCE_DIR}/include ${audio_io_core_SOURCE_DIR}/src)
target_link_libraries(vespera_audio_engine_integration_test PRIVATE vespera_runtime vespera_core_io
  Threads::Threads ${CMAKE_DL_LIBS})
vespera_warnings(vespera_audio_engine_integration_test)
add_dependencies(vespera_audio_engine_integration_test vespera_fixture_probe vespera_fixture_gain)
add_test(NAME audio_engine_integration COMMAND vespera_audio_engine_integration_test
  $<TARGET_FILE:vespera_fixture_probe> $<TARGET_FILE:vespera_fixture_gain>)

add_executable(vespera_parameter_queue_test parameter_queue_test.cpp)
target_link_libraries(vespera_parameter_queue_test PRIVATE vespera_runtime Threads::Threads)
vespera_warnings(vespera_parameter_queue_test)
add_test(NAME parameter_queue COMMAND vespera_parameter_queue_test)

add_test(NAME cli COMMAND ${CMAKE_COMMAND} -DHOST=$<TARGET_FILE:vespera>
  -DGAIN=$<TARGET_FILE:vespera_fixture_gain> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-presets
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake)

add_executable(vespera_core_io_session_test core_io_session_test.cpp scripted_audio_backend.cpp
  pinned_audio_engine.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_core_io_session_test PRIVATE
  ${audio_io_core_SOURCE_DIR}/include ${audio_io_core_SOURCE_DIR}/src)
target_link_libraries(vespera_core_io_session_test PRIVATE vespera_runtime vespera_core_io
  Threads::Threads ${CMAKE_DL_LIBS})
vespera_warnings(vespera_core_io_session_test)
add_dependencies(vespera_core_io_session_test vespera_fixture_gain)
add_test(NAME core_io_session COMMAND vespera_core_io_session_test $<TARGET_FILE:vespera_fixture_gain>)

add_executable(vespera_kit_test kit_test.cpp)
target_include_directories(vespera_kit_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
vespera_warnings(vespera_kit_test)
add_test(NAME kit COMMAND vespera_kit_test)

add_executable(vespera_kit_host_test kit_host_test.cpp)
target_link_libraries(vespera_kit_host_test PRIVATE vespera_runtime Threads::Threads)
vespera_warnings(vespera_kit_host_test)
add_test(NAME kit_host COMMAND vespera_kit_host_test)
set_tests_properties(kit_host PROPERTIES TIMEOUT 120)


set(kit_header_sources "")
foreach(header audio bypass controls host module parameters state)
  set(source ${CMAKE_CURRENT_BINARY_DIR}/kit_header_${header}.cpp)
  file(GENERATE OUTPUT ${source} CONTENT "#include <vespera/kit/${header}.hpp>\n")
  list(APPEND kit_header_sources ${source})
endforeach()
add_library(vespera_kit_headers OBJECT ${kit_header_sources})
target_include_directories(vespera_kit_headers PRIVATE ${PROJECT_SOURCE_DIR}/include)
vespera_warnings(vespera_kit_headers)

set(kit_rejections
  "ids must be non-zero" "ids must be unique" "names must be non-empty"
  "ranges must be finite" "defaults must lie within" "positive skew"
  "smoothing must be within" "at most one bypass parameter" "must range from 0 to 1"
  "a bypass policy needs" "exactly one of" "must declare tail_frames"
  "steps need 2 to 65536" "default must be one of its steps" "choice labels must be non-empty"
  "shown by name" "shown numbers must be different" "skew must be at most 1000"
  "decimals must be at most 6" "units must be at most 64 bytes" "off tolerance must be finite"
  "the bypass parameter must be a toggle")
add_test(NAME kit_config_accepted COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only
  -I${PROJECT_SOURCE_DIR}/include -DKIT_CASE=0 ${CMAKE_CURRENT_SOURCE_DIR}/kit_invalid_config.cpp)
set(kit_case 1)
foreach(message IN LISTS kit_rejections)
  add_test(NAME kit_config_rejected_${kit_case} COMMAND ${CMAKE_CXX_COMPILER} -std=c++20
    -fsyntax-only -I${PROJECT_SOURCE_DIR}/include -DKIT_CASE=${kit_case}
    ${CMAKE_CURRENT_SOURCE_DIR}/kit_invalid_config.cpp)
  set_tests_properties(kit_config_rejected_${kit_case} PROPERTIES
    PASS_REGULAR_EXPRESSION "${message}")
  math(EXPR kit_case "${kit_case} + 1")
endforeach()

add_subdirectory(module_check)

set(branch_fixtures "")
function(vespera_branch_fixture name)
  set(target vespera_fixture_branch_${name})
  add_library(${target} MODULE fixtures/branch_module.cpp)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE "BRANCH_ID=\"test.branch.${name}\"" ${ARGN})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  vespera_warnings(${target})
  set(branch_fixtures ${branch_fixtures} ${target} PARENT_SCOPE)
endfunction()
vespera_branch_fixture(plain)
vespera_branch_fixture(no_values BRANCH_VALUES=0)
vespera_branch_fixture(no_bypass BRANCH_BYPASS=0)
vespera_branch_fixture(midi BRANCH_MIDI=1)
vespera_branch_fixture(stepped BRANCH_STEPPED_MIX=1)
add_executable(vespera_graph_branch_test graph_branch_test.cpp)
target_link_libraries(vespera_graph_branch_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_include_directories(vespera_graph_branch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vespera_graph_branch_test PRIVATE cxx_std_20)
vespera_warnings(vespera_graph_branch_test)
add_dependencies(vespera_graph_branch_test ${branch_fixtures} vespera_fixture_shared_a
  vespera_fixture_shared_b vespera_fixture_shared_c vespera_fixture_shared_retain)
add_test(NAME graph_branch COMMAND vespera_graph_branch_test
  $<TARGET_FILE:vespera_fixture_branch_plain> $<TARGET_FILE:vespera_fixture_branch_no_values>
  $<TARGET_FILE:vespera_fixture_branch_no_bypass> $<TARGET_FILE:vespera_fixture_shared_a>
  $<TARGET_FILE:vespera_fixture_shared_b> $<TARGET_FILE:vespera_fixture_shared_c>
  $<TARGET_FILE:vespera_fixture_shared_retain> $<TARGET_FILE:vespera_fixture_branch_midi>
  $<TARGET_FILE:vespera_fixture_branch_stepped>)

# Audio bridge Special FX routes: route selector and branch fixtures and their bridge test.
function(vespera_route_fixture name)
  set(target vespera_fixture_route_${name})
  add_library(${target} MODULE fixtures/route_module.cpp)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE "ROUTE_ID=\"test.route.${name}\"" ${ARGN})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
endfunction()
vespera_route_fixture(a)
vespera_route_fixture(b)
vespera_route_fixture(three ROUTE_COUNT=3)
vespera_route_fixture(slow ROUTE_LATENCY_1=24)
vespera_route_fixture(failing ROUTE_FAILS=1)
add_executable(vespera_app_audio_route_test app_audio_route_test.cpp fake_audio_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_app_audio_route_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_features(vespera_app_audio_route_test PRIVATE cxx_std_20)
target_link_libraries(vespera_app_audio_route_test PRIVATE vespera_runtime vespera_core_io
  nlohmann_json::nlohmann_json)
if(APPLE)
  target_link_libraries(vespera_app_audio_route_test PRIVATE "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
vespera_warnings(vespera_app_audio_route_test)
add_dependencies(vespera_app_audio_route_test vespera_fixture_gain vespera_fixture_pedal
  vespera_fixture_shared_a vespera_fixture_route_a vespera_fixture_route_b
  vespera_fixture_route_three vespera_fixture_route_slow vespera_fixture_route_failing)
add_test(NAME app_audio_route COMMAND vespera_app_audio_route_test $<TARGET_FILE:vespera_fixture_gain>
  $<TARGET_FILE:vespera_fixture_pedal> $<TARGET_FILE:vespera_fixture_shared_a>
  $<TARGET_FILE:vespera_fixture_route_a> $<TARGET_FILE:vespera_fixture_route_b>
  $<TARGET_FILE:vespera_fixture_route_three> $<TARGET_FILE:vespera_fixture_route_slow>
  $<TARGET_FILE:vespera_fixture_route_failing>)

# Native branch absorption uses only synthetic shared engines.
function(vespera_native_branch_fixture name section)
  set(target vespera_fixture_native_branch_${name})
  add_library(${target} MODULE fixtures/native_branch_module.cpp)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE "NATIVE_ID=\"test.native.${name}\""
    NATIVE_SECTION=${section} ${ARGN})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  vespera_warnings(${target})
endfunction()
vespera_native_branch_fixture(main 0)
vespera_native_branch_fixture(branch 1)
vespera_native_branch_fixture(post 2)
vespera_native_branch_fixture(extra 3)
vespera_native_branch_fixture(older 1 NATIVE_OLDER)
add_executable(vespera_native_branch_test native_branch_test.cpp)
target_link_libraries(vespera_native_branch_test PRIVATE vespera_runtime ${CMAKE_DL_LIBS})
target_compile_features(vespera_native_branch_test PRIVATE cxx_std_20)
vespera_warnings(vespera_native_branch_test)
add_dependencies(vespera_native_branch_test vespera_fixture_native_branch_main
  vespera_fixture_native_branch_branch vespera_fixture_native_branch_post
  vespera_fixture_native_branch_extra vespera_fixture_native_branch_older)
add_test(NAME native_branch COMMAND vespera_native_branch_test
  $<TARGET_FILE:vespera_fixture_native_branch_main> $<TARGET_FILE:vespera_fixture_native_branch_branch>
  $<TARGET_FILE:vespera_fixture_native_branch_post> $<TARGET_FILE:vespera_fixture_native_branch_extra>
  $<TARGET_FILE:vespera_fixture_native_branch_older>)

add_executable(vespera_app_audio_native_branch_test app_audio_native_branch_test.cpp
  fake_audio_engine.cpp ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_app_audio_native_branch_test PRIVATE
  $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_features(vespera_app_audio_native_branch_test PRIVATE cxx_std_20)
target_link_libraries(vespera_app_audio_native_branch_test PRIVATE vespera_runtime vespera_core_io
  nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(vespera_app_audio_native_branch_test PRIVATE "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
vespera_warnings(vespera_app_audio_native_branch_test)
add_dependencies(vespera_app_audio_native_branch_test vespera_fixture_native_branch_main
  vespera_fixture_native_branch_branch vespera_fixture_native_branch_post
  vespera_fixture_midi_special_0)
add_test(NAME app_audio_native_branch COMMAND vespera_app_audio_native_branch_test
  $<TARGET_FILE:vespera_fixture_native_branch_main> $<TARGET_FILE:vespera_fixture_native_branch_branch>
  $<TARGET_FILE:vespera_fixture_native_branch_post> $<TARGET_FILE:vespera_fixture_midi_special_0>)
set_tests_properties(app_audio_native_branch PROPERTIES TIMEOUT 30)

if(APPLE)
  add_library(vespera_fixture_bridge_dispatch MODULE fixtures/bridge_dispatch_module.cpp)
  target_include_directories(vespera_fixture_bridge_dispatch PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(vespera_fixture_bridge_dispatch PRIVATE cxx_std_20)
  target_link_libraries(vespera_fixture_bridge_dispatch PRIVATE "-framework CoreFoundation")
  vespera_warnings(vespera_fixture_bridge_dispatch)
  add_executable(vespera_app_audio_dispatch_test app_audio_dispatch_test.cpp fake_audio_engine.cpp
    ${PROJECT_SOURCE_DIR}/src/app_audio.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
  target_include_directories(vespera_app_audio_dispatch_test PRIVATE
    $<TARGET_PROPERTY:audio_engine,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_features(vespera_app_audio_dispatch_test PRIVATE cxx_std_20)
  target_link_libraries(vespera_app_audio_dispatch_test PRIVATE vespera_runtime vespera_core_io
    nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS}
    "-framework CoreMIDI" "-framework CoreFoundation")
  vespera_warnings(vespera_app_audio_dispatch_test)
  add_dependencies(vespera_app_audio_dispatch_test vespera_fixture_bridge_dispatch)
  add_test(NAME app_audio_dispatch COMMAND vespera_app_audio_dispatch_test
    $<TARGET_FILE:vespera_fixture_bridge_dispatch>)
  set_tests_properties(app_audio_dispatch PROPERTIES TIMEOUT 30)
endif()

if(VESPERA_SANITIZERS)
  # Dlopened fixture modules stand in for external plugins, which are never built with this
  # tree's sanitizers. AddressSanitizer on macOS mis-flags in-bounds reads of globals in
  # dlopened instrumented images once dlclose and a later dlopen reuse their mapping, so the
  # fixtures opt out while every test executable keeps full instrumentation.
  get_property(fixtures DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(fixture IN LISTS fixtures)
    get_property(kind TARGET ${fixture} PROPERTY TYPE)
    if(kind STREQUAL "MODULE_LIBRARY")
      target_compile_options(${fixture} PRIVATE -fno-sanitize=address,undefined)
      target_link_options(${fixture} PRIVATE -fno-sanitize=address,undefined)
    endif()
  endforeach()
endif()
