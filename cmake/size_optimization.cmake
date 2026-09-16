# Size optimization is a compile/link policy for shipping configurations only.
# Keep Debug and RelWithDebInfo convenient for stepping through recovered code.
if(NOT TTPLAYER_OPTIMIZE_SIZE)
  return()
endif()
if(NOT MSVC)
  message(FATAL_ERROR "TTPLAYER_OPTIMIZE_SIZE requires MSVC")
endif()

foreach(target IN ITEMS ttplayer_core ttpcomm_api ttplayer_rebuild)
  set_target_properties(${target} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  # /O1 includes string pooling and function-level linking. /Gw also packages
  # globals so the linker can discard unused data and merge identical data.
  target_compile_options(${target} PRIVATE
    "$<$<CONFIG:Release,MinSizeRel>:/O1;/Gw>")
endforeach()

# Consumers of the /GL static library also need LTCG (including local tests).
# Set it explicitly to avoid the linker's fallback/restart and VS defaults.
target_link_options(ttplayer_core INTERFACE
  "$<$<CONFIG:Release,MinSizeRel>:/LTCG;/INCREMENTAL:NO>")

# Audio conversion, DSP and device callbacks retain /O2. Source-level options
# follow the target options; LTCG retains each function's optimization policy.
get_target_property(core_sources ttplayer_core SOURCES)
foreach(source IN LISTS core_sources)
  if(source MATCHES "^src/audio/")
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
      "$<$<CONFIG:Release,MinSizeRel>:/O2>")
  endif()
endforeach()

target_link_options(ttplayer_rebuild PRIVATE
  "$<$<CONFIG:Release,MinSizeRel>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")
