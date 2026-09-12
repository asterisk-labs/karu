#include "credential_support.hpp"

#include "platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <string>
#include <thread>

namespace karu::test {
namespace {

int g_fixture_port = 0;
std::string g_program_path;

std::string generate_private_key_pem() {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), EVP_PKEY_free);
    if (!key)
        return {};
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio)
        return {};
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return {};
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    if (size <= 0 || data == nullptr)
        return {};
    return std::string(data, static_cast<std::size_t>(size));
}

} // namespace

TempTree::TempTree(std::string_view label) {
    static int counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("karu_" + std::string(label) + "_" + std::to_string(os::pid()) + "_" +
                       std::to_string(++counter));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    root_ = path.string();
}

TempTree::~TempTree() {
    std::error_code error;
    std::filesystem::remove_all(os::path_from_utf8(root_), error);
}

std::string TempTree::write(std::string_view relative, std::string_view contents) const {
    const auto full =
        std::filesystem::path(os::path_from_utf8(root_)) / os::path_from_utf8(relative);
    std::error_code error;
    std::filesystem::create_directories(full.parent_path(), error);
    std::ofstream stream(full, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    return full.string();
}

std::string TempTree::absent(std::string_view relative) const {
    return (std::filesystem::path(os::path_from_utf8(root_)) / os::path_from_utf8(relative))
        .string();
}

const std::string& test_private_key_pem() {
    static const std::string key = generate_private_key_pem();
    return key;
}

void set_program_path(const char* argv0) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(os::path_from_utf8(argv0), error);
    g_program_path = error ? std::string(argv0) : absolute.string();
}

std::string credential_process_command(std::string_view mode) {
    return "\"" + g_program_path + "\" --credential-helper=" + std::string(mode);
}

bool run_credential_helper(int argc, char** argv, int& exit_code) {
    constexpr std::string_view flag = "--credential-helper=";
    if (argc < 2)
        return false;
    const std::string_view argument(argv[1]);
    if (!argument.starts_with(flag))
        return false;
    const std::string_view mode = argument.substr(flag.size());

    if (mode == "fail") {
        // Silent: the exit status is the whole signal, and a line on stderr
        // would look like a real failure in a CI log.
        exit_code = 3;
        return true;
    }
    if (mode == "incomplete") {
        std::fputs(R"({"Version":1,"AccessKeyId":"AKIAHELPER"})", stdout);
        exit_code = 0;
        return true;
    }
    if (mode == "no-key") {
        std::fputs(R"({"Version":1,"SecretAccessKey":"helper-secret"})", stdout);
        exit_code = 0;
        return true;
    }
    if (mode == "hang") {
        // Outlives any timeout the suite configures, so the caller exercises
        // the deadline rather than the exit path.
        std::this_thread::sleep_for(std::chrono::seconds(30));
        exit_code = 0;
        return true;
    }
    if (mode == "flood") {
        // More than the 1 MiB credential_process output limit.
        const std::string chunk(64 * 1024, 'x');
        for (int index = 0; index < 32; ++index)
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
        exit_code = 0;
        return true;
    }
    // "ok" and anything else: a complete, unexpired credential document.
    const std::string body = std::string(R"({"Version":1,"AccessKeyId":"AKIAHELPER",)") +
                             R"("SecretAccessKey":"helper-secret","SessionToken":"helper-token",)" +
                             R"("Expiration":")" + iso8601_in(3600) + R"("})";
    std::fwrite(body.data(), 1, body.size(), stdout);
    exit_code = 0;
    return true;
}

void set_fixture_port(int port) {
    g_fixture_port = port;
}

int fixture_port() {
    return g_fixture_port;
}

std::string fixture_url(std::string_view route) {
    return "http://127.0.0.1:" + std::to_string(g_fixture_port) + std::string(route);
}

ConfigBuilder empty_builder() {
    return ConfigBuilder(false);
}

std::string iso8601_in(std::int64_t seconds_from_now) {
    const std::time_t when = std::time(nullptr) + static_cast<std::time_t>(seconds_from_now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &when);
#else
    gmtime_r(&when, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

} // namespace karu::test
