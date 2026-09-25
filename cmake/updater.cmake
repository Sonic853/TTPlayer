include(FetchContent)
FetchContent_Declare(ttplayer_update_zlib
  URL https://zlib.net/fossils/zlib-1.3.2.tar.gz
  URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR ttplayer-inflate-only)
FetchContent_MakeAvailable(ttplayer_update_zlib)
# Only the checked ZIP inflater/CRC are linked; no gzip I/O or shared DLL.
set(_z "${ttplayer_update_zlib_SOURCE_DIR}")
add_library(ttplayer_update_inflate STATIC
  "${_z}/adler32.c" "${_z}/crc32.c" "${_z}/inflate.c" "${_z}/inffast.c"
  "${_z}/inftrees.c" "${_z}/zutil.c")
target_include_directories(ttplayer_update_inflate PUBLIC "${_z}")
target_compile_options(ttplayer_update_inflate PRIVATE /O1 /utf-8)

set(TTPLAYER_UPDATE_ACCESS_FILE "" CACHE FILEPATH "Generated optional updater read-only credential header")
file(MAKE_DIRECTORY "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer")
if(TTPLAYER_UPDATE_ACCESS_FILE)
  configure_file("${TTPLAYER_UPDATE_ACCESS_FILE}" "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/update_build_config.h" COPYONLY)
else()
  file(WRITE "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/update_build_config.h"
    "#pragma once\nnamespace ttplayer::update { inline constexpr char kUpdaterGiteeToken[] = \"\"; }\n")
endif()
add_library(ttplayer_update STATIC src/update/update.cpp src/update/update_http.cpp src/update/update_package.cpp)
target_include_directories(ttplayer_update PUBLIC include PRIVATE "${TTPLAYER_GENERATED_INCLUDE_DIR}")
target_compile_definitions(ttplayer_update PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_update PRIVATE /W4 /permissive- /EHsc /utf-8 /O1)
target_link_libraries(ttplayer_update PUBLIC ttplayer_update_inflate winhttp shlwapi version advapi32)
target_link_libraries(ttplayer_core PUBLIC ttplayer_update)
target_sources(ttplayer_core PRIVATE src/ui/player_window_update.cpp src/ui/update_notice.cpp)

add_custom_target(ttplayer_updater_version
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.ps1"
    -Template "${CMAKE_CURRENT_SOURCE_DIR}/src/updater/version.rc.in"
    -OutputPath "${TTPLAYER_GENERATED_INCLUDE_DIR}/updater_version.rc" ${_player_version_args}
  BYPRODUCTS "${TTPLAYER_GENERATED_INCLUDE_DIR}/updater_version.rc" VERBATIM)
add_executable(ttplayer_updater WIN32 src/updater/main.cpp src/updater/updater.rc src/app/ttplayer.manifest
  "${TTPLAYER_GENERATED_INCLUDE_DIR}/updater_version.rc")
set_target_properties(ttplayer_updater PROPERTIES OUTPUT_NAME TTPUpdater PDB_NAME TTPUpdater)
target_compile_definitions(ttplayer_updater PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_updater PRIVATE /W4 /permissive- /EHsc /utf-8 /O1)
target_link_libraries(ttplayer_updater PRIVATE ttplayer_core ttplayer_update comctl32 shell32 psapi)
add_dependencies(ttplayer_updater ttplayer_updater_version)
add_dependencies(ttplayer_rebuild ttplayer_updater)
if(TTPLAYER_XP_RUNTIME_CONFIGURED)
  add_custom_command(TARGET ttplayer_updater POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
      "$<TARGET_FILE:ttplayer_updater>"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
      --report "$<TARGET_FILE_DIR:ttplayer_updater>/updater-imports.json" VERBATIM)
endif()

option(TTPLAYER_BUILD_UPDATE_TESTS "Build local unpublished updater regressions" OFF)
if(TTPLAYER_BUILD_UPDATE_TESTS)
  add_executable(update_tests tests/update/update_tests.cpp)
  target_compile_definitions(update_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(update_tests PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(update_tests PRIVATE ttplayer_core ttplayer_update)
  add_executable(update_ui_tests tests/update/update_ui_tests.cpp src/app/ttplayer.manifest)
  target_compile_definitions(update_ui_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(update_ui_tests PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(update_ui_tests PRIVATE ttplayer_core)
  add_library(update_fixture_provider SHARED tests/update/fixture_provider.cpp tests/update/fixture_provider.def)
  target_include_directories(update_fixture_provider PRIVATE include)
  target_compile_options(update_fixture_provider PRIVATE /utf-8 /EHsc)
  add_executable(update_fixture_player WIN32 tests/update/fixture_player.cpp "${TTPLAYER_GENERATED_INCLUDE_DIR}/version.rc")
  target_compile_options(update_fixture_player PRIVATE /utf-8 /EHsc)
  add_dependencies(update_fixture_player ttplayer_file_version)
  add_executable(update_app_tests tests/update/update_app_tests.cpp src/app/ttplayer.manifest)
  target_compile_definitions(update_app_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(update_app_tests PRIVATE /utf-8 /EHsc)
  target_link_libraries(update_app_tests PRIVATE ttplayer_update ttplayer_core psapi)
endif()
