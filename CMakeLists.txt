cmake_minimum_required(VERSION 3.24)
project(TTPlayerRebuild VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTING "Build local, unpublished tests" OFF)
include(CTest)
option(TTPLAYER_STAGE_RUNTIME "Rebuild/copy local original DLLs, skins and configuration beside the EXE" ON)
option(TTPLAYER_LEGACY_WINDOWS "Build the separate Windows XP SP3 / Windows 7 edition" OFF)
option(TTPLAYER_OPTIMIZE_SIZE "Optimize Release player size, retaining fast audio processing" ON)
option(TTPLAYER_BUILD_LIBRARY_TESTS "Build local media library regression tests" OFF)
option(TTPLAYER_BUILD_NAVIGATION_TESTS "Build local playback navigation regression tests" OFF)
option(TTPLAYER_BUILD_INFO_TESTS "Build local playlist information regression tests" OFF)
option(TTPLAYER_BUILD_WHEEL_TESTS "Build local playlist mouse wheel tests" OFF)
option(TTPLAYER_BUILD_PREVIEW_TESTS "Build local taskbar album preview tests" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "TTPlayerRebuild currently targets Windows")
endif()

# The recovered resource/communication DLLs and legacy add-in ABI are 32-bit.
# Refuse a generator that would create a host process unable to load them.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "TTPlayerRebuild requires a Win32/x86 generator (configure with -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(TTPLAYER_LEGACY_WINDOWS)
  include(cmake/legacy_windows.cmake)
endif()
include(cmake/wtl.cmake)

# Re-evaluate build metadata at build time, not just when CMake configures.
# Generators rewrite their outputs only when the date or version changes.
find_program(TTPLAYER_POWERSHELL NAMES powershell pwsh REQUIRED)
set(TTPLAYER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TTPLAYER_BUILD_VERSION "" CACHE STRING "EXE/package yyyy.MM.dd[pN] version; empty uses the current Beijing date")
set(_player_version_args)
if(NOT TTPLAYER_BUILD_VERSION STREQUAL "")
  list(APPEND _player_version_args -Version "${TTPLAYER_BUILD_VERSION}")
endif()
add_custom_target(ttplayer_file_version
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.ps1"
          -Template "${CMAKE_CURRENT_SOURCE_DIR}/src/app/version.rc.in"
          -OutputPath "${TTPLAYER_GENERATED_INCLUDE_DIR}/version.rc"
          ${_player_version_args}
  BYPRODUCTS "${TTPLAYER_GENERATED_INCLUDE_DIR}/version.rc"
  COMMENT "Updating EXE author, description and version"
  VERBATIM
)
add_custom_target(ttplayer_build_date
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_build_date.ps1"
          -OutputPath "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/build_date.h"
  BYPRODUCTS "${TTPLAYER_GENERATED_INCLUDE_DIR}/ttplayer/build_date.h"
  COMMENT "Updating Beijing build date"
  VERBATIM
)

add_library(ttplayer_core
  src/i18n/i18n.cpp
  src/ui/wtl_runtime.cpp
  src/ui/wtl_dialogs.cpp
  src/ui/wtl_menu.cpp
  src/app/dsp_worker.cpp
  src/app/file_info_worker.cpp
  src/app/output_device_worker.cpp
  src/audio/archive_member.cpp
  src/audio/asio_sink.cpp
  src/audio/audio_engine.cpp
  src/audio/builtin_file_info.cpp
  src/audio/cue_sheet.cpp
  src/audio/directshow_source.cpp
  src/audio/file_encoder.cpp
  src/audio/extension_correction.cpp
  src/audio/format_probe.cpp
  src/audio/kernel_streaming_sink.cpp
  src/audio/legacy_windows_source.cpp
  src/audio/midi_player.cpp
  src/audio/mp3pro_source.cpp
  src/audio/native_output_contract.cpp
  src/audio/wasapi_sink.cpp
  src/audio/pcm_output_transform.cpp
  src/audio/replay_gain_scanner.cpp
  src/audio/winamp_dsp.cpp
  src/core/text.cpp
  src/integrations/discord_presence.cpp
  src/integrations/discord_track_presence.cpp
  src/lyrics/lrc_parser.cpp
  src/lyrics/association.cpp
  src/lyrics/local_search.cpp
  src/lyrics/online_search.cpp
  src/lyrics/service_catalog.cpp
  src/lyrics/lyric_http.cpp
  src/lyrics/https_provider.cpp
  src/playlist/playlist.cpp
  src/playlist/playback_order.cpp
  src/playlist/playlist_store.cpp
  src/platform/optional_windows_api.cpp
  src/plugins/plugin_manager.cpp
  src/settings/file_association.cpp
  src/settings/settings.cpp
  src/settings/runtime_settings.cpp
  src/skin/default_colors.cpp
  src/skin/skin.cpp
  src/skin/skin_image.cpp
  src/skin/skin_package.cpp
  src/skin/skin_plugin.cpp
  src/ui/desktop_lyrics.cpp
  src/ui/directory_change_monitor.cpp
  src/ui/file_info_probe_client.cpp
  src/ui/player_window.cpp
  src/ui/player_window_layout.cpp
  src/ui/player_window_skin_plugin.cpp
  src/ui/player_window_skin_animation.cpp
  src/ui/player_window_taskbar.cpp
  src/ui/taskbar_playback.cpp
  src/ui/taskbar_preview.cpp
  src/ui/player_window_equalizer.cpp
  src/ui/player_window_file_intake.cpp
  src/ui/player_window_extension_correction.cpp
  src/ui/output_devices.cpp
  src/ui/modern_file_dialog.cpp
  src/ui/player_control_packet.cpp
  src/ui/player_window_lyrics.cpp
  src/ui/lyric_association_dialog.cpp
  src/ui/lyric_upload_window.cpp
    src/ui/player_window_lyric_search.cpp
    src/ui/player_window_lyric_services.cpp
  src/ui/player_window_lyric_input.cpp
  src/ui/player_window_library.cpp
  src/ui/player_window_options.cpp
  src/ui/player_window_playlist.cpp
  src/ui/player_window_playlist_native.cpp
  src/ui/player_window_playlist_info.cpp
  src/ui/player_window_playlist_network.cpp
  src/ui/player_window_playlist_properties.cpp
  src/ui/player_window_playlist_search.cpp
  src/ui/playlist_network_commands.cpp
  src/ui/playlist_transforms.cpp
  src/ui/player_window_visual.cpp
  src/ui/album_background.cpp
  src/ui/cover_image.cpp
  src/ui/player_window_visual_options.cpp
  src/ui/shell_send_to.cpp
  src/ui/window_drag.cpp
)
target_include_directories(ttplayer_core
  PUBLIC include
  PRIVATE "${TTPLAYER_GENERATED_INCLUDE_DIR}"
)
add_dependencies(ttplayer_core ttplayer_build_date)
target_compile_definitions(ttplayer_core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_core PUBLIC
  ttplayer_wtl_headers
  winmm ole32 oleaut32 comsuppw uuid strmiids msimg32 shell32 shlwapi comctl32 comdlg32
  windowscodecs gdiplus dsound setupapi dwmapi uxtheme
  mfplat mfreadwrite mfuuid propsys winhttp
)

