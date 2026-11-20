# Checks windowBackground manifest validation against the PNG fixtures the Swift host tests share.
# Each case copies PACKAGE to WORK, rewrites its manifest and runs only the manifest check.
file(READ "${PACKAGE}/module.json" original)
string(JSON id GET "${original}" id)

function(check_background name category field expected_exit expected_output)
  set(package "${WORK}/${name}.vespera")
  file(REMOVE_RECURSE "${package}")
  file(COPY "${PACKAGE}/" DESTINATION "${package}")
  foreach(fixture IN LISTS ARGN)
    file(COPY "${FIXTURES}/${fixture}" DESTINATION "${package}/resources")
  endforeach()
  set(member "")
  if(NOT field STREQUAL "")
    set(member ", \"windowBackground\": ${field}")
  endif()
  file(WRITE "${package}/module.json"
    "{\"schemaVersion\": 1, \"id\": \"${id}\", \"category\": \"${category}\", "
    "\"binary\": \"module.so\", \"icon\": \"icon.svg\", \"resources\": \"resources\"${member}}\n")
  if(COMMAND prepare_${name})
    cmake_language(CALL prepare_${name} "${package}")
  endif()
  execute_process(COMMAND "${TOOL}" --checks manifest "${package}"
    RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT exit STREQUAL expected_exit)
    message(FATAL_ERROR "${name}: expected exit ${expected_exit}, got ${exit}\n${output}${error}")
  endif()
  if(NOT "${output}${error}" MATCHES "${expected_output}")
    message(FATAL_ERROR "${name}: expected output matching ${expected_output}\n${output}${error}")
  endif()
endfunction()

check_background(absent amp "" 0 "PASS +package +manifest +module.json declares [^\n]* \\(amp\\)\n")
check_background(null_amp amp "null" 0 "PASS +package +manifest")
check_background(null_pedal pre-fx "null" 0 "PASS +package +manifest")
check_background(valid amp "\"resources/valid.png\"" 0 "window background 8x4" valid.png)
check_background(widest amp "\"resources/widest.png\"" 0 "window background 4096x1" widest.png)
check_background(pixel_budget amp "\"resources/pixel-budget.png\"" 0
  "window background 4000x2000" pixel-budget.png)
# Header validation alone accepts these; the macOS host rejects them when it decodes the image.
foreach(header_only truncated-data corrupt-data bad-stream short-data animated)
  string(REPLACE "-" "_" case "${header_only}")
  check_background(${case} amp "\"resources/${header_only}.png\"" 0 "PASS +package +manifest"
    ${header_only}.png)
endforeach()

foreach(oversized too-wide too-tall too-many-pixels)
  string(REPLACE "-" "_" case "${oversized}")
  check_background(${case} amp "\"resources/${oversized}.png\"" 1
    "FAIL +package +manifest +windowBackground [^\n]* pixels; window backgrounds may be at most 4096 pixels per side and 8000000 pixels in total"
    ${oversized}.png)
endforeach()
check_background(empty_dimension amp "\"resources/empty-dimension.png\"" 1 "has no pixels"
  empty-dimension.png)
check_background(truncated_header amp "\"resources/truncated-header.png\"" 1 "is truncated"
  truncated-header.png)
check_background(not_png amp "\"resources/not-png.png\"" 1 "is not a PNG image" not-png.png)

check_background(pedal pre-fx "\"resources/valid.png\"" 1 "only amps declare a windowBackground"
  valid.png)
check_background(number amp "1" 1 "windowBackground must be a string or null")
check_background(empty amp "\"\"" 1 "must be a relative path inside the package")
check_background(parent amp "\"../valid.png\"" 1 "must be a relative path inside the package")
check_background(absolute amp "\"${FIXTURES}/valid.png\"" 1
  "must be a relative path inside the package")
check_background(missing amp "\"resources/absent.png\"" 1 "does not exist")
check_background(directory amp "\"resources\"" 1 "must be inside the resources directory")

function(prepare_outside package)
  file(COPY "${FIXTURES}/valid.png" DESTINATION "${package}")
endfunction()
check_background(outside amp "\"valid.png\"" 1 "must be inside the resources directory")

function(prepare_folder package)
  file(MAKE_DIRECTORY "${package}/resources/folder.png")
endfunction()
check_background(folder amp "\"resources/folder.png\"" 1 "is not a file")

function(prepare_escape package)
  file(CREATE_LINK "${FIXTURES}/valid.png" "${package}/resources/escape.png" SYMBOLIC)
endfunction()
check_background(escape amp "\"resources/escape.png\"" 1 "escapes the package")

function(prepare_inside_link package)
  file(COPY "${FIXTURES}/valid.png" DESTINATION "${package}/resources/art")
  file(CREATE_LINK "art/valid.png" "${package}/resources/link.png" SYMBOLIC)
endfunction()
check_background(inside_link amp "\"resources/link.png\"" 0 "window background 8x4")
