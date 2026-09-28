#include "resolver.hpp"

#include "../text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define KARU_SYSTEM_LOOKUP 1
#endif

namespace karu {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kKeepAnswer = std::chrono::seconds(60);
constexpr auto kWaitForFirstAnswer = std::chrono::milliseconds(300);
// Rotate every transfer during startup, then parse fewer lists for warm transfers.
constexpr auto kPassAlways = std::chrono::seconds(1);
constexpr std::size_t kPassEvery = 8;
constexpr std::size_t kMaxHosts = 256;

bool proxy_environment_present() {
    for (const char* name :
         {"http_proxy", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0')
            return true;
    }
    return false;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool is_decimal(std::string_view text) {
    return !text.empty() &&
           std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c) != 0; });
}

#ifdef KARU_SYSTEM_LOOKUP
void resolve_family(const std::string& host, int family, std::vector<std::string>& addresses) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0)
        return;
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(found, freeaddrinfo);
    for (const addrinfo* item = found; item != nullptr; item = item->ai_next) {
        char text[INET6_ADDRSTRLEN] = {};
        const void* address = nullptr;
        if (item->ai_family == AF_INET6)
            address = &reinterpret_cast<const sockaddr_in6*>(item->ai_addr)->sin6_addr;
        else if (item->ai_family == AF_INET)
            address = &reinterpret_cast<const sockaddr_in*>(item->ai_addr)->sin_addr;
        if (address == nullptr || inet_ntop(item->ai_family, address, text, sizeof text) == nullptr)
            continue;
        std::string formatted =
            item->ai_family == AF_INET6 ? concat("[", text, "]") : std::string(text);
        if (std::ranges::find(addresses, formatted) == addresses.end())
            addresses.push_back(std::move(formatted));
    }
}

