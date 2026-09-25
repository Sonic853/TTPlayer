# All player distributions share the XP-compatible Release runtime.
if(NOT MSVC)
  message(FATAL_ERROR "The universal player requires MSVC and the Win32 generator")
endif()
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Universal distribution configuration" FORCE)
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Universal distribution configuration" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/xp_runtime.cmake")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses")
configure_file("${ttplayer_yy_thunks_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses/YY-Thunks-LICENSE.txt" COPYONLY)
