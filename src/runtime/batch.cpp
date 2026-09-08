#include "batch.hpp"

#include <chrono>
#include <utility>

karu_batch::karu_batch() = default;

karu_status karu_batch::next(karu::Completion& out, int timeout_ms) {
    std::unique_lock lock(mutex);
    const auto can_return = [this] {
        return !ready.empty() || pending_parts == 0 || cancelled.load(std::memory_order_acquire);
    };

    if (timeout_ms < 0) {
        cv.wait(lock, can_return);
    } else if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), can_return)) {
        return KARU_TIMEOUT;
    }

    if (ready.empty())
        return KARU_END;
    out = std::move(ready.front());
    ready.pop_front();
    return KARU_OK;
}

void karu_batch::push(karu::Completion completion) {
    std::lock_guard lock(mutex);
    ready.push_back(std::move(completion));
    if (pending_parts > 0)
        --pending_parts;
    cv.notify_all();
}

void karu_batch::transfer_finished() {
    std::lock_guard lock(mutex);
    if (live_transfers > 0)
        --live_transfers;
    // Keep the notification inside the lock. A waiter that observes zero may
    // destroy the condition variable as soon as it reacquires the mutex.
    cv.notify_all();
}

bool karu_batch::is_cancelled() const {
    return cancelled.load(std::memory_order_acquire);
}
