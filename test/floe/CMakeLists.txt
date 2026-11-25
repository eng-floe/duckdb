add_library_unity(test_floe OBJECT floe_selectivity.cpp)

set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_floe>
    PARENT_SCOPE)
