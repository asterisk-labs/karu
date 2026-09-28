#ifndef KARU_RUNTIME_RESOLVER_HPP
#define KARU_RUNTIME_RESOLVER_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace karu {

// Resolves hosts off the I/O threads and rotates addresses across connections.
class HostResolver {
  public:
    using Lookup = std::function<std::vector<std::string>(const std::string& host)>;

    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    struct Entry {
        // Retry after a resolver wakeup.
        bool waiting = false;
        // CURLOPT_RESOLVE entry; additions expire in libcurl's DNS cache.
        std::string resolve;
    };

    // Lookup and clock overrides keep tests independent of DNS and wall time.
    explicit HostResolver(std::function<void()> wake, Lookup lookup = {}, Now now = Clock::now);
    ~HostResolver();
    HostResolver(const HostResolver&) = delete;
    HostResolver& operator=(const HostResolver&) = delete;

    [[nodiscard]] Entry entry(std::string_view url);
    // `wake` never runs after this returns.
    void stop() noexcept;

  private:
    struct Shared;
    static void serve(std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> shared_;
};

// The host and port a URL connects to, or nullopt for IP literals and URLs
// without a known port.
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
resolvable_host(std::string_view url);

} // namespace karu

#endif
