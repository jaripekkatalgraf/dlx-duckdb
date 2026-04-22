# extension_config.cmake
# Consumed by DuckDB's top-level CMake when building out-of-tree extensions.

duckdb_extension_load(dlx
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    DESCRIPTION "Exact cover solver using Knuth's Dancing Links (Algorithm DLX)"
    LOAD_TESTS
)