if(NOT TTPLAYER_LEGACY_WINDOWS)
  target_sources(ttplayer_core PRIVATE src/ui/system_media_controls.cpp)
  target_link_libraries(ttplayer_core PUBLIC runtimeobject shcore)
  if(BUILD_TESTING)
    add_executable(system_media_controls_tests tests/ui/system_media_controls_tests.cpp)
    target_compile_definitions(system_media_controls_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(system_media_controls_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(system_media_controls_tests PRIVATE ttplayer_core)
    add_test(NAME system_media_controls_tests COMMAND system_media_controls_tests)
    set_tests_properties(system_media_controls_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
endif()

add_library(ttpcomm_api STATIC src/app/ttpcomm_api.c)
target_include_directories(ttpcomm_api PUBLIC include)
target_compile_definitions(ttpcomm_api PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(ttplayer_rebuild WIN32
  src/app/application.cpp
  src/app/exports.cpp
  src/app/main.cpp
  src/app/resources.rc
  src/app/runtime.cpp
  src/app/single_instance.cpp
  src/app/ttplayer.manifest
  "${TTPLAYER_GENERATED_INCLUDE_DIR}/version.rc"
)
add_dependencies(ttplayer_rebuild ttplayer_file_version)
set_target_properties(ttplayer_rebuild PROPERTIES
  OUTPUT_NAME "TTPlayerRebuild" PDB_NAME "TTPlayerRebuild")
target_compile_definitions(ttplayer_rebuild PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_rebuild PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_rebuild PRIVATE ttplayer_core ttpcomm_api comctl32 comdlg32 shell32)

# The TLS DLL is built independently. This option only stages a prebuilt DLL.
set(TTPLAYER_HTTPS_DLL "" CACHE FILEPATH "Prebuilt ttp_https.dll to stage in the player's AddIn directory")
if(TTPLAYER_HTTPS_DLL)
  if(NOT EXISTS "${TTPLAYER_HTTPS_DLL}" OR IS_DIRECTORY "${TTPLAYER_HTTPS_DLL}")
    message(FATAL_ERROR "TTPLAYER_HTTPS_DLL must name an independently built ttp_https.dll")
  endif()
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TTPLAYER_HTTPS_DLL}"
      "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn/ttp_https.dll" VERBATIM)
endif()

# The player and translation provider are built and distributed independently.
# An already-built DLL is only needed for optional integration tests.
set(TTPLAYER_I18N_TEST_DLL "" CACHE FILEPATH "Prebuilt ttp_i18n.dll for local integration tests")
if(BUILD_TESTING AND TTPLAYER_I18N_TEST_DLL)
  if(NOT EXISTS "${TTPLAYER_I18N_TEST_DLL}" OR IS_DIRECTORY "${TTPLAYER_I18N_TEST_DLL}")
    message(FATAL_ERROR "TTPLAYER_I18N_TEST_DLL must point to an independently built ttp_i18n.dll")
  endif()
  add_executable(i18n_ui_tests tests/ui/i18n_tests.cpp tests/ui/i18n_test_resources.rc src/app/resources.rc)
  target_compile_definitions(i18n_ui_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(i18n_ui_tests PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(i18n_ui_tests PRIVATE ttplayer_core)
  foreach(language IN ITEMS chs cht)
    add_library(i18n_resource_mock_${language} SHARED tests/ui/i18n_mock_resources.rc)
    set_target_properties(i18n_resource_mock_${language} PROPERTIES LINKER_LANGUAGE CXX)
    if(TTPLAYER_LEGACY_WINDOWS)
      # Resource-only fixtures have no CRT entry point. Keep their XP header,
      # but do not pull executable thunk code into a /NOENTRY resource DLL.
      get_target_property(resource_link_options i18n_resource_mock_${language} LINK_OPTIONS)
      list(REMOVE_ITEM resource_link_options "${ttplayer_yy_thunks_SOURCE_DIR}/objs/x86/YY_Thunks_for_WinXP.obj")
      set_property(TARGET i18n_resource_mock_${language} PROPERTY LINK_OPTIONS ${resource_link_options})
    endif()
    target_link_options(i18n_resource_mock_${language} PRIVATE /NOENTRY)
    if(language STREQUAL "cht")
      target_compile_definitions(i18n_resource_mock_${language} PRIVATE I18N_TEST_TRADITIONAL)
    endif()
    add_dependencies(i18n_ui_tests i18n_resource_mock_${language})
  endforeach()
  add_test(NAME i18n_ui_tests COMMAND i18n_ui_tests "${TTPLAYER_I18N_TEST_DLL}"
    "$<TARGET_FILE:i18n_resource_mock_chs>" "$<TARGET_FILE:i18n_resource_mock_cht>")
  if(NOT TTPLAYER_LEGACY_WINDOWS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_dependencies(i18n_ui_tests ttplayer_rebuild ttpcomm_mock_no_version)
    add_test(NAME i18n_startup_tests COMMAND "${Python3_EXECUTABLE}" -X utf8
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/i18n_startup_tests.py"
      "$<TARGET_FILE:ttplayer_rebuild>" "${TTPLAYER_I18N_TEST_DLL}"
      "$<TARGET_FILE:ttpcomm_mock_no_version>")
    set_tests_properties(i18n_startup_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  endif()
endif()

include(cmake/size_optimization.cmake)

# Pre-ATL-8 AddIns generate window thunks in ordinary heap memory
# (ttp_ape!60101C36, also ttp_enc). /NXCOMPAT opts out of Windows' ATL
# emulation, faulting before WM_INITDIALOG finishes. wWinMain instead
# enables DEP explicitly, with the system's legacy ATL compatibility.
target_link_options(ttplayer_rebuild PRIVATE /NXCOMPAT:NO)
if(TTPLAYER_LEGACY_WINDOWS)
  include(cmake/legacy_ttpcomm.cmake)
  target_link_options(ttplayer_rebuild PRIVATE /SUBSYSTEM:WINDOWS,5.01 /OSVERSION:5.1)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
      "$<TARGET_FILE:ttplayer_rebuild>"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
      --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
      --require-ttpcomm-tls
      --report "$<TARGET_FILE_DIR:ttplayer_rebuild>/legacy-imports.json"
    COMMENT "Checking every static import against XP and Windows 7"
    VERBATIM)
  set_property(TARGET ttplayer_rebuild APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(TTPLAYER_LAME_ARCH x64)
else()
  set(TTPLAYER_LAME_ARCH x86)
endif()
function(ttplayer_stage_lame target)
  set(lame_archive "${CMAKE_CURRENT_SOURCE_DIR}/../lame-4.0-${TTPLAYER_LAME_ARCH}.zip")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_lame.cmake")
  if(EXISTS "${lame_archive}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${lame_archive}")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DARCHIVE=${CMAKE_CURRENT_SOURCE_DIR}/../lame-4.0-${TTPLAYER_LAME_ARCH}.zip"
      "-DSTAGING=${CMAKE_CURRENT_BINARY_DIR}/lame/${TTPLAYER_LAME_ARCH}"
      "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_lame.cmake"
    VERBATIM)
endfunction()
if(TTPLAYER_STAGE_RUNTIME)
  ttplayer_stage_lame(ttplayer_rebuild)
endif()

# Standalone development/test front ends only. The shipped player embeds all
# three workers and relaunches itself in private modes for fault isolation;
# building/distributing ttplayer_rebuild does not require any helper EXE.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio/extension_correction_tests.cpp")
  add_executable(extension_correction_tests tests/audio/extension_correction_tests.cpp)
  target_compile_definitions(extension_correction_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(extension_correction_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(extension_correction_tests PRIVATE ttplayer_core)
  add_test(NAME extension_correction_tests COMMAND extension_correction_tests)
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio/extension_mismatch_tests.cpp")
  add_executable(extension_mismatch_tests tests/audio/extension_mismatch_tests.cpp src/app/exports.cpp)
  target_compile_definitions(extension_mismatch_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(extension_mismatch_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(extension_mismatch_tests PRIVATE ttplayer_core)
  if(TTPLAYER_LEGACY_WINDOWS)
    add_dependencies(extension_mismatch_tests ttplayer_legacy_ttpcomm_import)
    target_link_libraries(extension_mismatch_tests PRIVATE "${ttpcomm_import_library}")
    target_link_options(extension_mismatch_tests PRIVATE /INCLUDE:__imp__srand48)
  endif()
  add_test(NAME extension_mismatch_tests COMMAND extension_mismatch_tests --self)
  set_tests_properties(extension_mismatch_tests PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_PREVIEW_TESTS)
enable_testing()
add_executable(taskbar_preview_tests tests/ui/taskbar_preview_tests.cpp)
target_compile_definitions(taskbar_preview_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(taskbar_preview_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(taskbar_preview_tests PRIVATE ttplayer_core)
add_test(NAME taskbar_preview_tests COMMAND taskbar_preview_tests)
set_tests_properties(taskbar_preview_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_WHEEL_TESTS)
enable_testing()
add_executable(playlist_wheel_tests tests/ui/playlist_wheel_tests.cpp)
target_compile_definitions(playlist_wheel_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_wheel_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_wheel_tests PRIVATE ttplayer_core)
add_test(NAME playlist_wheel_tests COMMAND playlist_wheel_tests)
set_tests_properties(playlist_wheel_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_INFO_TESTS)
enable_testing()
add_executable(playlist_info_tests tests/ui/playlist_info_tests.cpp)
target_compile_definitions(playlist_info_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_info_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_info_tests PRIVATE ttplayer_core)
add_test(NAME playlist_info_tests COMMAND playlist_info_tests)
set_tests_properties(playlist_info_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_NAVIGATION_TESTS)
enable_testing()
add_executable(playback_navigation_tests tests/ui/playback_navigation_tests.cpp)
target_compile_definitions(playback_navigation_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playback_navigation_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playback_navigation_tests PRIVATE ttplayer_core)
add_test(NAME playback_navigation_tests COMMAND playback_navigation_tests)
set_tests_properties(playback_navigation_tests PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING OR TTPLAYER_BUILD_LIBRARY_TESTS)
enable_testing()
add_executable(media_library_tests tests/ui/media_library_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(media_library_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(media_library_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(media_library_tests PRIVATE ttplayer_core)
add_test(NAME media_library_tests COMMAND media_library_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(media_library_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING)
add_executable(track_change_fade_tests tests/ui/track_change_fade_tests.cpp)
target_compile_definitions(track_change_fade_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(track_change_fade_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(track_change_fade_tests PRIVATE ttplayer_core)
add_dependencies(track_change_fade_tests dsp_probe_valid)
add_test(NAME track_change_fade_tests COMMAND track_change_fade_tests)
set_tests_properties(track_change_fade_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_executable(midi_playback_tests tests/audio/midi_playback_tests.cpp)
target_compile_definitions(midi_playback_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(midi_playback_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(midi_playback_tests PRIVATE ttplayer_core)
add_test(NAME midi_playback_tests COMMAND midi_playback_tests)
set_tests_properties(midi_playback_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_executable(cover_format_tests tests/ui/cover_format_tests.cpp src/app/exports.cpp)
target_compile_definitions(cover_format_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(cover_format_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(cover_format_tests PRIVATE ttplayer_core)
add_test(NAME cover_format_tests COMMAND cover_format_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/album_cover" "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(cover_format_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
if(TTPLAYER_LEGACY_WINDOWS)
  add_dependencies(cover_format_tests ttplayer_legacy_ttpcomm_import)
  target_link_libraries(cover_format_tests PRIVATE "${ttpcomm_import_library}")
  target_link_options(cover_format_tests PRIVATE /INCLUDE:__imp__srand48)
endif()
add_executable(cover_image_tests tests/ui/cover_image_tests.cpp)
target_compile_definitions(cover_image_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(cover_image_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(cover_image_tests PRIVATE ttplayer_core)
if(TTPLAYER_LEGACY_WINDOWS)
  add_dependencies(cover_image_tests ttplayer_legacy_ttpcomm_import)
  target_link_libraries(cover_image_tests PRIVATE "${ttpcomm_import_library}")
  target_link_options(cover_image_tests PRIVATE /INCLUDE:__imp__srand48)
endif()
add_test(NAME cover_image_tests COMMAND cover_image_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/album_cover" "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(cover_image_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(idle_error_tests tests/ui/idle_error_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(idle_error_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(idle_error_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(idle_error_tests PRIVATE ttplayer_core)
add_test(NAME idle_error_tests COMMAND idle_error_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(idle_error_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(default_colors_tests tests/skin/default_colors_tests.cpp)
target_compile_definitions(default_colors_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(default_colors_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(default_colors_tests PRIVATE ttplayer_core)
add_test(NAME default_colors_tests COMMAND default_colors_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(default_colors_tests PROPERTIES TIMEOUT 100 RUN_SERIAL TRUE)

add_executable(legacy_windows_tests tests/platform/legacy_windows_tests.cpp
  src/platform/optional_windows_api.cpp)
target_compile_definitions(legacy_windows_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN TTPLAYER_TEST_NO_OPTIONAL_WINDOWS_API)
target_compile_options(legacy_windows_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(legacy_windows_tests PRIVATE ttplayer_core)
add_test(NAME legacy_windows_tests COMMAND legacy_windows_tests)
set_tests_properties(legacy_windows_tests PROPERTIES TIMEOUT 45)

add_executable(runtime_paths_tests tests/ui/runtime_paths_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(runtime_paths_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(runtime_paths_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(runtime_paths_tests PRIVATE ttplayer_core)
add_dependencies(runtime_paths_tests ttplayer_rebuild)
add_test(NAME runtime_paths_tests COMMAND runtime_paths_tests "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "$<TARGET_FILE:ttplayer_rebuild>")
set_tests_properties(runtime_paths_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
add_test(NAME stage_settings_tests COMMAND "${CMAKE_COMMAND}"
  "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/Testing"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_stage_settings.cmake")

add_executable(fullscreen_lyric_drag_tests tests/ui/fullscreen_lyric_drag_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_lyric_drag_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_lyric_drag_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_lyric_drag_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_lyric_drag_tests COMMAND fullscreen_lyric_drag_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_lyric_drag_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(fullscreen_lyric_render_tests tests/ui/fullscreen_lyric_render_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_lyric_render_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_lyric_render_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_lyric_render_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_lyric_render_tests COMMAND fullscreen_lyric_render_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_lyric_render_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(fullscreen_album_tests tests/ui/fullscreen_album_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(fullscreen_album_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fullscreen_album_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(fullscreen_album_tests PRIVATE ttplayer_core)
add_test(NAME fullscreen_album_tests COMMAND fullscreen_album_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(fullscreen_album_tests PROPERTIES TIMEOUT 45)

add_executable(desktop_lyrics_menu_tests tests/ui/desktop_lyrics_menu_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(desktop_lyrics_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(desktop_lyrics_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(desktop_lyrics_menu_tests PRIVATE ttplayer_core)
add_test(NAME desktop_lyrics_menu_tests COMMAND desktop_lyrics_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(desktop_lyrics_menu_tests PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

add_executable(options_drawing_tests tests/ui/options_drawing_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(options_drawing_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(options_drawing_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(options_drawing_tests PRIVATE ttplayer_core)
add_test(NAME options_drawing_tests COMMAND options_drawing_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(options_drawing_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME options_general_tabs_tests COMMAND options_drawing_tests "${CMAKE_CURRENT_SOURCE_DIR}/.." --general-tabs)
set_tests_properties(options_general_tabs_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_search_tests tests/ui/lyric_search_tests.cpp src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_search_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_search_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_search_tests PRIVATE ttplayer_core ws2_32)
add_test(NAME lyric_search_tests COMMAND lyric_search_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_search_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_menu_tests tests/ui/lyric_menu_tests.cpp src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_menu_tests PRIVATE ttplayer_core oleacc)
add_test(NAME lyric_menu_tests COMMAND lyric_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_menu_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

add_executable(lyric_association_tests tests/ui/lyric_association_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_association_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_association_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_association_tests PRIVATE ttplayer_core)
add_test(NAME lyric_association_tests COMMAND lyric_association_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_association_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(lyric_service_catalog_tests tests/lyrics/service_catalog_tests.cpp)
target_compile_definitions(lyric_service_catalog_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_service_catalog_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_service_catalog_tests PRIVATE ttplayer_core)
add_test(NAME lyric_service_catalog_tests COMMAND lyric_service_catalog_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_service_catalog_tests PROPERTIES TIMEOUT 60)


add_executable(window_topmost_tests tests/ui/window_topmost_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(window_topmost_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(window_topmost_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(window_topmost_tests PRIVATE ttplayer_core)
add_test(NAME window_topmost_tests COMMAND window_topmost_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(window_topmost_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

add_executable(skin_profile_tests tests/skin/skin_profile_tests.cpp)
target_compile_definitions(skin_profile_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_profile_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_profile_tests PRIVATE ttplayer_core)
add_test(NAME skin_profile_tests COMMAND skin_profile_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(skin_profile_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

add_executable(skin_alignment_tests tests/skin/skin_alignment_tests.cpp)
target_compile_definitions(skin_alignment_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_alignment_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_alignment_tests PRIVATE ttplayer_core)
add_test(NAME skin_alignment_tests COMMAND skin_alignment_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(project_links_tests tests/ui/project_links_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(project_links_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(project_links_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(project_links_tests PRIVATE ttplayer_core)
add_test(NAME project_links_tests COMMAND project_links_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(shared_menu_tests tests/ui/shared_menu_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(shared_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(shared_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(shared_menu_tests PRIVATE ttplayer_core)
add_test(NAME shared_menu_tests COMMAND shared_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(shared_menu_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
add_executable(rearrange_window_tests tests/ui/rearrange_window_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(rearrange_window_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rearrange_window_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(rearrange_window_tests PRIVATE ttplayer_core)
add_test(NAME rearrange_window_tests COMMAND rearrange_window_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(rearrange_window_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_executable(lyric_main_menu_tests tests/ui/lyric_main_menu_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(lyric_main_menu_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lyric_main_menu_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(lyric_main_menu_tests PRIVATE ttplayer_core)
add_test(NAME lyric_main_menu_tests COMMAND lyric_main_menu_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(lyric_main_menu_tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(wtl_behavior_tests tests/ui/wtl_behavior_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(wtl_behavior_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wtl_behavior_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wtl_behavior_tests PRIVATE ttplayer_core ttpcomm_api)
add_test(NAME wtl_behavior_tests COMMAND wtl_behavior_tests)
set_tests_properties(wtl_behavior_tests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(wtl_migration_tests tests/ui/wtl_migration_tests.cpp)
target_compile_definitions(wtl_migration_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(wtl_migration_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(wtl_migration_tests PRIVATE ttplayer_core)
add_test(NAME wtl_migration_tests COMMAND wtl_migration_tests)
set_tests_properties(wtl_migration_tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_executable(skin_png_tests tests/skin/skin_png_tests.cpp)
target_compile_definitions(skin_png_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_png_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_png_tests PRIVATE ttplayer_core)
add_test(NAME skin_png_tests COMMAND skin_png_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(ttplayer_dsp_probe EXCLUDE_FROM_ALL
  tools/dsp_plugin_probe.cpp src/app/dsp_worker.cpp)
target_include_directories(ttplayer_dsp_probe PRIVATE include)
target_compile_definitions(ttplayer_dsp_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_dsp_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
set_property(TARGET ttplayer_dsp_probe PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

# Legacy output discovery enters SetupAPI, KS miniports and DirectSound driver
# code.  Keep the original catalogue/order, but run that blocking boundary in
# a same-bitness helper which the modeless Options window can time out safely.
add_executable(ttplayer_output_device_probe EXCLUDE_FROM_ALL
  tools/output_device_probe.cpp
  src/core/text.cpp
  src/i18n/i18n.cpp
  src/app/output_device_worker.cpp
  src/audio/asio_sink.cpp
  src/audio/native_output_contract.cpp
  src/audio/wasapi_sink.cpp
  src/ui/output_devices.cpp
)
target_include_directories(ttplayer_output_device_probe PRIVATE src/ui include)
target_compile_definitions(ttplayer_output_device_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_output_device_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_output_device_probe PRIVATE
  winmm dsound setupapi ole32)
set_property(TARGET ttplayer_output_device_probe PROPERTY
  MSVC_RUNTIME_LIBRARY MultiThreaded)

# CFileInfoDlg enters third-party reader/tag-writer vtables on worker threads.
# Keep that recovered ABI, but isolate it so Close/cancel can enforce a hard
# bound even when an old add-in never returns from Open or metadata slot 6.
add_executable(ttplayer_file_info_probe EXCLUDE_FROM_ALL tools/file_info_probe.cpp)
target_include_directories(ttplayer_file_info_probe PRIVATE src/ui)
target_compile_definitions(ttplayer_file_info_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttplayer_file_info_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttplayer_file_info_probe PRIVATE ttplayer_core)

foreach(dsp_fixture IN ITEMS valid old stall fault pacemaker configure)
  add_library(dsp_probe_${dsp_fixture} SHARED tests/mock_winamp_dsp.cpp)
  target_compile_definitions(dsp_probe_${dsp_fixture} PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(dsp_probe_${dsp_fixture} PRIVATE
    /W4 /permissive- /EHsc /utf-8)
  set_property(TARGET dsp_probe_${dsp_fixture} PROPERTY
    MSVC_RUNTIME_LIBRARY MultiThreaded)
  set_target_properties(dsp_probe_${dsp_fixture} PROPERTIES
    PREFIX "" OUTPUT_NAME "dsp_${dsp_fixture}")
endforeach()
target_compile_definitions(dsp_probe_old PRIVATE TTPLAYER_DSP_OLD_HEADER)
target_compile_definitions(dsp_probe_stall PRIVATE TTPLAYER_DSP_STALL_HEADER)
target_compile_definitions(dsp_probe_fault PRIVATE TTPLAYER_DSP_FAULT_MODIFY)
target_compile_definitions(dsp_probe_pacemaker PRIVATE
  TTPLAYER_DSP_PACEMAKER_HEADER)
target_compile_definitions(dsp_probe_configure PRIVATE TTPLAYER_DSP_CONFIGURE_HEADER)

add_executable(portable_worker_tests tests/portable_worker_tests.cpp)
target_include_directories(portable_worker_tests PRIVATE src/ui)
target_compile_definitions(portable_worker_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(portable_worker_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(portable_worker_tests PRIVATE ttplayer_core)
add_dependencies(portable_worker_tests ttplayer_rebuild dsp_probe_configure
  ttplayer_output_device_probe)

add_executable(dsp_probe_tests tests/dsp_probe_tests.cpp)
target_compile_definitions(dsp_probe_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dsp_probe_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
set_property(TARGET dsp_probe_tests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
add_dependencies(dsp_probe_tests ttplayer_dsp_probe dsp_probe_valid
  dsp_probe_old dsp_probe_stall)

add_executable(winamp_dsp_runtime_tests tests/winamp_dsp_runtime_tests.cpp)
target_compile_definitions(winamp_dsp_runtime_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(winamp_dsp_runtime_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(winamp_dsp_runtime_tests PRIVATE ttplayer_core)
add_dependencies(winamp_dsp_runtime_tests dsp_probe_valid dsp_probe_old
  dsp_probe_fault dsp_probe_pacemaker)

add_executable(ttplayer_tests tests/tests.cpp)
target_compile_definitions(ttplayer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# This executable is itself a regression oracle.  MSVC's Release defaults add
# NDEBUG, which used to compile all 300+ assert expressions out of the guest
# Sandbox run while still returning exit code zero.  Keep those checks live in
# every configuration; the product targets retain their normal Release flags.
target_compile_options(ttplayer_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(ttplayer_tests PRIVATE ttplayer_core)

add_executable(file_association_tests tests/file_association_tests.cpp)
add_executable(default_programs_tests tests/settings/default_programs_tests.cpp)
target_compile_definitions(default_programs_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(default_programs_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(default_programs_tests PRIVATE ttplayer_core)
add_test(NAME default_programs_tests COMMAND default_programs_tests)
set_tests_properties(default_programs_tests PROPERTIES TIMEOUT 30)
target_compile_definitions(file_association_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_association_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(file_association_tests PRIVATE ttplayer_core)

add_executable(desktop_lyrics_tests tests/desktop_lyrics_tests.cpp
  tests/desktop_lyric_scroll_tests.cpp)
target_compile_definitions(desktop_lyrics_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(desktop_lyrics_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(desktop_lyrics_tests PRIVATE ttplayer_core)

add_executable(shell_send_to_tests tests/shell_send_to_tests.cpp)
target_compile_definitions(shell_send_to_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(shell_send_to_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(shell_send_to_tests PRIVATE ttplayer_core)

add_executable(playlist_network_commands_tests
  tests/playlist_network_commands_tests.cpp)
target_compile_definitions(playlist_network_commands_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_network_commands_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(playlist_network_commands_tests PRIVATE ttplayer_core)

add_executable(file_info_probe_protocol_tests
  tests/file_info_probe_protocol_tests.cpp
  src/ui/file_info_probe_client.cpp)
target_include_directories(file_info_probe_protocol_tests PRIVATE src/ui include)
target_compile_definitions(file_info_probe_protocol_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_info_probe_protocol_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)

add_executable(builtin_file_info_tests
  tests/builtin_file_info_tests.cpp
  src/ui/file_info_probe_client.cpp)
target_include_directories(builtin_file_info_tests PRIVATE src/ui)
target_compile_definitions(builtin_file_info_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(builtin_file_info_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(builtin_file_info_tests PRIVATE ttplayer_core)
add_dependencies(builtin_file_info_tests ttplayer_file_info_probe)

add_executable(playlist_list_contract_tests
  tests/playlist_list_contract_tests.cpp)
target_compile_definitions(playlist_list_contract_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_list_contract_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_include_directories(playlist_list_contract_tests PRIVATE include)

add_executable(playlist_local_search_tests
  tests/playlist_local_search_tests.cpp)
target_compile_definitions(playlist_local_search_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_local_search_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(playlist_local_search_tests PRIVATE ttplayer_core)

# A deliberately deceptive legacy add-in: EnumInterface advertises the reader
# category and its raw object has a description slot, but QueryInterface for
# the reader IID fails. FUN_004CABC0 must not publish that description.
add_library(ttp_mock_incompatible SHARED tests/mock_incompatible_sound_addin.cpp)
target_compile_definitions(ttp_mock_incompatible PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttp_mock_incompatible PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttp_mock_incompatible PRIVATE ole32)
set_property(TARGET ttp_mock_incompatible PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

# A valid reader creator whose Open slot deliberately ignores cancellation for
# seven seconds.  It exercises AudioEngine's bounded UI wait without invoking
# undefined thread termination or depending on a network/device timeout.
add_library(ttp_mock_stalling SHARED tests/mock_stalling_sound_addin.cpp)
target_compile_definitions(ttp_mock_stalling PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttp_mock_stalling PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttp_mock_stalling PRIVATE ole32)
set_property(TARGET ttp_mock_stalling PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)

add_executable(audio_recovery_tests tests/audio_recovery_tests.cpp)
add_executable(conversion_recovery_tests tests/conversion_recovery_tests.cpp
  src/app/exports.cpp)
target_compile_definitions(conversion_recovery_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(conversion_recovery_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(conversion_recovery_tests PRIVATE ttplayer_core)
ttplayer_stage_lame(conversion_recovery_tests)
target_compile_definitions(audio_recovery_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(audio_recovery_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(audio_recovery_tests PRIVATE ttplayer_core)
add_dependencies(audio_recovery_tests ttp_mock_incompatible ttp_mock_stalling)

foreach(revision_target IN ITEMS ttpcomm_mock_revision ttpcomm_mock_no_version)
  add_library(${revision_target} SHARED tests/mock_ttpcomm_revision.c)
  target_compile_options(${revision_target} PRIVATE /W4 /utf-8)
endforeach()
target_compile_definitions(ttpcomm_mock_no_version PRIVATE MOCK_NO_VERSION)

add_executable(ttpcomm_compatibility_tests
  tests/ttpcomm_compatibility_tests.cpp src/app/runtime.cpp)
target_compile_definitions(ttpcomm_compatibility_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttpcomm_compatibility_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ttpcomm_compatibility_tests PRIVATE ttplayer_core ttpcomm_api)
add_dependencies(ttpcomm_compatibility_tests ttpcomm_mock_revision ttpcomm_mock_no_version)

add_executable(native_output_contract_tests tests/native_output_contract_tests.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio/wasapi_playback_tests.cpp")
  add_executable(wasapi_playback_tests tests/audio/wasapi_playback_tests.cpp)
  target_include_directories(wasapi_playback_tests PRIVATE src/ui)
  target_compile_definitions(wasapi_playback_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(wasapi_playback_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(wasapi_playback_tests PRIVATE ttplayer_core)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio/output_switch_volume_tests.cpp")
  add_executable(output_switch_volume_tests tests/audio/output_switch_volume_tests.cpp)
  target_compile_definitions(output_switch_volume_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(output_switch_volume_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(output_switch_volume_tests PRIVATE ttplayer_core)
endif()
target_include_directories(native_output_contract_tests PRIVATE src/ui)
target_compile_definitions(native_output_contract_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(native_output_contract_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(native_output_contract_tests PRIVATE ttplayer_core)

add_executable(kernel_streaming_sink_tests tests/kernel_streaming_sink_tests.cpp)
target_compile_definitions(kernel_streaming_sink_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(kernel_streaming_sink_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(kernel_streaming_sink_tests PRIVATE ttplayer_core)

add_executable(asio_sink_tests tests/asio_sink_tests.cpp)
target_compile_definitions(asio_sink_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(asio_sink_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(asio_sink_tests PRIVATE ttplayer_core)

add_executable(pcm_output_transform_tests
  tests/pcm_output_transform_tests.cpp)
target_compile_definitions(pcm_output_transform_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pcm_output_transform_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(pcm_output_transform_tests PRIVATE ttplayer_core)

add_executable(equalizer_recovery_tests tests/equalizer_recovery_tests.cpp)
target_compile_definitions(equalizer_recovery_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# This suite also uses assert() throughout; keep its Sandbox Release checks
# active for the same reason as ttplayer_tests above.
target_compile_options(equalizer_recovery_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(equalizer_recovery_tests PRIVATE ttplayer_core)

add_executable(discord_presence_tests tests/discord_presence_tests.cpp)
target_compile_definitions(discord_presence_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(discord_presence_tests PRIVATE
  /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(discord_presence_tests PRIVATE ttplayer_core)

add_executable(discord_sync_tests tests/discord_sync_tests.cpp)
target_compile_definitions(discord_sync_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(discord_sync_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /UNDEBUG)
target_link_libraries(discord_sync_tests PRIVATE ttplayer_core)

add_executable(ttpcomm_api_test ../reverse/tests/test_ttpcomm_api.c)
target_link_libraries(ttpcomm_api_test PRIVATE ttpcomm_api)

# Reverse-engineering probe for the legacy ttpGetSoundAddIn ABI. With optional
# sample paths it exercises creator/open/format/read-to-EOF/seek/read as well
# as reporting formats whose reader creators registered successfully.
add_executable(sound_addin_probe tools/probe_sound_addins.cpp)
target_compile_definitions(sound_addin_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(sound_addin_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(sound_addin_probe PRIVATE ttplayer_core)

# Destructive-by-design diagnostic for the exact ordinal-101 ReplayGain path.
# It is intentionally an explicit command-line probe and should be pointed at
# a disposable copy of an audio file because successful execution writes two
# metadata comments through the legacy add-in.
add_executable(replay_gain_probe tools/probe_replay_gain.cpp)
target_compile_definitions(replay_gain_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(replay_gain_probe PRIVATE
  /W4 /permissive- /EHsc /utf-8)
target_link_libraries(replay_gain_probe PRIVATE ttplayer_core)

# End-to-end diagnostic for the decoder -> processor -> waveOut path.  Unlike
# the unit tests this uses explicit exit-status checks, so Release/NDEBUG
# builds exercise playback instead of compiling assert expressions away.
add_executable(audio_playback_probe tools/probe_audio_playback.cpp)
target_compile_definitions(audio_playback_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(audio_playback_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(audio_playback_probe PRIVATE ttplayer_core)

add_executable(progress_seek_tests tests/progress_seek_tests.cpp)
target_compile_definitions(progress_seek_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(progress_seek_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(progress_seek_tests PRIVATE ttplayer_core)

add_executable(taskbar_playback_tests tests/taskbar_playback_tests.cpp)
target_compile_definitions(taskbar_playback_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(taskbar_playback_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(taskbar_playback_tests PRIVATE ttplayer_core)

add_executable(skin_rebind_tests tests/ui/skin_rebind_tests.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/waskin/host_tests.cpp")
  add_executable(waskin_host_tests tests/waskin/host_tests.cpp)
  target_compile_definitions(waskin_host_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(waskin_host_tests PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(waskin_host_tests PRIVATE ttplayer_core)
endif()
target_compile_definitions(skin_rebind_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(skin_rebind_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(skin_rebind_tests PRIVATE ttplayer_core)

add_executable(playlist_marquee_tests tests/ui/playlist_marquee_tests.cpp
  src/app/resources.rc src/app/ttplayer.manifest)
target_compile_definitions(playlist_marquee_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(playlist_marquee_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(playlist_marquee_tests PRIVATE ttplayer_core)
add_test(NAME playlist_marquee_tests COMMAND playlist_marquee_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(playlist_marquee_tests PROPERTIES TIMEOUT 30)

add_executable(pcm_integrity_probe tools/probe_pcm_integrity.cpp)
target_compile_definitions(pcm_integrity_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pcm_integrity_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(pcm_integrity_probe PRIVATE ttplayer_core)

add_executable(ttpcomm_processor_probe tools/probe_ttpcomm_processors.cpp)
target_compile_definitions(ttpcomm_processor_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ttpcomm_processor_probe PRIVATE /W4 /permissive- /EHsc /utf-8)

# Deterministic cross-process OLE source for the isolated playlist ingress
# regression. It publishes a real CF_HDROP IDataObject and enters DoDragDrop;
# unlike WM_DROPFILES injection this proves that the destination registered an
# IDropTarget. Keep the CRT static so the helper itself also runs in a pristine
# Windows Sandbox image.
add_executable(ole_drop_source tools/windows_sandbox/ole_drop_source.cpp)
target_compile_definitions(ole_drop_source PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ole_drop_source PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(ole_drop_source PRIVATE ole32 shell32 user32)
set_property(TARGET ole_drop_source PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

# The two legacy ttpres.dll files contain resources only: no code, entry point,
# imports, or exports.  Their recovered .res files are linked as /NOENTRY DLLs
# so resource type/name/language identities remain byte-for-byte compatible.
if(TTPLAYER_STAGE_RUNTIME)
set(TTPRES_RECOVERY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../reverse/ttpres")
set(TTPRES_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
          "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
  VERBATIM
)

add_custom_command(
  OUTPUT "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TTPRES_OUTPUT_DIR}/root"
  COMMAND "${CMAKE_LINKER}" /NOLOGO /DLL /NOENTRY /MACHINE:X86 /NODEFAULTLIB
          /BASE:0x6FF50000
          "/OUT:${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
          "${TTPRES_RECOVERY_DIR}/root/ttpres_original.res"
  DEPENDS "${TTPRES_RECOVERY_DIR}/root/ttpres_original.res"
  VERBATIM
)

add_custom_command(
  OUTPUT "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TTPRES_OUTPUT_DIR}/AddIn"
  COMMAND "${CMAKE_LINKER}" /NOLOGO /DLL /NOENTRY /MACHINE:X86 /NODEFAULTLIB
          /BASE:0x6FF50000
          "/OUT:${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
          "${TTPRES_RECOVERY_DIR}/addin/ttpres_addin_original.res"
  DEPENDS "${TTPRES_RECOVERY_DIR}/addin/ttpres_addin_original.res"
  VERBATIM
)

add_custom_target(ttpres_resources ALL
  DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/ttpcomm.dll"
    "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
    "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
)

# The recovered program resolves runtime files strictly beside the executable.
# Keep the intermediate resource products for tests/inspection, then stage the
# exact runtime layout into every single- or multi-config output directory.
add_dependencies(ttplayer_rebuild ttpres_resources)
add_custom_command(TARGET ttplayer_rebuild POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/../ttpcomm.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpcomm.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${TTPRES_OUTPUT_DIR}/root/ttpres.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../Skin"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Skin"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Skin/new"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../Icons"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/Icons"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../AddIn"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../PlayList"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/PlayList"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${TTPRES_OUTPUT_DIR}/AddIn/ttpres.dll"
          "$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn/ttpres.dll"
  COMMAND "${CMAKE_COMMAND}"
          "-DSOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/.."
          "-DDESTINATION=$<TARGET_FILE_DIR:ttplayer_rebuild>"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_settings.cmake"
  COMMAND "${CMAKE_COMMAND}"
          "-DPRESET_FILE=$<TARGET_FILE_DIR:ttplayer_rebuild>/AddIn/ttp_clienc.xml"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/repair_cli_presets.cmake"
  VERBATIM
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll")
  add_custom_command(TARGET ttplayer_rebuild POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll"
            "$<TARGET_FILE_DIR:ttplayer_rebuild>/mp3PRO.dll"
    VERBATIM)
endif()
endif()

if(BUILD_TESTING)
add_test(NAME build_date_tests
  COMMAND "${TTPLAYER_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_build_date.ps1")
add_library(mp3pro_input_mock SHARED tests/mock_winamp_input.cpp tests/mock_winamp_input.def)
target_compile_definitions(mp3pro_input_mock PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mp3pro_input_mock PRIVATE /W4 /EHsc /utf-8)
add_executable(mp3pro_source_tests tests/mp3pro_source_tests.cpp)
target_compile_definitions(mp3pro_source_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mp3pro_source_tests PRIVATE /W4 /EHsc /utf-8 /UNDEBUG)
target_link_libraries(mp3pro_source_tests PRIVATE ttplayer_core)
add_dependencies(mp3pro_source_tests mp3pro_input_mock)
add_test(NAME mp3pro_source_tests COMMAND mp3pro_source_tests
    "$<TARGET_FILE:mp3pro_input_mock>" "${CMAKE_CURRENT_SOURCE_DIR}/../mp3PRO.dll")
set_tests_properties(mp3pro_source_tests PROPERTIES TIMEOUT 60)
add_test(NAME ttplayer_tests COMMAND ttplayer_tests)
add_test(NAME shell_send_to_tests COMMAND shell_send_to_tests)
add_test(NAME playlist_network_commands_tests
         COMMAND playlist_network_commands_tests)
add_test(NAME file_info_probe_protocol_tests
         COMMAND file_info_probe_protocol_tests)
add_test(NAME builtin_file_info_tests COMMAND builtin_file_info_tests)
add_test(NAME file_association_tests COMMAND file_association_tests)
add_test(NAME playlist_list_contract_tests
         COMMAND playlist_list_contract_tests)
add_test(NAME playlist_local_search_tests
         COMMAND playlist_local_search_tests)
add_test(NAME desktop_lyrics_tests COMMAND desktop_lyrics_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME audio_recovery_tests COMMAND audio_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME conversion_recovery_tests COMMAND conversion_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(conversion_recovery_tests PROPERTIES TIMEOUT 60)
add_test(NAME progress_seek_tests COMMAND progress_seek_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME taskbar_playback_tests COMMAND taskbar_playback_tests)
add_test(NAME skin_rebind_tests COMMAND skin_rebind_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME native_output_contract_tests COMMAND native_output_contract_tests)
add_test(NAME kernel_streaming_sink_tests COMMAND kernel_streaming_sink_tests)
add_test(NAME asio_sink_tests COMMAND asio_sink_tests)
add_test(NAME pcm_output_transform_tests COMMAND pcm_output_transform_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME equalizer_recovery_tests COMMAND equalizer_recovery_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME discord_presence_tests COMMAND discord_presence_tests)
add_test(NAME discord_sync_tests COMMAND discord_sync_tests)
set_tests_properties(discord_presence_tests discord_sync_tests PROPERTIES TIMEOUT 45)
add_test(NAME ttpcomm_api_test COMMAND ttpcomm_api_test)
add_test(NAME ttpcomm_compatibility_tests COMMAND ttpcomm_compatibility_tests
         "$<TARGET_FILE:ttpcomm_mock_revision>"
         "$<TARGET_FILE:ttpcomm_mock_no_version>")
set_tests_properties(ttpcomm_compatibility_tests PROPERTIES
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:ttplayer_rebuild>")
add_test(NAME ttplayer_ui_smoke COMMAND ttplayer_rebuild --smoke-test)
add_test(NAME ttpcomm_processor_probe COMMAND ttpcomm_processor_probe
         "$<TARGET_FILE_DIR:ttplayer_rebuild>/ttpcomm.dll")
add_test(NAME dsp_probe_tests COMMAND dsp_probe_tests
         "$<TARGET_FILE:ttplayer_dsp_probe>"
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_stall>")
add_test(NAME embedded_dsp_probe_tests COMMAND dsp_probe_tests
         "$<TARGET_FILE:ttplayer_rebuild>"
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_stall>" --embedded)
add_test(NAME portable_worker_tests COMMAND portable_worker_tests
         "$<TARGET_FILE:ttplayer_rebuild>"
         "$<TARGET_FILE:dsp_probe_configure>"
         "$<TARGET_FILE:ttplayer_output_device_probe>")
set_tests_properties(portable_worker_tests embedded_dsp_probe_tests PROPERTIES TIMEOUT 45)
add_test(NAME winamp_dsp_runtime_tests COMMAND winamp_dsp_runtime_tests
         "$<TARGET_FILE:dsp_probe_valid>"
         "$<TARGET_FILE:dsp_probe_old>"
         "$<TARGET_FILE:dsp_probe_fault>"
         "$<TARGET_FILE:dsp_probe_pacemaker>")
set_tests_properties(ttplayer_ui_smoke PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
endif()
