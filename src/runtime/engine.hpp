#ifndef KARU_ENGINE_HPP
#define KARU_ENGINE_HPP

#include "../config.hpp"
#include "../request_builder.hpp"
#include "batch.hpp"
#include "curl_types.hpp"
#include "transfer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace karu {

struct Retry {
    std::chrono::steady_clock::time_point due;
    std::unique_ptr<Transfer> transfer;
};

struct CurlShareState {
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;
};

class Engine {
  public:
    explicit Engine(ConfigSnapshot config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] std::unique_ptr<BatchCore> submit(std::span<const Request> requests,
                                                    karu_status& status);

    void cancel(BatchCore& batch);
    karu_status size_of(const Locator& locator, std::uint64_t& size);

  private:
    void io_loop();
    void file_loop();
    void start_transfer(std::unique_ptr<Transfer> transfer);
    void finish_transfer(std::unique_ptr<Transfer> transfer, karu_status status,
                         std::string detail = {});
    void deliver(Transfer& transfer, karu_status status, const std::string& detail);
    void discard_transfer(std::unique_ptr<Transfer> transfer) noexcept;
    void discard_cancelled_http();
    void stop_workers() noexcept;

    std::atomic<bool> stop_{false};
    std::atomic<bool> cancellation_pending_{false};
    std::thread io_thread_;
    std::vector<std::thread> file_workers_;

    RequestBuilder request_builder_;
    ClientOptions options_;
    CurlShareState share_state_;
    Multi multi_;
    Share share_;
    std::vector<Easy> easy_pool_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::unique_ptr<Transfer>> http_queue_;
    std::deque<std::unique_ptr<Transfer>> file_queue_;
    std::deque<std::unique_ptr<Transfer>> pending_;
    std::unordered_map<Transfer*, std::unique_ptr<Transfer>> active_;
    std::deque<Retry> retries_;
    std::size_t last_connection_limit_ = 0;
};

} // namespace karu

#endif
