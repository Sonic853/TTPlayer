include(FetchContent)
FetchContent_Declare(ttplayer_wtl
  URL https://downloads.sourceforge.net/project/wtl/WTL%2010/WTL%2010.01%20Release/WTL10_01_Release.zip
  URL_HASH SHA256=1a62ea728d088c7c5c7cfc76db445e5c9f04923f92cadcf27b5d7678d85826a2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ttplayer_wtl)

add_library(ttplayer_wtl_headers INTERFACE)
target_include_directories(ttplayer_wtl_headers SYSTEM INTERFACE "${ttplayer_wtl_SOURCE_DIR}/Include")
target_compile_definitions(ttplayer_wtl_headers INTERFACE
  _WTL_NO_AUTOMATIC_NAMESPACE _ATL_NO_AUTOMATIC_NAMESPACE
  _ATL_XP_TARGETING)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wtl-license")
configure_file("${ttplayer_wtl_SOURCE_DIR}/MS-PL.txt"
  "${CMAKE_CURRENT_BINARY_DIR}/wtl-license/WTL-MS-PL.txt" COPYONLY)
