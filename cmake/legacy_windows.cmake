# A distinct Release build tree is required: current debug CRTs cannot run on XP.
if(NOT MSVC)
  message(FATAL_ERROR "TTPLAYER_LEGACY_WINDOWS requires MSVC and the Win32 generator")
endif()
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Legacy distribution configuration" FORCE)
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Legacy distribution configuration" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

include(FetchContent)
FetchContent_Declare(ttplayer_yy_thunks
  URL https://github.com/Chuyu-Team/YY-Thunks/releases/download/v1.2.2/YY-Thunks-Objs.zip
  URL_HASH SHA256=518ed7ef4825e8a41997fbccfa2c8090cf31a6038fd51520a2e49886f947f9fc
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(ttplayer_vc_ltl
  URL https://github.com/Chuyu-Team/VC-LTL5/releases/download/v5.3.1/VC-LTL-Binary.7z
  URL_HASH SHA256=7a18799ed3aa84a225610a5447a56bc534c5c98ccb8dec05caba0e3f633431ad
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ttplayer_yy_thunks ttplayer_vc_ltl)

# Use the XP CRT adapter and link the thunk OBJECT before Windows import libs.
# _WIN32_WINNT alone cannot change imports in MSVC's precompiled C++ library.
set(VC_LTL_Root "${ttplayer_vc_ltl_SOURCE_DIR}")
set(WindowsTargetPlatformMinVersion "5.1.2600.0")
set(SupportLTL "true")
include("${VC_LTL_Root}/config/config.cmake")
if(NOT InternalLTLCRTVersion STREQUAL "5.1.2600.0")
  message(FATAL_ERROR "The legacy build must use VC-LTL's XP runtime")
endif()
add_link_options("${ttplayer_yy_thunks_SOURCE_DIR}/objs/x86/YY_Thunks_for_WinXP.obj")
add_compile_definitions(TTPLAYER_LEGACY_WINDOWS=1)
# Each executable, including test and worker front ends, must have an XP header.
add_link_options(/OSVERSION:5.1
  "$<IF:$<BOOL:$<TARGET_PROPERTY:WIN32_EXECUTABLE>>,/SUBSYSTEM:WINDOWS$<COMMA>5.01,/SUBSYSTEM:CONSOLE$<COMMA>5.01>")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses")
configure_file("${ttplayer_yy_thunks_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_BINARY_DIR}/legacy-licenses/YY-Thunks-LICENSE.txt" COPYONLY)
