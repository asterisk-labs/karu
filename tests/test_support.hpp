#ifndef KARU_TEST_SUPPORT_HPP
#define KARU_TEST_SUPPORT_HPP

#include "config.hpp"
#include "request_builder.hpp"
#include "uri.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace karu::test {

extern int failures;
extern int checks;
extern const char* current;
extern std::string fixture_path;

void fail(int line, const std::string& message);
void ok_at(int line, bool condition, const char* expression);

template <typename Left, typename Right>
void equal_at(int line, const Left& left, const Right& right, const char* expression) {
    ++checks;
    if (!(left == static_cast<Left>(right)))
        fail(line, std::string(expression) + " did not equal the expected value");
}

void string_at(int line, std::string_view left, std::string_view right, const char* expression);

struct Section {
    explicit Section(const char* name);
};

void set_env(const char* name, const char* value);
void unset_env(const char* name);

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, const char* value);
    ~ScopedEnvironment();
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

[[nodiscard]] Resolved must_resolve(std::string_view uri);
[[nodiscard]] ConfigSnapshot must_freeze(const ConfigBuilder& builder);
[[nodiscard]] std::string header(const PreparedRequest& request, std::string_view name);
[[nodiscard]] std::vector<unsigned char> make_fixture(std::size_t size);

void test_uri_identity();
void test_windows();
void test_config_precedence();
void test_s3_request();
void test_managed_headers();
void test_gcs_request();
void test_azure_request();
void test_hugging_face_request();
void test_source_request();
void test_renewable_callback_cache();
void test_local_engine();
void test_windows_allocations_and_errors();
void test_batch_lifetimes_and_concurrency();
void test_engine_shutdown();
void test_planner_scope();
void test_planner_property();
void test_transport_statuses();
void test_cpp_facade();

} // namespace karu::test

#define OK(value) ::karu::test::ok_at(__LINE__, static_cast<bool>(value), #value)
#define EQ(left, right) ::karu::test::equal_at(__LINE__, (left), (right), #left)
#define EQS(left, right) ::karu::test::string_at(__LINE__, (left), (right), #left)
#define KARU_TEST_JOIN_IMPL(left, right) left##right
#define KARU_TEST_JOIN(left, right) KARU_TEST_JOIN_IMPL(left, right)
#define SECTION(name) ::karu::test::Section KARU_TEST_JOIN(section_, __LINE__)(name)

#endif
