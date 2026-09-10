#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ht2mp::game {

struct MaskedPattern {
  std::vector<std::uint8_t> bytes;
  // 0 means wildcard, 0xff means the corresponding byte must match.
  std::vector<std::uint8_t> mask;

  [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes.size(); }
};

// Parses IDA-style patterns such as "55 8B EC ?? ?? 6A FF". A wildcard must
// occupy an entire byte ("?" and "??" are both accepted).
bool ParseMaskedPattern(std::string_view text, MaskedPattern& out,
                        std::string& error);

[[nodiscard]] std::vector<std::size_t> FindAllMasked(
    std::span<const std::uint8_t> haystack, const MaskedPattern& pattern,
    std::size_t maximum_results = static_cast<std::size_t>(-1));

}  // namespace ht2mp::game
