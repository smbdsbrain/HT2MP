#include "ht2mp/game/pe_image.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace ht2mp::game {
namespace {

bool ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset,
             std::uint16_t& value) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < 2U) {
    return false;
  }
  value = static_cast<std::uint16_t>(bytes[offset]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U])
                                     << 8U);
  return true;
}

bool ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset,
             std::uint32_t& value) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < 4U) {
    return false;
  }
  value = static_cast<std::uint32_t>(bytes[offset]) |
          (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
          (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
          (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
  return true;
}

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t RotateRight(std::uint32_t value,
                                    unsigned int amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

void Sha256Compress(std::array<std::uint32_t, 8>& state,
                    const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16U; ++index) {
    const auto offset = index * 4U;
    words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                   (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                   static_cast<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16U; index < words.size(); ++index) {
    const auto x = words[index - 15U];
    const auto y = words[index - 2U];
    const auto sigma0 = RotateRight(x, 7U) ^ RotateRight(x, 18U) ^ (x >> 3U);
    const auto sigma1 = RotateRight(y, 17U) ^ RotateRight(y, 19U) ^ (y >> 10U);
    words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];

  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto big_sigma1 =
        RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
    const auto choose = (e & f) ^ ((~e) & g);
    const auto temporary1 = h + big_sigma1 + choose +
                            kSha256RoundConstants[index] + words[index];
    const auto big_sigma0 =
        RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary2 = big_sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

bool PeImage::Load(const std::filesystem::path& path, PeImage& out,
                   std::string& error) {
  error.clear();
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    error = "cannot open executable";
    return false;
  }
  const auto end = stream.tellg();
  if (end < 0) {
    error = "cannot determine executable size";
    return false;
  }
  const auto unsigned_size = static_cast<std::uint64_t>(end);
  if (unsigned_size < 0x100U ||
      unsigned_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    error = "executable has an invalid size";
    return false;
  }

  PeImage candidate;
  candidate.path_ = path;
  candidate.bytes_.resize(static_cast<std::size_t>(unsigned_size));
  stream.seekg(0, std::ios::beg);
  stream.read(reinterpret_cast<char*>(candidate.bytes_.data()),
              static_cast<std::streamsize>(candidate.bytes_.size()));
  if (!stream) {
    error = "cannot read the complete executable";
    return false;
  }

  const auto bytes = std::span<const std::uint8_t>(candidate.bytes_);
  if (bytes[0] != static_cast<std::uint8_t>('M') ||
      bytes[1] != static_cast<std::uint8_t>('Z')) {
    error = "missing DOS MZ signature";
    return false;
  }

  std::uint32_t pe_offset = 0;
  if (!ReadU32(bytes, 0x3cU, pe_offset) || pe_offset > bytes.size() ||
      bytes.size() - pe_offset < 24U) {
    error = "invalid PE header offset";
    return false;
  }
  const auto pe = static_cast<std::size_t>(pe_offset);
  if (bytes[pe] != static_cast<std::uint8_t>('P') ||
      bytes[pe + 1U] != static_cast<std::uint8_t>('E') || bytes[pe + 2U] != 0U ||
      bytes[pe + 3U] != 0U) {
    error = "missing PE signature";
    return false;
  }

  const auto coff = pe + 4U;
  std::uint16_t optional_header_size = 0;
  if (!ReadU16(bytes, coff, candidate.metadata_.machine) ||
      !ReadU16(bytes, coff + 2U, candidate.metadata_.number_of_sections) ||
      !ReadU32(bytes, coff + 4U, candidate.metadata_.timestamp) ||
      !ReadU16(bytes, coff + 16U, optional_header_size) ||
      !ReadU16(bytes, coff + 18U, candidate.metadata_.characteristics)) {
    error = "truncated COFF header";
    return false;
  }
  if (candidate.metadata_.number_of_sections == 0U ||
      candidate.metadata_.number_of_sections > 96U || optional_header_size < 96U) {
    error = "unsupported PE section/optional-header count";
    return false;
  }

  const auto optional = coff + 20U;
  if (optional > bytes.size() || bytes.size() - optional < optional_header_size) {
    error = "truncated optional header";
    return false;
  }
  candidate.metadata_.linker_major = bytes[optional + 2U];
  candidate.metadata_.linker_minor = bytes[optional + 3U];
  if (!ReadU16(bytes, optional, candidate.metadata_.optional_header_magic) ||
      !ReadU32(bytes, optional + 16U, candidate.metadata_.entry_point_rva) ||
      !ReadU32(bytes, optional + 28U, candidate.metadata_.image_base) ||
      !ReadU32(bytes, optional + 32U, candidate.metadata_.section_alignment) ||
      !ReadU32(bytes, optional + 36U, candidate.metadata_.file_alignment) ||
      !ReadU32(bytes, optional + 56U, candidate.metadata_.size_of_image) ||
      !ReadU32(bytes, optional + 60U, candidate.metadata_.size_of_headers) ||
      !ReadU32(bytes, optional + 64U, candidate.metadata_.checksum) ||
      !ReadU16(bytes, optional + 68U, candidate.metadata_.subsystem) ||
      !ReadU16(bytes, optional + 70U, candidate.metadata_.dll_characteristics)) {
    error = "truncated PE32 optional header";
    return false;
  }
  if (candidate.metadata_.optional_header_magic != 0x10bU) {
    error = "only PE32 images are supported";
    return false;
  }

  const auto section_table = optional + optional_header_size;
  const auto section_count =
      static_cast<std::size_t>(candidate.metadata_.number_of_sections);
  if (section_table > bytes.size() ||
      section_count > (bytes.size() - section_table) / 40U) {
    error = "truncated section table";
    return false;
  }

  candidate.sections_.reserve(section_count);
  for (std::size_t index = 0; index < section_count; ++index) {
    const auto offset = section_table + index * 40U;
    std::size_t name_length = 0;
    while (name_length < 8U && bytes[offset + name_length] != 0U) {
      ++name_length;
    }
    PeSection section;
    section.name.assign(reinterpret_cast<const char*>(bytes.data() + offset), name_length);
    if (!ReadU32(bytes, offset + 8U, section.virtual_size) ||
        !ReadU32(bytes, offset + 12U, section.virtual_address) ||
        !ReadU32(bytes, offset + 16U, section.raw_size) ||
        !ReadU32(bytes, offset + 20U, section.raw_offset) ||
        !ReadU32(bytes, offset + 36U, section.characteristics)) {
      error = "truncated section record";
      return false;
    }
    const auto raw_offset = static_cast<std::size_t>(section.raw_offset);
    const auto raw_size = static_cast<std::size_t>(section.raw_size);
    if (raw_offset > bytes.size() || raw_size > bytes.size() - raw_offset) {
      error = "section raw range is outside the executable";
      return false;
    }
    if (section.virtual_address > candidate.metadata_.size_of_image ||
        section.virtual_size >
            candidate.metadata_.size_of_image - section.virtual_address) {
      error = "section virtual range is outside SizeOfImage";
      return false;
    }
    candidate.sections_.push_back(std::move(section));
  }

  out = std::move(candidate);
  return true;
}

const PeSection* PeImage::FindSection(std::string_view name) const noexcept {
  const auto found = std::find_if(
      sections_.begin(), sections_.end(),
      [name](const PeSection& section) { return section.name == name; });
  return found == sections_.end() ? nullptr : &*found;
}

const PeSection* PeImage::FindSectionForRva(std::uint32_t rva) const noexcept {
  for (const auto& section : sections_) {
    const auto mapped_size = std::max(section.virtual_size, section.raw_size);
    if (rva >= section.virtual_address &&
        rva - section.virtual_address < mapped_size) {
      return &section;
    }
  }
  return nullptr;
}

std::optional<std::span<const std::uint8_t>> PeImage::RawSection(
    std::string_view name) const noexcept {
  const auto* section = FindSection(name);
  if (section == nullptr) {
    return std::nullopt;
  }
  return std::span<const std::uint8_t>(bytes_).subspan(section->raw_offset,
                                                       section->raw_size);
}

std::optional<std::size_t> PeImage::RvaToFileOffset(
    std::uint32_t rva) const noexcept {
  if (rva < metadata_.size_of_headers && rva < bytes_.size()) {
    return static_cast<std::size_t>(rva);
  }
  for (const auto& section : sections_) {
    if (rva < section.virtual_address) {
      continue;
    }
    const auto delta = rva - section.virtual_address;
    if (delta >= section.raw_size) {
      continue;
    }
    const auto result = static_cast<std::size_t>(section.raw_offset) + delta;
    if (result < bytes_.size()) {
      return result;
    }
  }
  return std::nullopt;
}

std::string Sha256Hex(std::span<const std::uint8_t> data) {
  std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                     0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                     0x1f83d9abU, 0x5be0cd19U};
  const auto full_blocks = data.size() / 64U;
  for (std::size_t block = 0; block < full_blocks; ++block) {
    Sha256Compress(state, data.data() + block * 64U);
  }

  std::array<std::uint8_t, 128> tail{};
  const auto remaining = data.size() % 64U;
  if (remaining != 0U) {
    std::memcpy(tail.data(), data.data() + full_blocks * 64U, remaining);
  }
  tail[remaining] = 0x80U;
  const auto tail_blocks = remaining < 56U ? 1U : 2U;
  const auto bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
  const auto length_offset = tail_blocks * 64U - 8U;
  for (std::size_t byte = 0; byte < 8U; ++byte) {
    tail[length_offset + byte] = static_cast<std::uint8_t>(
        bit_length >> static_cast<unsigned int>((7U - byte) * 8U));
  }
  for (std::size_t block = 0; block < tail_blocks; ++block) {
    Sha256Compress(state, tail.data() + block * 64U);
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : state) {
    output << std::setw(8) << word;
  }
  return output.str();
}

}  // namespace ht2mp::game
