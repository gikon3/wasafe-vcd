include_guard()

add_executable(wasafe_vcd_bench bench_vcd.cpp)
set_target_properties(wasafe_vcd_bench PROPERTIES OUTPUT_NAME wasafe-vcd-bench)

target_link_libraries(wasafe_vcd_bench PRIVATE wasafe::wasafe_vcd)

# Генератор синтетических дампов и счётный приёмник общие с тестами.
target_include_directories(wasafe_vcd_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)
