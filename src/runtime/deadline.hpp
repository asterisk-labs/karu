#ifndef KARU_DEADLINE_HPP
#define KARU_DEADLINE_HPP

#include <algorithm>
#include <chrono>
#include <limits>

namespace karu {

class RequestDeadline {
  public:
    void start(long timeout_seconds) noexcept {
        if (started_)
            return;
        started_ = true;
        if (timeout_seconds > 0) {
            expires_ = Clock::now() + std::chrono::seconds(timeout_seconds);
            bounded_ = true;
        }
    }

    [[nodiscard]] bool expired() const noexcept { return bounded_ && Clock::now() >= expires_; }

    [[nodiscard]] bool can_wait_for(std::chrono::milliseconds delay) const noexcept {
        return !bounded_ || delay < expires_ - Clock::now();
    }

    // libcurl uses zero to mean no timeout. Bounded callers check expired()
    // first, so one millisecond is the smallest useful value here.
    [[nodiscard]] long curl_timeout_ms() const noexcept {
        if (!bounded_)
            return 0;
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(expires_ - Clock::now()).count();
        const auto maximum = static_cast<long long>(std::numeric_limits<long>::max());
        return static_cast<long>(std::clamp(remaining, 1LL, maximum));
    }

  private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point expires_{};
    bool started_ = false;
    bool bounded_ = false;
};

} // namespace karu

#endif
