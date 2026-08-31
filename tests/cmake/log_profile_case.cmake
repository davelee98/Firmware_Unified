# One od_select_log_profile() case, run in cmake script mode so no target owns the result.
#
# Invoked as: cmake -DCASE=<name> -P log_profile_case.cmake
#
# Every failure here is a FATAL_ERROR, so exit status alone cannot tell a refuse case that the
# selector correctly rejected from one where the selector returned and the fixture's own
# "should not have got here" line fired. Both are non-zero. So a refuse case prints the sentinel
# below on the way past the call, and the runner treats its presence as the failure. Without it
# the fixture passes against a selector whose guards have been deleted outright.

cmake_minimum_required(VERSION 3.16)

set(reached "SELECTOR-RETURNED")

include(${CMAKE_CURRENT_LIST_DIR}/../../shared/profiles.cmake)

function(expect_list actual_var)
  set(_expect ${ARGN})
  if(NOT "${${actual_var}}" STREQUAL "${_expect}")
    message(FATAL_ERROR "${actual_var} is '${${actual_var}}', expected '${_expect}'")
  endif()
endfunction()

if("${CASE}" STREQUAL "info")
  set(defines "")
  od_select_log_profile(info defines)
  expect_list(defines OD_LOG_LEVEL=OD_LOG_INFO)

elseif("${CASE}" STREQUAL "debug")
  set(defines "")
  od_select_log_profile(debug defines)
  expect_list(defines OD_LOG_LEVEL=OD_LOG_DEBUG)

elseif("${CASE}" STREQUAL "appends")
  # A board's own definitions survive, and the selector contributes exactly one entry.
  set(defines OD_BOARD_XIAO=1 OD_TX_POWER_DBM=8)
  od_select_log_profile(debug defines)
  expect_list(defines OD_BOARD_XIAO=1 OD_TX_POWER_DBM=8 OD_LOG_LEVEL=OD_LOG_DEBUG)

elseif("${CASE}" STREQUAL "empty")
  set(defines "")
  od_select_log_profile("" defines)
  message(FATAL_ERROR "${reached}: empty profile was accepted")

elseif("${CASE}" STREQUAL "unknown")
  set(defines "")
  od_select_log_profile(verbose defines)
  message(FATAL_ERROR "${reached}: unknown profile was accepted")

elseif("${CASE}" STREQUAL "wrong-case")
  # Exactly "info" or "debug": a spelling the front door did not agree to is not a near miss to
  # be repaired here, because repairing it is how a third spelling gets into a build file.
  set(defines "")
  od_select_log_profile(INFO defines)
  message(FATAL_ERROR "${reached}: INFO was accepted as a profile name")

elseif("${CASE}" STREQUAL "duplicate")
  set(defines OD_LOG_LEVEL=OD_LOG_INFO)
  od_select_log_profile(debug defines)
  message(FATAL_ERROR
          "${reached}: a destination already carrying OD_LOG_LEVEL was appended to")

elseif("${CASE}" STREQUAL "duplicate-bare")
  # Valueless, which CMake turns into -DOD_LOG_LEVEL. GCC and Clang both define that as 1, which
  # is OD_LOG_WARN -- so it builds clean at the wrong level rather than failing, and nothing
  # downstream says the requested profile was overruled.
  set(defines OD_LOG_LEVEL)
  od_select_log_profile(debug defines)
  message(FATAL_ERROR
          "${reached}: a destination carrying a bare OD_LOG_LEVEL was appended to")

elseif("${CASE}" STREQUAL "duplicate-dashd")
  # CMake tolerates a -D prefix in a definition list and strips it, so this reaches the compiler
  # as the same macro the selector is about to emit.
  set(defines -DOD_LOG_LEVEL=OD_LOG_INFO)
  od_select_log_profile(debug defines)
  message(FATAL_ERROR
          "${reached}: a destination carrying -DOD_LOG_LEVEL was appended to")

elseif("${CASE}" STREQUAL "duplicate-conditional-genex")
  set(defines "$<$<CONFIG:Debug>:OD_LOG_LEVEL=OD_LOG_DEBUG>")
  od_select_log_profile(info defines)
  message(FATAL_ERROR
          "${reached}: a conditional OD_LOG_LEVEL generator expression was appended to")

elseif("${CASE}" STREQUAL "duplicate-if-genex")
  set(defines "$<IF:$<CONFIG:Debug>,OD_LOG_LEVEL=OD_LOG_DEBUG,OD_LOG_LEVEL=OD_LOG_INFO>")
  od_select_log_profile(info defines)
  message(FATAL_ERROR
          "${reached}: an IF OD_LOG_LEVEL generator expression was appended to")

elseif("${CASE}" STREQUAL "similar-name")
  # A definition that merely contains the token as part of a larger identifier is unrelated.
  set(defines MY_OD_LOG_LEVEL=1)
  od_select_log_profile(info defines)
  expect_list(defines MY_OD_LOG_LEVEL=1 OD_LOG_LEVEL=OD_LOG_INFO)

else()
  message(FATAL_ERROR "unknown fixture case '${CASE}'")
endif()
