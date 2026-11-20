foreach(target audio preset)
  add_executable(vespera_${target}_fuzz ${target}_fuzz.cpp)
  target_link_libraries(vespera_${target}_fuzz PRIVATE vespera_presets)
  target_link_options(vespera_${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  vespera_warnings(vespera_${target}_fuzz)
endforeach()
add_dependencies(vespera_audio_fuzz vespera_fixture_gain)
target_compile_definitions(vespera_audio_fuzz PRIVATE VESPERA_GAIN_PATH="$<TARGET_FILE:vespera_fixture_gain>")

add_executable(vespera_live_graph_fuzz live_graph_fuzz.cpp ../scripted_audio_backend.cpp
  ../pinned_audio_engine.cpp ${PROJECT_SOURCE_DIR}/src/audio_session.cpp)
target_include_directories(vespera_live_graph_fuzz PRIVATE
  ${audio_io_core_SOURCE_DIR}/include ${audio_io_core_SOURCE_DIR}/src)
target_link_libraries(vespera_live_graph_fuzz PRIVATE vespera_runtime vespera_core_io ${CMAKE_DL_LIBS})
target_link_options(vespera_live_graph_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_compile_definitions(vespera_live_graph_fuzz PRIVATE VESPERA_GAIN_PATH="$<TARGET_FILE:vespera_fixture_gain>")
add_dependencies(vespera_live_graph_fuzz vespera_fixture_gain)
vespera_warnings(vespera_live_graph_fuzz)

add_executable(vespera_core_io_fuzz core_io_fuzz.cpp)
target_link_libraries(vespera_core_io_fuzz PRIVATE vespera_core_io)
target_link_options(vespera_core_io_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
vespera_warnings(vespera_core_io_fuzz)

if(VESPERA_BUILD_DSP)
  add_executable(vespera_wav_fuzz wav_fuzz.cpp)
  target_compile_features(vespera_wav_fuzz PRIVATE cxx_std_20)
  target_link_options(vespera_wav_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  vespera_warnings(vespera_wav_fuzz)
  add_executable(vespera_dsp_fuzz dsp_fuzz.cpp)
  target_link_libraries(vespera_dsp_fuzz PRIVATE vespera::dsp)
  target_link_options(vespera_dsp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  vespera_warnings(vespera_dsp_fuzz)
  if(BUILD_TESTING)
    file(COPY corpus/wav DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/corpus)
    add_test(NAME fuzz_wav_smoke COMMAND vespera_wav_fuzz
      ${CMAKE_CURRENT_BINARY_DIR}/corpus/wav -runs=500 -max_len=8192)
    add_test(NAME fuzz_dsp_smoke COMMAND vespera_dsp_fuzz -runs=500 -max_len=8192)
  endif()
endif()

if(BUILD_TESTING)
  file(COPY corpus/preset corpus/audio corpus/live DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/corpus)
  add_test(NAME fuzz_core_io_smoke COMMAND vespera_core_io_fuzz -runs=500 -max_len=8192)
  add_test(NAME fuzz_live_graph_smoke COMMAND vespera_live_graph_fuzz
    ${CMAKE_CURRENT_BINARY_DIR}/corpus/live -runs=500 -max_len=8192)
  add_test(NAME fuzz_audio_smoke COMMAND vespera_audio_fuzz
    ${CMAKE_CURRENT_BINARY_DIR}/corpus/audio -runs=500 -max_len=8192)
  add_test(NAME fuzz_preset_smoke COMMAND vespera_preset_fuzz
    ${CMAKE_CURRENT_BINARY_DIR}/corpus/preset -runs=500 -max_len=8192)
endif()
