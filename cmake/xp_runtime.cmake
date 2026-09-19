# Directory-scoped XP runtime policy shared by the legacy player and the
# optional i18n DLL. Including it in gettext must not change the modern EXE.
if(TTPLAYER_XP_RUNTIME_CONFIGURED)
  return()
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The shared XP runtime requires MSVC and the Win32 generator")
endif()
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

# _WIN32_WINNT alone cannot change imports in MSVC's precompiled C++ library.
set(VC_LTL_Root "${ttplayer_vc_ltl_SOURCE_DIR}")
set(WindowsTargetPlatformMinVersion "5.1.2600.0")
set(SupportLTL "true")
include("${VC_LTL_Root}/config/config.cmake")
if(NOT InternalLTLCRTVersion STREQUAL "5.1.2600.0")
  message(FATAL_ERROR "The shared DLL and legacy player must use VC-LTL's XP runtime")
endif()
# The thunk object must precede Windows import libraries.
add_link_options("${ttplayer_yy_thunks_SOURCE_DIR}/objs/x86/YY_Thunks_for_WinXP.obj")
add_link_options(/OSVERSION:5.1
  "$<IF:$<BOOL:$<TARGET_PROPERTY:WIN32_EXECUTABLE>>,/SUBSYSTEM:WINDOWS$<COMMA>5.01,/SUBSYSTEM:CONSOLE$<COMMA>5.01>")
set(TTPLAYER_XP_RUNTIME_CONFIGURED TRUE)
