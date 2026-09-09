#include "test_support.hpp"

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace karu::test {

int failures = 0;
int checks = 0;
const char* current = "";
std::string fixture_path;

void fail(int line, const std::string& message) {
    ++failures;
    std::printf("  FAIL %s:%d  %s\n", current, line, message.c_str());
}

void ok_at(int line, bool condition, const char* expression) {
    ++checks;
    if (!condition)
        fail(line, std::string(expression) + " was false");
}

void string_at(int line, std::string_view left, std::string_view right, const char* expression) {
    ++checks;
    if (left != right) {
        fail(line, std::string(expression) + " gave '" + std::string(left) + "', expected '" +
                       std::string(right) + "'");
    }
}

Section::Section(const char* name) {
    current = name;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

ScopedEnvironment::ScopedEnvironment(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name); previous != nullptr)
        previous_ = previous;
    if (value == nullptr)
        unset_env(name);
    else
        set_env(name, value);
}

ScopedEnvironment::~ScopedEnvironment() {
    if (previous_)
        set_env(name_.c_str(), previous_->c_str());
    else
        unset_env(name_.c_str());
}

Resolved must_resolve(std::string_view uri) {
    auto result = resolve(uri);
    if (!result) {
        fail(__LINE__, std::string(uri) + ": " + result.error().message);
        return {};
    }
    return std::move(*result);
}

ConfigSnapshot must_freeze(const ConfigBuilder& builder) {
    auto result = builder.freeze();
    if (!result) {
        fail(__LINE__, result.error());
        return {};
    }
    return std::move(*result);
}

std::string header(const PreparedRequest& request, std::string_view name) {
    for (const auto& [candidate, value] : request.headers) {
        if (candidate == name)
            return value;
    }
    return {};
}

std::vector<unsigned char> make_fixture(std::size_t size) {
    std::vector<unsigned char> result(size);
    for (std::size_t index = 0; index < size; ++index)
        result[index] = static_cast<unsigned char>((index * 31 + 7) & 0xff);
    const auto path = std::filesystem::temp_directory_path() /
                      ("karu_test_" + std::to_string(os::pid()) + ".bin");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(result.data()),
                 static_cast<std::streamsize>(result.size()));
    if (!stream)
        fail(__LINE__, "could not write fixture");
    fixture_path = path.string();
    return result;
}

} // namespace karu::test
