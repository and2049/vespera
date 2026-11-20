function(check_cli name expected_result expected_output)
  execute_process(COMMAND "${HOST}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT "${result}" STREQUAL "${expected_result}")
    message(FATAL_ERROR "${name}: expected exit ${expected_result}, got ${result}\n${output}${error}")
  endif()
  if(NOT "${output}${error}" MATCHES "${expected_output}")
    message(FATAL_ERROR "${name}: expected ${expected_output}\n${output}${error}")
  endif()
endfunction()

check_cli(inspect 0 "node=1 module=dev.vespera.gain" inspect --module "${GAIN}" --module "${GAIN}")
check_cli(metadata 0 "parameter=1 name=Gain unit=linear min=0 max=4 default=1"
  inspect --module "${GAIN}")
check_cli(gain 0 "peak=0.5 peak_frame=0" render --module "${GAIN}" --set 0:1=0.5)
check_cli(mono_gain 0 "peak=0.5 peak_frame=0" render --module "${GAIN}" --set 0:1=0.5
  --graph-channels 1)
check_cli(invalid_graph_channels 2 "--graph-channels" render --graph-channels 3)
check_cli(zero_graph_channels 2 "--graph-channels" stream 0 --input 0 --graph-channels 0)
check_cli(order 0 "peak=0.25 peak_frame=0" render --module "${GAIN}"
  --set 0:1=2 --at 0:0:1=0.25)
check_cli(instances 0 "peak=0.125 peak_frame=0" render --module "${GAIN}" --module "${GAIN}"
  --set 0:1=0.5 --set 1:1=0.25)
check_cli(unknown_node 2 "unknown node ID" render --module "${GAIN}" --set 1:1=0.5)
check_cli(unknown_parameter 2 "unknown parameter ID" render --module "${GAIN}" --set 0:2=0.5)
check_cli(out_of_range 2 "outside bounds" render --module "${GAIN}" --set 0:1=5)
check_cli(non_finite 2 "finite number" render --module "${GAIN}" --set 0:1=nan)
check_cli(trailing_value 2 "finite number" render --module "${GAIN}" --set 0:1=1junk)
check_cli(invalid_target 2 "NODE:PARAM=VALUE" render --set 0=0.5)
check_cli(negative_node 2 "unsigned 64-bit" render --set -1:1=0.5)
check_cli(overflow_id 2 "unsigned 64-bit" render --set 18446744073709551616:1=0.5)
check_cli(end_of_render 2 "within the render length" render --module "${GAIN}"
  --length 128 --at 128:0:1=0.5)

execute_process(COMMAND "${HOST}" render --module "${GAIN}" --length 257 --frames 33
  --set 0:1=0.5 --at 33:0:1=2
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE small_blocks)
execute_process(COMMAND "${HOST}" render --module "${GAIN}" --length 257 --frames 128
  --set 0:1=0.5 --at 33:0:1=2
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE large_blocks)
string(REGEX MATCH "checksum=[0-9a-f]+" small_checksum "${small_blocks}")
string(REGEX MATCH "checksum=[0-9a-f]+" large_checksum "${large_blocks}")
execute_process(COMMAND "${HOST}" render --module "${GAIN}" --length 257 --frames 33
  --graph-channels 1 --set 0:1=0.5 --at 33:0:1=2
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE mono_blocks)
string(REGEX MATCH "checksum=[0-9a-f]+" mono_checksum "${mono_blocks}")
if(NOT small_checksum STREQUAL mono_checksum)
  message(FATAL_ERROR "Mono graph output duplication changed samples\n${small_blocks}${mono_blocks}")
endif()
if(small_checksum STREQUAL "" OR NOT small_checksum STREQUAL large_checksum)
  message(FATAL_ERROR "Render block segmentation changed output\n${small_blocks}${large_blocks}")
endif()

check_cli(output_gain 0 "peak=0\\.(0999[0-9]*|1|10000[0-9]*) peak_frame=0" render --length 64
  --output-gain-db -20)
check_cli(input_gain 0 "peak=(0\\.9999[0-9]*|1|1\\.0000[0-9]*) peak_frame=0" render --module "${GAIN}"
  --length 64 --set 0:1=0.5 --input-gain-db 6.0206)
check_cli(gain_range 2 "--output-gain-db expects a finite number in \\[-60, 24\\]" render
  --output-gain-db 24.5)
check_cli(gain_non_finite 2 "--input-gain-db expects a finite number" render --input-gain-db inf)
check_cli(gain_trailing 2 "--input-gain-db expects a finite number" render --input-gain-db 3dB)
check_cli(gate_threshold_range 2 "--gate-threshold-db expects" render --gate-threshold-db 1)
check_cli(gate_threshold_nan 2 "--gate-threshold-db expects" render --gate-threshold-db nan)
check_cli(transpose_range 2 "--transpose-st expects" render --transpose-st 12.5)
check_cli(doubler_short 2 "--doubler-ms expects" render --doubler-ms 0.5)
check_cli(doubler_long 2 "--doubler-ms expects" render --doubler-ms 41)
check_cli(gate_missing_value 2 "--gate-threshold-db requires a value" render --gate-threshold-db)
check_cli(gate 0 "gate=on gate_threshold_db=-40.*non_finite=0" render --length 4800
  --gate-threshold-db -40)
check_cli(transpose 0 "transpose_st=7 .*input_latency=[1-9][0-9]*.*non_finite=0" render
  --length 4800 --transpose-st 7)
check_cli(doubler 0 "doubler=on doubler_ms=10 .*peak=1 peak_frame=0 non_finite=0" render
  --length 4800 --doubler-ms 10)

execute_process(COMMAND "${HOST}" render --length 4800 --frames 33
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE dry_render)
execute_process(COMMAND "${HOST}" render --length 4800 --frames 33 --doubler
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE doubled_small)
execute_process(COMMAND "${HOST}" render --length 4800 --frames 128 --doubler
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE doubled_large)
execute_process(COMMAND "${HOST}" render --length 4800 --frames 33 --doubler --graph-channels 1
  COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE doubled_mono)
string(REGEX MATCH "checksum=[0-9a-f]+" dry_checksum "${dry_render}")
string(REGEX MATCH "checksum=[0-9a-f]+" doubled_small_checksum "${doubled_small}")
string(REGEX MATCH "checksum=[0-9a-f]+" doubled_large_checksum "${doubled_large}")
string(REGEX MATCH "checksum=[0-9a-f]+" doubled_mono_checksum "${doubled_mono}")
if(dry_checksum STREQUAL "" OR dry_checksum STREQUAL doubled_small_checksum)
  message(FATAL_ERROR "Doubler did not change the render\n${dry_render}${doubled_small}")
endif()
if(NOT doubled_small_checksum STREQUAL doubled_large_checksum OR
   NOT doubled_small_checksum STREQUAL doubled_mono_checksum)
  message(FATAL_ERROR "Doubler output depends on block size or graph layout\n"
    "${doubled_small}${doubled_large}${doubled_mono}")
endif()

check_cli(input_mode_invalid 2 "--input-mode expects mono or stereo" stream 0
  --input-mode quad --input 0)
check_cli(inputs_without_stereo 2 "--inputs requires --input-mode stereo" stream 0 --inputs 0,1)
check_cli(stereo_without_inputs 2 "--input-mode stereo requires --inputs" stream 0
  --input-mode stereo)
check_cli(stereo_with_input 2 "takes --inputs L,R instead of --input" stream 0
  --input-mode stereo --input 0 --inputs 0,1)
check_cli(stereo_mono_graph 2 "requires --graph-channels 2" stream 0 --input-mode stereo
  --inputs 0,1 --graph-channels 1)
check_cli(stereo_same_inputs 2 "--inputs requires two distinct channels" stream 0
  --input-mode stereo --inputs 1,1)
check_cli(stereo_inputs_format 2 "--inputs expects two channels" stream 0
  --input-mode stereo --inputs 1)
check_cli(stream_core_io_invalid 2 "--output-gain-db expects a finite number" stream 0
  --input 0 --output-gain-db 99)

set(changes)
foreach(index RANGE 0 256)
  list(APPEND changes --set 0:1=1)
endforeach()
check_cli(too_many_changes 2 "at most 256" render --module "${GAIN}" ${changes})

file(MAKE_DIRECTORY "${WORK}")
set(preset "${WORK}/roundtrip.json")
check_cli(save 0 "peak=0.5 peak_frame=0" render --module "${GAIN}" --length 64
  --set 0:1=0.5 --save-preset "${preset}")
check_cli(preset_inspect 0 "unresolved=0" inspect --preset "${preset}" --module "${GAIN}")
check_cli(preset_unresolved 0 "unresolved=1" inspect --preset "${preset}")
check_cli(preset_render 0 "peak=0.5 peak_frame=0" render --preset "${preset}"
  --module "${GAIN}" --frames 33)
check_cli(preset_missing_module 1 "module" render --preset "${preset}")
check_cli(save_current_value 0 "peak=0.5 peak_frame=0" render --module "${GAIN}" --length 64
  --set 0:1=0.5 --at 16:0:1=2 --save-preset "${preset}")
check_cli(restore_current_value 0 "peak=2 peak_frame=0" render --preset "${preset}"
  --module "${GAIN}")
check_cli(override_restored_value 0 "peak=0.25 peak_frame=0" render --preset "${preset}"
  --module "${GAIN}" --set 0:1=0.25 --save-preset "${preset}")
check_cli(replaced_file 0 "peak=0.25 peak_frame=0" render --preset "${preset}"
  --module "${GAIN}")
file(READ "${preset}" saved)
string(REPLACE "dev.vespera.gain" "org.test.missing" unresolved "${saved}")
file(WRITE "${WORK}/unresolved.json" "${unresolved}")
check_cli(unknown_module_inspect 0 "module=org.test.missing" inspect
  --preset "${WORK}/unresolved.json" --module "${GAIN}")
file(WRITE "${WORK}/malformed.json" "{not a preset}")
check_cli(malformed_preset 1 "error:" render --preset "${WORK}/malformed.json")
