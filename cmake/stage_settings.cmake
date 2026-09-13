# Seed once. An incremental build must never reset the user's live settings.
if(NOT DEFINED SOURCE_DIRECTORY OR NOT DEFINED DESTINATION)
  message(FATAL_ERROR "SOURCE_DIRECTORY and DESTINATION are required")
endif()
set(settings_target "${DESTINATION}/TTPlayerRebuild.xml")
if(EXISTS "${settings_target}")
  return()
endif()
foreach(settings_source IN ITEMS
    "${DESTINATION}/TTPlayer.xml"
    "${SOURCE_DIRECTORY}/TTPlayerRebuild.xml"
    "${SOURCE_DIRECTORY}/TTPlayer.xml")
  if(EXISTS "${settings_source}" AND NOT IS_DIRECTORY "${settings_source}")
    file(MAKE_DIRECTORY "${DESTINATION}")
    file(COPY_FILE "${settings_source}" "${settings_target}")
    return()
  endif()
endforeach()
