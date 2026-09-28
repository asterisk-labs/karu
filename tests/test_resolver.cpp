#include "runtime/resolver.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace karu::test {
namespace {

using namespace std::chrono_literals;

// Polls until `ready` or a second passes.
template <typename Predicate> bool eventually(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

} // namespace

void test_host_resolver() {
    SECTION("Host resolver");
    ScopedEnvironment http_proxy("http_proxy", nullptr);
    ScopedEnvironment https_proxy("https_proxy", nullptr);
    ScopedEnvironment upper_https_proxy("HTTPS_PROXY", nullptr);
    ScopedEnvironment all_proxy("all_proxy", nullptr);
    ScopedEnvironment upper_all_proxy("ALL_PROXY", nullptr);
    std::atomic<long long> elapsed{0};
    const auto now = [&] {
        return HostResolver::Clock::time_point{} + std::chrono::milliseconds(elapsed.load());
    };

    auto host = resolvable_host("https://Bucket.S3.amazonaws.com/key?x=1");
    OK(host.has_value());
    if (host) {
        EQS(host->first, "bucket.s3.amazonaws.com");
        EQS(host->second, "443");
    }
    host = resolvable_host("http://user:secret@minio.test:9000/bucket");
    OK(host.has_value());
    if (host) {
        EQS(host->first, "minio.test");
        EQS(host->second, "9000");
    }
    OK(!resolvable_host("http://127.0.0.1:9000/object"));
    OK(!resolvable_host("https://[2001:db8::1]/object"));
    OK(!resolvable_host("ftp://files.test/object"));
    OK(!resolvable_host("ftp://files.test:21/object"));
    OK(!resolvable_host("not a url"));

    // Both ports share one lookup.
    std::atomic<int> lookups{0};
    std::atomic<int> wakes{0};
    {
        HostResolver resolver(
            [&] { wakes.fetch_add(1); },
            [&](const std::string& name) {
                lookups.fetch_add(1);
                return name == "data.test"
                           ? std::vector<std::string>{"192.0.2.1", "[2001:db8::1]", "192.0.2.2"}
                           : std::vector<std::string>{};
            },
            now);
        OK(resolver.entry("https://data.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 1; }));
        EQS(resolver.entry("https://data.test/a").resolve,
            "+data.test:443:192.0.2.1,[2001:db8::1],192.0.2.2");
        EQS(resolver.entry("https://DATA.test/b").resolve,
            "+data.test:443:[2001:db8::1],192.0.2.2,192.0.2.1");
        EQS(resolver.entry("http://data.test:8080/c").resolve,
            "+data.test:8080:192.0.2.2,192.0.2.1,[2001:db8::1]");
        EQ(lookups.load(), 1);

        // Warm transfers reuse libcurl's cached list.
        elapsed.store(1050);
        EQS(resolver.entry("https://data.test/d").resolve,
            "+data.test:443:192.0.2.1,[2001:db8::1],192.0.2.2");
        for (int index = 0; index < 7; ++index)
            EQS(resolver.entry("https://data.test/e").resolve, "");
        EQS(resolver.entry("https://data.test/f").resolve,
            "+data.test:443:[2001:db8::1],192.0.2.2,192.0.2.1");

        // A host without addresses goes back to libcurl, which reports the error.
        OK(resolver.entry("https://missing.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 2; }));
        const HostResolver::Entry missing = resolver.entry("https://missing.test/a");
        OK(!missing.waiting);
        EQS(missing.resolve, "-missing.test:443");

        const HostResolver::Entry literal = resolver.entry("http://127.0.0.1:1/object");
        OK(!literal.waiting);
        EQS(literal.resolve, "");
        EQ(lookups.load(), 2);
    }

    SECTION("Resolver refresh and ports");
    elapsed.store(0);
    wakes.store(0);
    std::atomic<int> answer{1};
    {
        HostResolver resolver([&] { ++wakes; },
                              [&](const std::string&) {
                                  if (answer.load() == 0)
                                      return std::vector<std::string>{};
                                  return std::vector<std::string>{"192.0.2." +
                                                                  std::to_string(answer.load())};
                              },
                              now);
        OK(resolver.entry("https://data.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 1; }));
        EQS(resolver.entry("https://data.test/a").resolve, "+data.test:443:192.0.2.1");
        EQS(resolver.entry("http://data.test:8080/a").resolve, "+data.test:8080:192.0.2.1");
        answer.store(2);
        elapsed.store(60001);
        OK(resolver.entry("https://data.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 2; }));
        EQS(resolver.entry("https://data.test/a").resolve, "+data.test:443:192.0.2.2");
        EQS(resolver.entry("http://data.test:8080/a").resolve, "+data.test:8080:192.0.2.2");
        answer.store(0);
        elapsed.store(120002);
        OK(resolver.entry("https://data.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 3; }));
        EQS(resolver.entry("https://data.test/a").resolve, "-data.test:443");
        EQS(resolver.entry("http://data.test:8080/a").resolve, "-data.test:8080");
        EQS(resolver.entry("https://data.test/a").resolve, "-data.test:443");
        EQ(wakes.load(), 3);
    }

    SECTION("Resolver capacity and eviction");
    elapsed.store(0);
    wakes.store(0);
    {
        HostResolver resolver(
            [&] { ++wakes; },
            [](const std::string&) { return std::vector<std::string>{"192.0.2.1"}; }, now);
        for (int index = 0; index < 256; ++index)
            OK(resolver.entry("https://host" + std::to_string(index) + ".test/a").waiting);
        OK(eventually([&] { return wakes.load() == 256; }));
        EQS(resolver.entry("https://overflow.test/a").resolve, "-overflow.test:443");
        elapsed.store(60001);
        OK(resolver.entry("https://overflow.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 257; }));
        EQS(resolver.entry("https://overflow.test/a").resolve, "+overflow.test:443:192.0.2.1");
        OK(resolver.entry("https://host0.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 258; }));
    }

    SECTION("Proxy environment bypasses local DNS");
    for (const char* name :
         {"http_proxy", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
        ScopedEnvironment proxy(name, "socks5h://127.0.0.1:1080");
        std::atomic<int> calls{0};
        HostResolver resolver({}, [&](const std::string&) {
            ++calls;
            return std::vector<std::string>{"192.0.2.1"};
        });
        const auto result = resolver.entry("https://private.test/a");
        OK(!result.waiting);
        OK(result.resolve.empty());
        resolver.stop();
        EQ(calls.load(), 0);
    }

    // Transfers stop waiting for a lookup that does not answer in time.
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    {
        HostResolver resolver({}, [released](const std::string&) {
            released.wait();
            return std::vector<std::string>{"192.0.2.9"};
        });
        OK(resolver.entry("https://slow.test/a").waiting);
        OK(eventually([&] { return !resolver.entry("https://slow.test/a").waiting; }));
        EQS(resolver.entry("https://slow.test/a").resolve, "-slow.test:443");
        release.set_value();
    }

    SECTION("Concurrent lookups share one result");
    {
        std::promise<void> unblock;
        auto gate = unblock.get_future().share();
        std::atomic<int> calls{0};
        wakes.store(0);
        HostResolver resolver([&] { ++wakes; },
                              [gate, &calls](const std::string&) {
                                  ++calls;
                                  gate.wait();
                                  return std::vector<std::string>{"192.0.2.1"};
                              });
        std::vector<std::future<HostResolver::Entry>> readers;
        for (int index = 0; index < 16; ++index)
            readers.push_back(std::async(std::launch::async,
                                         [&] { return resolver.entry("https://shared.test/a"); }));
        for (auto& reader : readers)
            OK(reader.get().waiting);
        unblock.set_value();
        OK(eventually([&] { return wakes.load() == 1; }));
        EQ(calls.load(), 1);
    }

    SECTION("Resolver exceptions fall back to libcurl");
    {
        wakes.store(0);
        HostResolver resolver([&] { ++wakes; },
                              [](const std::string&) -> std::vector<std::string> {
                                  throw std::runtime_error("lookup failed");
                              });
        OK(resolver.entry("https://failed.test/a").waiting);
        OK(eventually([&] { return wakes.load() == 1; }));
        EQS(resolver.entry("https://failed.test/a").resolve, "-failed.test:443");
    }

    SECTION("Shutdown with a lookup in flight");
    {
        std::promise<void> unblock;
        std::promise<void> entered;
        std::promise<void> finished;
        auto gate = unblock.get_future().share();
        auto started = entered.get_future();
        auto done = finished.get_future();
        std::atomic<int> notifications{0};
        {
            HostResolver resolver([&] { ++notifications; },
                                  [gate, &entered, &finished](const std::string&) {
                                      entered.set_value();
                                      gate.wait();
                                      finished.set_value();
                                      return std::vector<std::string>{"192.0.2.1"};
                                  });
            OK(resolver.entry("https://blocked.test/a").waiting);
            OK(started.wait_for(1s) == std::future_status::ready);
            resolver.stop();
            OK(!resolver.entry("https://another.test/a").waiting);
        }
        unblock.set_value();
        OK(done.wait_for(1s) == std::future_status::ready);
        EQ(notifications.load(), 0);
    }

    // After stop() no lookup starts and nothing is woken.
    std::atomic<int> late_wakes{0};
    HostResolver stopped([&] { late_wakes.fetch_add(1); },
                         [](const std::string&) { return std::vector<std::string>{"192.0.2.3"}; });
    stopped.stop();
    const HostResolver::Entry after_stop = stopped.entry("https://data.test/a");
    OK(!after_stop.waiting);
    EQS(after_stop.resolve, "");
    std::this_thread::sleep_for(20ms);
    EQ(late_wakes.load(), 0);
}

} // namespace karu::test
