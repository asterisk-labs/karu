cmake_minimum_required(VERSION 3.21)
project(karu_package_consumer LANGUAGES C CXX)

find_package(karu CONFIG REQUIRED)
add_executable(karu_package_consumer main.cpp)
target_link_libraries(karu_package_consumer PRIVATE karu::karu)

add_executable(karu_c_package_consumer main.c)
set_target_properties(karu_c_package_consumer PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(karu_c_package_consumer PRIVATE karu::karu)

include(CTest)
add_test(NAME karu_package_consumer COMMAND karu_package_consumer)
add_test(NAME karu_c_package_consumer COMMAND karu_c_package_consumer)
