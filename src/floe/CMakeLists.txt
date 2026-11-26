add_library_unity(duckdb_floe OBJECT floe_selectivity.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_floe>
    PARENT_SCOPE)
