include_directories(include)

add_subdirectory(storage)

add_library(
  odbc_ext_library OBJECT
  odbc_connection.cpp
  sqlserver_extension.cpp
  odbc_filter_pushdown.cpp
  odbc_metadata.cpp
  odbc_parameter.cpp
  odbc_result.cpp
  odbc_scanner.cpp
  odbc_storage.cpp
  odbc_types.cpp
  odbc_utils.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:odbc_ext_library>
    PARENT_SCOPE)
