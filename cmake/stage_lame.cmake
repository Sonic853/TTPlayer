# User-supplied archive, no network download. The DLL encoder has no executable
# dependency; lame.exe is optional, for the original command-line presets only.
# Selecting x86/x64 follows the consuming target, not the operating system.
if(NOT EXISTS "${ARCHIVE}")
  return()
endif()
file(MAKE_DIRECTORY "${STAGING}")
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${STAGING}"
     PATTERNS "lame_enc.dll" "libmpg123-0.dll" "lame.exe")
if(NOT EXISTS "${STAGING}/lame_enc.dll")
  message(FATAL_ERROR "The supplied LAME archive has no lame_enc.dll")
endif()
if(EXISTS "${STAGING}/lame.exe")
  file(MAKE_DIRECTORY "${DESTINATION}/Encoders")
  # Do not replace a separately installed/custom command-line encoder.
  if(NOT EXISTS "${DESTINATION}/Encoders/lame.exe")
    file(COPY_FILE "${STAGING}/lame.exe" "${DESTINATION}/Encoders/lame.exe")
  endif()
  if(EXISTS "${STAGING}/libmpg123-0.dll" AND
     NOT EXISTS "${DESTINATION}/Encoders/libmpg123-0.dll")
    file(COPY_FILE "${STAGING}/libmpg123-0.dll" "${DESTINATION}/Encoders/libmpg123-0.dll")
  endif()
endif()
file(COPY_FILE "${STAGING}/lame_enc.dll" "${DESTINATION}/lame_enc.dll"
     ONLY_IF_DIFFERENT)
if(EXISTS "${STAGING}/libmpg123-0.dll")
  file(COPY_FILE "${STAGING}/libmpg123-0.dll" "${DESTINATION}/libmpg123-0.dll"
       ONLY_IF_DIFFERENT)
endif()
