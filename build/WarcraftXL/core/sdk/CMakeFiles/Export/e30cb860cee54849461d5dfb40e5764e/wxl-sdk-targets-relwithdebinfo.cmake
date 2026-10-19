#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "wxl::wxl-runtime" for configuration "RelWithDebInfo"
set_property(TARGET wxl::wxl-runtime APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(wxl::wxl-runtime PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/wxl-runtime.lib"
  )

list(APPEND _cmake_import_check_targets wxl::wxl-runtime )
list(APPEND _cmake_import_check_files_for_wxl::wxl-runtime "${_IMPORT_PREFIX}/lib/wxl-runtime.lib" )

# Import target "wxl::wxl-host" for configuration "RelWithDebInfo"
set_property(TARGET wxl::wxl-host APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(wxl::wxl-host PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/wxl-host.lib"
  )

list(APPEND _cmake_import_check_targets wxl::wxl-host )
list(APPEND _cmake_import_check_files_for_wxl::wxl-host "${_IMPORT_PREFIX}/lib/wxl-host.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
