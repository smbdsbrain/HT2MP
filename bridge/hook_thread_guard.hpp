#pragma once

#include <atomic>
#include <cstdint>

namespace ht2mp::bridge {

struct HookThreadSnapshot final {
  std::uint32_t thread_id{};
  std::uint32_t mismatches{};
};

// Claims the first non-zero thread identifier and rejects every different
// identifier thereafter. Observe is safe in a detour and never allocates or
// waits. Reset is initialization-only and must run before the hook is enabled.
class HookThreadGuard final {
 public:
  [[nodiscard]] bool Observe(std::uint32_t thread_id) noexcept {
    if (thread_id == 0U) {
      mismatches_.fetch_add(1U, std::memory_order_relaxed);
      return false;
    }
    std::uint32_t expected{};
    if (thread_id_.compare_exchange_strong(
            expected, thread_id, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
    if (expected == thread_id) return true;
    mismatches_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }

  [[nodiscard]] HookThreadSnapshot Snapshot() const noexcept {
    return {thread_id_.load(std::memory_order_acquire),
            mismatches_.load(std::memory_order_relaxed)};
  }

  void Reset() noexcept {
    mismatches_.store(0U, std::memory_order_relaxed);
    thread_id_.store(0U, std::memory_order_release);
  }

 private:
  std::atomic<std::uint32_t> thread_id_{};
  std::atomic<std::uint32_t> mismatches_{};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

}  // namespace ht2mp::bridge
