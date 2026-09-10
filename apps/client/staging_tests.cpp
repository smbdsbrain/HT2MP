#include "edition.hpp"
#include "staging.hpp"

#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ht2mp::client::staging_testing {
bool recover_backup(const fs::path& runtime_root, const fs::path& incoming,
                    const fs::path& target, const fs::path& backup,
                    bool target_was_backed_up, std::string& error);
} // namespace ht2mp::client::staging_testing

namespace {

class TestContext final {
public:
  void expect(const bool condition, const std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
  }
  [[nodiscard]] int failures() const noexcept { return failures_; }

private:
  int failures_{};
};

class ScopedTree final {
public:
  explicit ScopedTree(fs::path path) : path_(std::move(path)) {}
  ~ScopedTree() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  ScopedTree(const ScopedTree&) = delete;
  ScopedTree& operator=(const ScopedTree&) = delete;

private:
  fs::path path_;
};

bool write_bytes(const fs::path& path, const std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

bool write_text(const fs::path& path, const std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(output);
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::byte> fake_i386_pe() {
  std::vector<std::byte> image(512U);
  IMAGE_DOS_HEADER dos{};
  dos.e_magic = IMAGE_DOS_SIGNATURE;
  dos.e_lfanew = 0x80;
  std::memcpy(image.data(), &dos, sizeof(dos));
  constexpr DWORD signature = IMAGE_NT_SIGNATURE;
  constexpr std::size_t signature_offset = 0x80U;
  constexpr std::size_t file_header_offset = 0x84U;
  static_assert(file_header_offset - signature_offset == sizeof(signature));
  std::memcpy(image.data() + signature_offset, &signature, sizeof(signature));
  IMAGE_FILE_HEADER header{};
  header.Machine = IMAGE_FILE_MACHINE_I386;
  header.NumberOfSections = 1U;
  std::memcpy(image.data() + file_header_offset, &header, sizeof(header));
  return image;
}

fs::path unique_temp(const wchar_t* prefix) {
  return fs::temp_directory_path() /
         (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(GetTickCount64()));
}

void test_validation(TestContext& test) {
  test.expect(ht2mp::client::valid_instance_id("") &&
                  ht2mp::client::valid_instance_id("p1") &&
                  ht2mp::client::valid_instance_id("player_a-2") &&
                  !ht2mp::client::valid_instance_id("PlayerA") &&
                  !ht2mp::client::valid_instance_id("-p1") &&
                  !ht2mp::client::valid_instance_id("p.1") &&
                  !ht2mp::client::valid_instance_id(
                      "abcdefghijklmnopqrstuvwxyz1234567"),
              "validate bounded lowercase instance identifiers");
  const auto source = unique_temp(L"ht2mp-stage-source-");
  ScopedTree source_cleanup(source);
  std::error_code ec;
  fs::create_directories(source, ec);
  test.expect(!ec, "create synthetic source directory");
  const auto image = fake_i386_pe();
  test.expect(write_bytes(source / L"king.exe", image), "write synthetic king.exe");

  const std::array<std::byte, 4> module_bytes{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  test.expect(write_bytes(source / L"ddraw.dll", module_bytes),
              "write synthetic protected module");

  ht2mp::windows::Sha256 digest{};
  std::string error;
  test.expect(ht2mp::windows::sha256_file(source / L"king.exe", digest, error),
              "hash synthetic king.exe");
  const auto hash = ht2mp::windows::sha256_hex(digest);
  test.expect(ht2mp::windows::sha256_file(source / L"ddraw.dll", digest, error),
              "hash synthetic protected module");
  const auto module_hash = ht2mp::windows::sha256_hex(digest);
  const std::array protected_modules{
      ht2mp::client::ProtectedModuleDescriptor{L"ddraw.dll", module_hash}};
  const auto profile = std::string("staging-test-") +
                       std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64());
  const ht2mp::client::EditionDescriptor edition{
      "test", profile, hash, protected_modules};

  const auto runtime = ht2mp::client::staged_game_directory(edition, error);
  test.expect(error.empty(), "resolve synthetic runtime directory");
  ScopedTree runtime_cleanup(runtime);

  ht2mp::client::StageResult staged;
  test.expect(ht2mp::client::stage_game(edition, source, staged, error),
              std::string("stage synthetic game: ") + error);
  test.expect(staged.destination == runtime, "stage destination is profile runtime");

  const auto named_runtime =
      ht2mp::client::staged_game_directory(edition, error, "p1");
  test.expect(error.empty() &&
                  named_runtime.filename() == fs::path(profile + "--p1"),
              "derive isolated profile--instance runtime path");
  ScopedTree named_runtime_cleanup(named_runtime);
  ht2mp::client::StageResult named_staged;
  test.expect(ht2mp::client::stage_game(edition, source, named_staged, error,
                                        "p1"),
              std::string("stage named runtime: ") + error);
  ht2mp::client::StageValidation named_validation;
  test.expect(ht2mp::client::validate_staged_game(
                  edition, named_runtime, named_validation, error, "p1"),
              std::string("validate named runtime: ") + error);
  auto named_manifest = read_text(named_runtime / L".ht2mp-stage.json");
  test.expect(named_manifest.find("\"schema\": 2") != std::string::npos &&
                  named_manifest.find("\"instance\": \"p1\"") !=
                      std::string::npos,
              "write schema-2 manifest with exact instance identity");
  const auto instance_position = named_manifest.find("\"p1\"");
  test.expect(instance_position != std::string::npos,
              "locate named manifest instance fixture");
  if (instance_position != std::string::npos) {
    named_manifest.replace(instance_position, 4U, "\"p2\"");
    test.expect(write_text(named_runtime / L".ht2mp-stage.json",
                           named_manifest),
                "alter named manifest instance");
    test.expect(!ht2mp::client::validate_staged_game(
                    edition, named_runtime, named_validation, error, "p1") &&
                    error.find("instance does not match") != std::string::npos,
                "reject schema-2 manifest for another instance");
  }
  test.expect(ht2mp::client::stage_game(edition, source, named_staged, error,
                                        "p1"),
              std::string("restore named runtime: ") + error);
  named_manifest = read_text(named_runtime / L".ht2mp-stage.json");
  const auto named_schema = named_manifest.find("\"schema\": 2");
  const auto named_instance_line =
      named_manifest.find("  \"instance\": \"p1\",\n");
  if (named_schema != std::string::npos &&
      named_instance_line != std::string::npos) {
    named_manifest.replace(named_schema, 11U, "\"schema\": 1");
    named_manifest.erase(named_instance_line,
                         std::string("  \"instance\": \"p1\",\n").size());
    test.expect(write_text(named_runtime / L".ht2mp-stage.json",
                           named_manifest),
                "write named schema-1 compatibility fixture");
    test.expect(!ht2mp::client::validate_staged_game(
                    edition, named_runtime, named_validation, error, "p1") &&
                    error.find("schema 1 is accepted only") !=
                        std::string::npos,
                "reject schema 1 for a named runtime");
  } else {
    test.expect(false, "locate schema-2 named manifest fields");
  }

  auto default_manifest = read_text(runtime / L".ht2mp-stage.json");
  const auto default_schema = default_manifest.find("\"schema\": 2");
  const auto default_instance_line =
      default_manifest.find("  \"instance\": \"\",\n");
  if (default_schema != std::string::npos &&
      default_instance_line != std::string::npos) {
    default_manifest.replace(default_schema, 11U, "\"schema\": 1");
    default_manifest.erase(default_instance_line,
                           std::string("  \"instance\": \"\",\n").size());
    test.expect(write_text(runtime / L".ht2mp-stage.json", default_manifest),
                "write default schema-1 compatibility fixture");
    ht2mp::client::StageValidation legacy_validation;
    test.expect(ht2mp::client::validate_staged_game(
                    edition, runtime, legacy_validation, error),
                std::string("accept schema 1 only on default runtime: ") +
                    error);
    test.expect(ht2mp::client::stage_game(edition, source, staged, error),
                std::string("restore default schema-2 stage: ") + error);
  } else {
    test.expect(false, "locate schema-2 default manifest fields");
  }

  ht2mp::client::StageValidation validation;
  test.expect(ht2mp::client::validate_staged_game(edition, runtime, validation, error),
              std::string("validate exact staged game: ") + error);
  test.expect(validation.runtime == fs::weakly_canonical(runtime),
              "validation returns canonical runtime");
  test.expect(validation.source == fs::weakly_canonical(source),
              "validation returns canonical source");

  const std::array<std::byte, 5> driver_bytes{
      std::byte{0x48}, std::byte{0x54}, std::byte{0x32}, std::byte{0x4d},
      std::byte{0x50}};
  const auto source_driver = source / L"DRIVER.pl1";
  test.expect(write_bytes(source_driver, driver_bytes), "write driver seed");
  ht2mp::client::DriverSeedResult seeded;
  test.expect(ht2mp::client::seed_staged_driver(
                  edition, source_driver, seeded, error),
              std::string("seed isolated driver: ") + error);
  std::string identity_error;
  test.expect(seeded.destination.filename() == L"DRIVER.pl1" &&
                  seeded.bytes_copied == driver_bytes.size(),
              "retain driver filename and report copied size");
  test.expect(ht2mp::windows::same_existing_file(
                  seeded.destination, runtime / L"DRIVER.pl1", identity_error) &&
                  identity_error.empty(),
              "return the committed staged driver identity");
  identity_error.clear();
  test.expect(!ht2mp::windows::same_existing_file(
                  source_driver, seeded.destination, identity_error) &&
                  identity_error.empty(),
              "seeded driver has distinct file identity");
  ht2mp::client::DriverSeedResult duplicate;
  test.expect(!ht2mp::client::seed_staged_driver(
                  edition, source_driver, duplicate, error) &&
                  error.find("refusing to overwrite") != std::string::npos,
              "refuse to overwrite an existing staged driver");
  fs::remove(seeded.destination, ec);
  test.expect(!ec, "remove seeded driver before remaining staging checks");

  const auto wrong_extension = source / L"DRIVER.bin";
  test.expect(write_bytes(wrong_extension, driver_bytes),
              "write wrong-extension driver fixture");
  test.expect(!ht2mp::client::seed_staged_driver(
                  edition, wrong_extension, duplicate, error) &&
                  error.find(".pl1 extension") != std::string::npos,
              "reject a non-pl1 driver seed");

  auto changed_module = module_bytes;
  changed_module.back() = std::byte{0x41};
  test.expect(write_bytes(runtime / L"ddraw.dll", changed_module),
              "alter staged protected module");
  test.expect(!ht2mp::client::validate_staged_game(
                  edition, runtime, validation, error) &&
                  error.find("protected module hash mismatch") !=
                      std::string::npos,
              "reject tampered staged protected module");
  ht2mp::client::StageResult module_restaged;
  test.expect(ht2mp::client::stage_game(edition, source, module_restaged, error),
              std::string("restore protected module stage: ") + error);

  fs::remove(runtime / L"ddraw.dll", ec);
  test.expect(!ec, "remove runtime module before hard-link identity test");
  const auto module_hard_linked = CreateHardLinkW(
      (runtime / L"ddraw.dll").c_str(), (source / L"ddraw.dll").c_str(),
      nullptr) != FALSE;
  test.expect(module_hard_linked, "create source/runtime module hard link");
  if (module_hard_linked) {
    test.expect(!ht2mp::client::validate_staged_game(
                    edition, runtime, validation, error) &&
                    error.find("aliases the source") != std::string::npos,
                "reject protected module with source file identity");
  }
  test.expect(ht2mp::client::stage_game(edition, source, module_restaged, error),
              std::string("restore stage after module identity test: ") + error);

  const auto wrong_runtime = runtime.parent_path() / L"different-profile";
  test.expect(!ht2mp::client::validate_staged_game(
                  edition, wrong_runtime, validation, error),
              "reject runtime path not fixed to selected profile");

  const auto manifest = runtime / L".ht2mp-stage.json";
  const auto invalid_manifest =
      std::string("{\"schema\":1,\"profile\":\"") + profile +
      "\",\"profile\":\"duplicate\",\"king_sha256\":\"" + hash +
      "\",\"source\":\"x\",\"save_policy\":\"excluded\"}";
  test.expect(write_text(manifest, invalid_manifest), "write invalid manifest");
  test.expect(!ht2mp::client::validate_staged_game(
                  edition, runtime, validation, error) &&
                  error.find("duplicate 'profile'") != std::string::npos,
              "reject duplicate security-sensitive manifest keys");

  ht2mp::client::StageResult restaged;
  test.expect(ht2mp::client::stage_game(edition, source, restaged, error),
              std::string("restore test stage: ") + error);
  fs::remove(runtime / L"king.exe", ec);
  test.expect(!ec, "remove runtime king before hard-link identity test");
  const auto hard_linked = CreateHardLinkW((runtime / L"king.exe").c_str(),
                                           (source / L"king.exe").c_str(),
                                           nullptr) != FALSE;
  test.expect(hard_linked, "create source/runtime king hard link");
  if (hard_linked) {
    test.expect(!ht2mp::client::validate_staged_game(
                    edition, runtime, validation, error) &&
                    error.find("aliases the source") != std::string::npos,
                "reject staged king.exe with source file identity");
  }

  test.expect(ht2mp::client::stage_game(edition, source, restaged, error),
              std::string("restore stage after identity test: ") + error);
  const auto external = source / L"external.bin";
  const std::array<std::byte, 1> marker{std::byte{0x42}};
  test.expect(write_bytes(external, marker), "write symlink target");
  const auto link = runtime / L"runtime-link.bin";
  const auto symlink_created = CreateSymbolicLinkW(
      link.c_str(), external.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  if (symlink_created) {
    test.expect(!ht2mp::client::validate_staged_game(
                    edition, runtime, validation, error) &&
                    error.find("reparse point/junction") != std::string::npos,
                "reject a reparse point anywhere in staged runtime");
    fs::remove(link, ec);
    test.expect(!ec, "remove runtime reparse test fixture");
  } else {
    std::cout << "SKIP: reparse test (CreateSymbolicLinkW unavailable: "
              << GetLastError() << ")\n";
  }

  auto changed_image = image;
  changed_image.back() = std::byte{0x7f};
  test.expect(write_bytes(source / L"king.exe", changed_image),
              "alter synthetic source king.exe");
  test.expect(!ht2mp::client::validate_staged_game(
                  edition, runtime, validation, error) &&
                  error.find("Source installation no longer matches") !=
                      std::string::npos,
              "reject runtime when manifest source no longer has exact hash");
}

void test_failed_recovery_diagnostic(TestContext& test) {
  const auto root = unique_temp(L"ht2mp-stage-recovery-");
  ScopedTree cleanup(root);
  const auto incoming = root / L".incoming";
  const auto target = root / L"profile";
  const auto backup = root / L".backup";
  std::error_code ec;
  fs::create_directories(incoming, ec);
  fs::create_directories(target, ec);
  fs::create_directories(backup, ec);
  test.expect(!ec, "create failed-recovery fixture");

  std::string error;
  test.expect(!ht2mp::client::staging_testing::recover_backup(
                  root, incoming, target, backup, true, error),
              "report failed backup recovery");
  test.expect(error.find("AUTOMATIC RECOVERY FAILED") != std::string::npos,
              "failed recovery is unmistakable");
  test.expect(error.find(backup.string()) != std::string::npos,
              "failed recovery reports preserved backup path");
  test.expect(fs::is_directory(backup), "failed recovery leaves backup recoverable");

  fs::remove_all(target, ec);
  error.clear();
  test.expect(ht2mp::client::staging_testing::recover_backup(
                  root, incoming, target, backup, true, error),
              std::string("restore backup successfully: ") + error);
  test.expect(fs::is_directory(target) && !fs::exists(backup),
              "successful recovery restores old runtime directory");
}

} // namespace

int main() {
  TestContext test;
  test_validation(test);
  test_failed_recovery_diagnostic(test);
  if (test.failures() == 0) {
    std::cout << "All staging safety tests passed\n";
  }
  return test.failures() == 0 ? 0 : 1;
}
