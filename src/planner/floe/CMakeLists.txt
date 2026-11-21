add_library_unity(duckdb_planner_floe OBJECT extracted_columns.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_planner_floe>
    PARENT_SCOPE)
