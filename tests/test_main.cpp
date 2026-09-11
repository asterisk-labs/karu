#include "karu/karu.h"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>

int main() {
    using namespace karu::test;

    std::printf("karu %s (%s)\n", karu_version_string(), karu_http_backend());
    test_uri_identity();
    test_windows();
    test_config_precedence();
    test_s3_request();
    test_managed_headers();
    test_gcs_request();
    test_azure_request();
    test_hugging_face_request();
    test_source_request();
    test_renewable_callback_cache();
    test_local_engine();
    test_windows_allocations_and_errors();
    test_batch_lifetimes_and_concurrency();
    test_engine_shutdown();
    test_planner_scope();
    test_planner_property();
    test_transport_statuses();
    test_cpp_facade();

    if (!fixture_path.empty())
        std::filesystem::remove(fixture_path);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
