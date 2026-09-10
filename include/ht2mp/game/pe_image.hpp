#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ht2mp::game {

struct PeSection {
  std::string name;
  std::uint32_t virtual_size{};
  std::uint32_t virtual_address{};
  std::uint32_t raw_size{};
  std::uint32_t raw_offset{};
  std::uint32_t characteristics{};
};

struct PeMetadata {
  std::uint16_t machine{};
  std::uint16_t number_of_sections{};
  std::uint32_t timestamp{};
  std::uint16_t characteristics{};
  std::uint16_t optional_header_magic{};
  std::uint8_t linker_major{};
  std::uint8_t linker_minor{};
  std::uint32_t entry_point_rva{};
  std::uint32_t image_base{};
  std::uint32_t section_alignment{};
  std::uint32_t file_alignment{};
  std::uint32_t size_of_image{};
  std::uint32_t size_of_headers{};
  std::uint32_t checksum{};
  std::uint16_t subsystem{};
  std::uint16_t dll_characteristics{};
};

// A deliberately small PE32 reader. It does not map or execute the image and is
// therefore safe to use in the launcher and in offline tooling.
class PeImage {
 public:
  static bool Load(const std::filesystem::path& path, PeImage& out,
                   std::string& error);

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }
  [[nodiscard]] const PeMetadata& metadata() const noexcept { return metadata_; }
  [[nodiscard]] const std::vector<PeSection>& sections() const noexcept {
    return sections_;
  }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] std::uint64_t file_size() const noexcept { return bytes_.size(); }

  [[nodiscard]] const PeSection* FindSection(std::string_view name) const noexcept;
  [[nodiscard]] const PeSection* FindSectionForRva(std::uint32_t rva) const noexcept;
  [[nodiscard]] std::optional<std::span<const std::uint8_t>> RawSection(
      std::string_view name) const noexcept;
  [[nodiscard]] std::optional<std::size_t> RvaToFileOffset(
      std::uint32_t rva) const noexcept;

 private:
  std::filesystem::path path_;
  PeMetadata metadata_{};
  std::vector<PeSection> sections_;
  std::vector<std::uint8_t> bytes_;
};

[[nodiscard]] std::string Sha256Hex(std::span<const std::uint8_t> data);

}  // namespace ht2mp::game
