add_executable(karu_tests
    test_main.cpp
    test_support.cpp
    test_uri_config.cpp
    test_s3.cpp
    test_gcs.cpp
    test_azure.cpp
    test_hugging_face.cpp
    test_source.cpp
    test_engine.cpp)
target_link_libraries(karu_tests PRIVATE karu::karu)
target_include_directories(karu_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(karu_tests PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
target_compile_options(karu_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Werror>)
add_test(NAME karu COMMAND karu_tests)

add_executable(karu_c_api_test test_c_api.c)
set_target_properties(karu_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(karu_c_api_test PRIVATE c_std_11)
target_link_libraries(karu_c_api_test PRIVATE karu::karu)
add_test(NAME karu_c_api COMMAND karu_c_api_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_executable(karu_http_test test_http.cpp)
    target_link_libraries(karu_http_test PRIVATE karu::karu)
    target_compile_options(karu_http_test PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4;/utf-8>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow>
        $<$<AND:$<BOOL:${KARU_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
        $<$<AND:$<BOOL:${KARU_WERROR}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Werror>)
    add_test(NAME karu_http
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/http_server.py
                $<TARGET_FILE:karu_http_test>)
endif()
