#include "ht2mp/game/pattern.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace ht2mp::game {
namespace {

int HexValue(char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  if (value >= static_cast<unsigned char>('0') &&
      value <= static_cast<unsigned char>('9')) {
    return static_cast<int>(value - static_cast<unsigned char>('0'));
  }
  const auto lower = static_cast<unsigned char>(std::tolower(value));
  if (lower >= static_cast<unsigned char>('a') &&
      lower <= static_cast<unsigned char>('f')) {
    return static_cast<int>(lower - static_cast<unsigned char>('a') + 10U);
  }
  return -1;
}

}  // namespace

bool ParseMaskedPattern(std::string_view text, MaskedPattern& out,
                        std::string& error) {
  error.clear();
  MaskedPattern parsed;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    if (cursor == text.size()) {
      break;
    }
    const auto token_begin = cursor;
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) == 0) {
      ++cursor;
    }
    const auto token = text.substr(token_begin, cursor - token_begin);
    if (token == "?" || token == "??") {
      parsed.bytes.push_back(0U);
      parsed.mask.push_back(0U);
      continue;
    }
    if (token.size() != 2U) {
      error = "pattern token must be two hex digits or ??";
      return false;
    }
    const auto high = HexValue(token[0]);
    const auto low = HexValue(token[1]);
    if (high < 0 || low < 0) {
      error = "pattern contains a non-hex token";
      return false;
    }
    parsed.bytes.push_back(
        static_cast<std::uint8_t>((static_cast<unsigned int>(high) << 4U) |
                                  static_cast<unsigned int>(low)));
    parsed.mask.push_back(0xffU);
  }
  if (parsed.empty()) {
    error = "pattern is empty";
    return false;
  }
  if (std::none_of(parsed.mask.begin(), parsed.mask.end(),
                   [](std::uint8_t value) { return value != 0U; })) {
    error = "pattern cannot consist only of wildcards";
    return false;
  }
  out = std::move(parsed);
  return true;
}

std::vector<std::size_t> FindAllMasked(
    std::span<const std::uint8_t> haystack, const MaskedPattern& pattern,
    std::size_t maximum_results) {
  std::vector<std::size_t> results;
  if (pattern.empty() || pattern.bytes.size() != pattern.mask.size() ||
      pattern.size() > haystack.size() || maximum_results == 0U) {
    return results;
  }
  const auto first_required = static_cast<std::size_t>(std::distance(
      pattern.mask.begin(),
      std::find_if(pattern.mask.begin(), pattern.mask.end(),
                   [](std::uint8_t value) { return value != 0U; })));
  if (first_required == pattern.mask.size()) {
    return results;
  }

  const auto last_start = haystack.size() - pattern.size();
  for (std::size_t start = 0; start <= last_start; ++start) {
    if (haystack[start + first_required] != pattern.bytes[first_required]) {
      continue;
    }
    bool matched = true;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      if (pattern.mask[index] != 0U &&
          haystack[start + index] != pattern.bytes[index]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      results.push_back(start);
      if (results.size() >= maximum_results) {
        break;
      }
    }
  }
  return results;
}

}  // namespace ht2mp::game
