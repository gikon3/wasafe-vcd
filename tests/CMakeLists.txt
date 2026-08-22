include_guard()

if(NOT BUILD_TESTING)
    return()
endif()

find_package(GTest REQUIRED)

add_executable(wasafe_vcd_tests
    test_code_book.cpp
    test_emitter.cpp
    test_scanner.cpp
    test_value_text.cpp
    test_vcd.cpp
    test_vcd_large.cpp
    test_vcd_writer.cpp)

set_target_properties(wasafe_vcd_tests PROPERTIES
    OUTPUT_NAME wasafe-vcd-tests)

target_link_libraries(wasafe_vcd_tests
    PRIVATE
    wasafe::wasafe_vcd
    GTest::gtest_main)

# Тесты внутренних узлов (Scanner) видят приватные заголовки библиотеки.
target_include_directories(wasafe_vcd_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(wasafe_vcd_tests)