// Separate IPv4 and IPv6 lookups avoid paired A/AAAA queries.
std::vector<std::string> system_lookup(const std::string& host) {
    std::vector<std::string> addresses;
    std::vector<std::string> ipv4;
    std::exception_ptr failure;
    std::thread ipv6([&] {
        try {
            resolve_family(host, AF_INET6, addresses);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    try {
        resolve_family(host, AF_INET, ipv4);
    } catch (...) {
        ipv6.join();
        throw;
    }
    ipv6.join();
    if (failure)
        std::rethrow_exception(failure);
    addresses.insert(addresses.end(), ipv4.begin(), ipv4.end());
    return addresses;
}
#endif

} // namespace

struct HostResolver::Shared {
    struct Host {
        std::vector<std::string> addresses;
        Clock::time_point expires{};
        Clock::time_point started{};
        Clock::time_point answered{};
        std::size_t transfers = 0;
        std::size_t next = 0;
        bool looking = false;
    };

    std::mutex mutex;
    std::condition_variable wanted;
    std::function<void()> wake;
    Lookup lookup;
    Now now;
    std::atomic<bool> enabled{false};
    bool serving = false;
    bool stopped = false;
    std::unordered_map<std::string, Host> hosts;
    std::deque<std::string> queue;
};

std::optional<std::pair<std::string, std::string>> resolvable_host(std::string_view url) {
    const std::size_t separator = url.find("://");
    if (separator == std::string_view::npos)
        return std::nullopt;
    const std::string scheme = lowercase(url.substr(0, separator));
    if (scheme != "http" && scheme != "https")
        return std::nullopt;
    std::string_view authority = url.substr(separator + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos)
        authority.remove_prefix(at + 1);
    // An IPv6 literal needs no lookup.
    if (authority.empty() || authority.front() == '[')
        return std::nullopt;
    std::string_view host = authority;
    std::string_view port;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (port.empty())
        port = scheme == "https" ? "443" : scheme == "http" ? "80" : "";
    if (host.empty() || !is_decimal(port))
        return std::nullopt;
    // Neither does an IPv4 literal.
    if (std::ranges::all_of(host, [](unsigned char c) { return std::isdigit(c) != 0 || c == '.'; }))
        return std::nullopt;
    return std::pair{lowercase(host), std::string(port)};
}

HostResolver::HostResolver(std::function<void()> wake, Lookup lookup, Now now)
    : shared_(std::make_shared<Shared>()) {
    shared_->wake = std::move(wake);
    shared_->lookup = std::move(lookup);
    shared_->now = std::move(now);
#ifdef KARU_SYSTEM_LOOKUP
    if (!shared_->lookup)
        shared_->lookup = system_lookup;
#endif
    // Let libcurl apply proxy and no_proxy rules without local DNS queries.
    shared_->enabled.store(static_cast<bool>(shared_->lookup) && !proxy_environment_present(),
                           std::memory_order_relaxed);
}

HostResolver::~HostResolver() {
    stop();
}

void HostResolver::stop() noexcept {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->stopped = true;
        shared_->queue.clear();
    }
    shared_->wanted.notify_all();
}

void HostResolver::serve(std::shared_ptr<Shared> shared) {
    std::unique_lock lock(shared->mutex);
    for (;;) {
        shared->wanted.wait(lock, [&] { return shared->stopped || !shared->queue.empty(); });
        if (shared->stopped)
            return;
        const std::string host = std::move(shared->queue.front());
        shared->queue.pop_front();
        lock.unlock();
        std::vector<std::string> addresses;
        try {
            addresses = shared->lookup(host);
        } catch (...) {
            addresses.clear();
        }
        lock.lock();
        if (shared->stopped)
            return;
        // Failures are kept as long, so transfers wait at most once a minute.
        if (auto found = shared->hosts.find(host); found != shared->hosts.end()) {
            found->second.looking = false;
            found->second.addresses = std::move(addresses);
            found->second.answered = shared->now();
            found->second.expires = found->second.answered + kKeepAnswer;
            found->second.transfers = 0;
        }
        if (shared->wake)
            shared->wake();
    }
}

HostResolver::Entry HostResolver::entry(std::string_view url) {
    if (!shared_->enabled.load(std::memory_order_relaxed))
        return {};
    auto target = resolvable_host(url);
    if (!target)
        return {};
    const auto& [host, port] = *target;
    const auto now = shared_->now();

    std::lock_guard lock(shared_->mutex);
    if (shared_->stopped)
        return {};
    auto found = shared_->hosts.find(host);
    if (found == shared_->hosts.end()) {
        if (shared_->hosts.size() >= kMaxHosts) {
            std::erase_if(shared_->hosts, [now](const auto& item) {
                return !item.second.looking && item.second.expires <= now;
            });
            if (shared_->hosts.size() >= kMaxHosts)
                return {false, concat("-", host, ":", port)};
        }
        found = shared_->hosts.try_emplace(host).first;
    }
    Shared::Host& state = found->second;

    if (!state.looking && state.expires <= now) {
        if (!shared_->serving) {
            try {
                // System DNS cannot be cancelled. The worker owns its state after shutdown.
                std::thread(serve, shared_).detach();
            } catch (...) {
                shared_->enabled.store(false, std::memory_order_relaxed);
                return {};
            }
            shared_->serving = true;
        }
        shared_->queue.push_back(host);
        state.looking = true;
        state.started = now;
        shared_->wanted.notify_one();
    }

    if (!state.addresses.empty() && state.expires > now) {
        if (now - state.answered >= kPassAlways && state.transfers++ % kPassEvery != 0)
            return {};
        const std::size_t count = state.addresses.size();
        const std::size_t first = state.next++ % count;
        std::string resolve = concat("+", host, ":", port, ":");
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0)
                resolve.push_back(',');
            resolve += state.addresses[(first + index) % count];
        }
        return {false, std::move(resolve)};
    }
    if (state.looking && now - state.started < kWaitForFirstAnswer)
        return {true, {}};
    // Removal is per port: a failed refresh must not leave another port pinned.
    return {false, concat("-", host, ":", port)};
}

} // namespace karu
