#ifndef KARU_BATCH_HPP
#define KARU_BATCH_HPP

#include "../buffer.hpp"
#include "karu/karu.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace karu {

class Engine;

struct Completion {
    void* tag = nullptr;
    karu_status status = KARU_OK;
    std::uint64_t got = 0;
    void* buffer = nullptr;
    OwnedBuffer owned_buffer;
    std::string detail;

    [[nodiscard]] void* release_buffer() noexcept {
        return owned_buffer ? owned_buffer.release() : buffer;
    }
};

} // namespace karu

struct karu_batch {
    karu_batch();

    karu_batch(const karu_batch&) = delete;
    karu_batch& operator=(const karu_batch&) = delete;

    karu_status next(karu::Completion& out, int timeout_ms);
    void push(karu::Completion completion);
    void transfer_finished();
    [[nodiscard]] bool is_cancelled() const;

    std::shared_ptr<karu::Engine> owner;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<karu::Completion> ready;
    std::size_t pending_parts = 0;
    std::size_t live_transfers = 0;
    std::atomic<bool> cancelled{false};
};

namespace karu {

using BatchCore = ::karu_batch;

} // namespace karu

#endif
