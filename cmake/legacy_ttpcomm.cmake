# ttpcomm!srand48/lrand48 use __declspec(thread). XP allocates that DLL's
# TLS only when it is an ordinary startup dependency (as in TTPlayer.exe).
# Loading it later aliases the EXE's TLS slot and corrupts the CRT epoch.
# Keep one ordinal import alive even under /OPT:REF and LTCG. No call or
# version check is needed; the existing API adapter still resolves exports.
set(ttpcomm_import_library "${CMAKE_CURRENT_BINARY_DIR}/legacy-ttpcomm/ttpcomm.lib")
add_custom_command(OUTPUT "${ttpcomm_import_library}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/legacy-ttpcomm"
  COMMAND "${CMAKE_AR}" /nologo /machine:X86
    "/def:${CMAKE_CURRENT_SOURCE_DIR}/cmake/legacy_ttpcomm.def"
    "/out:${ttpcomm_import_library}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/legacy_ttpcomm.def"
  COMMENT "Generating the XP startup import for ttpcomm TLS"
  VERBATIM)
add_custom_target(ttplayer_legacy_ttpcomm_import DEPENDS "${ttpcomm_import_library}")
add_dependencies(ttplayer_rebuild ttplayer_legacy_ttpcomm_import)
target_link_libraries(ttplayer_rebuild PRIVATE "${ttpcomm_import_library}")
target_link_options(ttplayer_rebuild PRIVATE /INCLUDE:__imp__srand48)
