#include "locator.hpp"
#include "platform.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <utility>

namespace karu::test {

void test_hugging_face_request() {
    SECTION("Hugging Face request");
    auto resolved = must_resolve("hf://datasets/org/repo@v1/path/a b.bin");
    EQ(resolved.backend, Backend::HuggingFace);
    EQS(resolved.canonical_uri, "/vsihf/datasets/org/repo@v1/path/a b.bin");
    EQS(resolved.target, "/datasets/org/repo/resolve/v1/path/a%20b.bin");

    ConfigBuilder config(false);
    OK(config.set("HF_ENDPOINT", "https://mirror.example.test/base/"));
    OK(config.set("HF_TOKEN", "token"));
    OK(config.set("GDAL_HTTP_HEADERS", "X-Karu-Test: yes"));
    RequestBuilder request_builder(must_freeze(config));
    auto request = request_builder.prepare(Locator{std::move(resolved)}, 0, 1, {}, "\"etag\"");
    OK(request.has_value());
    if (request) {
        EQS(request->url,
            "https://mirror.example.test/base/datasets/org/repo/resolve/v1/path/a%20b.bin");
        EQS(header(*request, "Authorization"), "Bearer token");
        EQS(header(*request, "If-Match"), "\"etag\"");
        EQS(header(*request, "X-Karu-Test"), "yes");
    }

    SECTION("Hugging Face cached token");
    const auto token_home =
        std::filesystem::temp_directory_path() / ("karu_hf_" + std::to_string(os::pid()));
    std::filesystem::create_directories(token_home);
    const auto token_path = token_home / "token";
    {
        std::ofstream token_file(token_path, std::ios::binary | std::ios::trunc);
        token_file << "cached-token\n";
        OK(token_file.good());
    }

    ConfigBuilder cached(false);
    OK(cached.set("HF_HOME", token_home.string().c_str()));
    RequestBuilder cached_builder(must_freeze(cached));
    auto cached_request =
        cached_builder.prepare(Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
    OK(cached_request.has_value());
    if (cached_request)
        EQS(header(*cached_request, "Authorization"), "Bearer cached-token");

    // Token files are consulted for each request preparation. Rotating a
    // login does not require rebuilding the client or introduce object state.
    {
        std::ofstream token_file(token_path, std::ios::binary | std::ios::trunc);
        token_file << "rotated-token\n";
        OK(token_file.good());
    }
    auto rotated_request =
        cached_builder.prepare(Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
    OK(rotated_request.has_value());
    if (rotated_request)
        EQS(header(*rotated_request, "Authorization"), "Bearer rotated-token");

    ConfigBuilder custom_path(false);
    OK(custom_path.set("HF_HOME", "/ignored"));
    OK(custom_path.set("HF_TOKEN_PATH", token_path.string().c_str()));
    RequestBuilder custom_path_builder(must_freeze(custom_path));
    auto custom_path_request = custom_path_builder.prepare(
        Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
    OK(custom_path_request.has_value());
    if (custom_path_request)
        EQS(header(*custom_path_request, "Authorization"), "Bearer rotated-token");

    ConfigBuilder direct(false);
    OK(direct.set("HF_HOME", token_home.string().c_str()));
    OK(direct.set("HUGGING_FACE_HUB_TOKEN", "explicit-token"));
    RequestBuilder direct_builder(must_freeze(direct));
    auto direct_request =
        direct_builder.prepare(Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
    OK(direct_request.has_value());
    if (direct_request)
        EQS(header(*direct_request, "Authorization"), "Bearer explicit-token");

    const auto cache_home = token_home / "cache";
    std::filesystem::create_directories(cache_home / "huggingface");
    {
        std::ofstream token_file(cache_home / "huggingface" / "token",
                                 std::ios::binary | std::ios::trunc);
        token_file << "cli-token\n";
        OK(token_file.good());
    }
    {
        ScopedEnvironment token("HF_TOKEN", nullptr);
        ScopedEnvironment legacy_token("HUGGING_FACE_HUB_TOKEN", nullptr);
        ScopedEnvironment token_file("HF_TOKEN_PATH", nullptr);
        ScopedEnvironment hub_home("HF_HOME", nullptr);
        ScopedEnvironment cache("XDG_CACHE_HOME", cache_home.string().c_str());
        ConfigBuilder environment(true);
        RequestBuilder environment_builder(must_freeze(environment));
        auto environment_request = environment_builder.prepare(
            Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
        OK(environment_request.has_value());
        if (environment_request)
            EQS(header(*environment_request, "Authorization"), "Bearer cli-token");
    }

    ConfigBuilder hermetic(false);
    RequestBuilder hermetic_builder(must_freeze(hermetic));
    auto anonymous_request = hermetic_builder.prepare(
        Locator{must_resolve("hf://datasets/org/repo/file.bin")}, 0, 1, {});
    OK(anonymous_request.has_value());
    if (anonymous_request)
        OK(header(*anonymous_request, "Authorization").empty());

    std::filesystem::remove_all(token_home);
}

} // namespace karu::test
