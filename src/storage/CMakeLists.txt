add_library(
  odbc_ext_storage OBJECT
  odbc_catalog.cpp
  odbc_catalog_set.cpp
  odbc_clear_cache.cpp
  odbc_connection_pool.cpp
  odbc_optimizer.cpp
  odbc_schema_entry.cpp
  odbc_schema_set.cpp
  odbc_table_entry.cpp
  odbc_table_set.cpp
  odbc_transaction.cpp
  odbc_transaction_manager.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:odbc_ext_storage>
    PARENT_SCOPE)
