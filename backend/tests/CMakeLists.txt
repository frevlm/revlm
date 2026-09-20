add_library(revlm_test_support INTERFACE)
target_include_directories(revlm_test_support INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(revlm_test_support INTERFACE
  REVLM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

file(GLOB_RECURSE REVLM_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

function(revlm_add_unit_test test_cpp)
  get_filename_component(test_basename "${test_cpp}" NAME_WE)
  set(target "revlm_${test_basename}")
  add_executable(${target} "${test_cpp}")
  target_link_libraries(${target} PRIVATE revlm_core revlm_test_support revlm_warnings)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

foreach(test_cpp IN LISTS REVLM_TEST_SOURCES)
  revlm_add_unit_test("${test_cpp}")
endforeach()

# ---------------------------------------------------------------------------
# Long-running measurement probes (rate-limit / SSE-size / streaming POCs).
# Built on demand (targets revlm_probe_*), NOT registered with ctest — they
# are interactive tools that talk to real upstreams, not assertions.
# ---------------------------------------------------------------------------

option(REVLM_BUILD_PROBES "Build the long-running measurement probes" OFF)

function(revlm_add_probe probe_cpp)
  get_filename_component(probe_basename "${probe_cpp}" NAME_WE)
  set(target "revlm_probe_${probe_basename}")
  add_executable(${target} "${probe_cpp}")
  target_link_libraries(${target} PRIVATE revlm_test_support revlm_warnings)
  target_link_libraries(${target} PRIVATE revlm_http ${CMAKE_DL_LIBS})
  set_target_properties(${target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

if(REVLM_BUILD_PROBES)
  set(REVLM_PROBE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rate_limit_two_pool.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/rate_limit_curl_pool.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/sse_size_probe.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/streaming_proxy_poc.cc")
  foreach(probe_cpp IN LISTS REVLM_PROBE_SOURCES)
    revlm_add_probe("${probe_cpp}")
  endforeach()
  # Probes that use libcurl directly (not via revlm_core).
  foreach(probe_target IN ITEMS revlm_probe_rate_limit_two_pool revlm_probe_rate_limit_curl_pool)
    target_link_libraries(${probe_target} PRIVATE ${CURL_LIBRARIES})
  endforeach()
endif()
