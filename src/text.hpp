#ifndef KARU_TEXT_HPP
#define KARU_TEXT_HPP

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

namespace karu {

inline void append_text(std::string& output, std::string_view value) {
    output.append(value);
}

template <std::integral Integer>
void append_text(std::string& output, Integer value) {
    output.append(std::to_string(value));
}

template <class... Parts>
[[nodiscard]] std::string concat(Parts&&... parts) {
    std::string output;
    (append_text(output, std::forward<Parts>(parts)), ...);
    return output;
}

} // namespace karu

#endif
