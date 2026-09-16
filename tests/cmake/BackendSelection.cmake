# Test selection without any simulator installation or compiler invocation.
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}/source")
file(WRITE "${TEST_BINARY_DIR}/source/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.16)\nproject(BackendSelection NONE)\ninclude(\"${SOURCE_DIR}/cmake/GazeboBackend.cmake\")\ngst_plane_camera_validate_backend()\n")

function(configure_case name distro expected_result expected_text)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${TEST_BINARY_DIR}/source" -B "${TEST_BINARY_DIR}/${name}"
    "-DGZ_DISTRO=${distro}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  set(log "${output}\n${error}")
  if(expected_result STREQUAL "success")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "${name}: expected configure success:\n${log}")
    endif()
  elseif(result EQUAL 0 OR NOT log MATCHES "${expected_text}")
    message(FATAL_ERROR "${name}: expected rejection (${expected_text}), got ${result}:\n${log}")
  endif()
endfunction()

# Each test run starts clean, so the migration case is repeatable.
file(REMOVE_RECURSE "${TEST_BINARY_DIR}/invalid" "${TEST_BINARY_DIR}/migration"
  "${TEST_BINARY_DIR}/classic" "${TEST_BINARY_DIR}/jetty")
configure_case(invalid fortress failure "Unknown GZ_DISTRO")
configure_case(migration harmonic success "")
configure_case(migration jetty failure "separate build-jetty")
configure_case(jetty jetty success "")
configure_case(classic classic failure "Unknown GZ_DISTRO")
