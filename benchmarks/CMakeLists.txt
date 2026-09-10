add_executable(karu_coalescing_benchmark coalescing.cpp)
target_link_libraries(karu_coalescing_benchmark PRIVATE karu::karu)
target_include_directories(karu_coalescing_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(karu_coalescing_benchmark PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Werror>)
