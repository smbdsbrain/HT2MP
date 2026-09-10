#include "ht2mp/ipc/authentication.hpp"

#include <algorithm>

namespace ht2mp::ipc {

AuthenticationError authenticate_bridge_hello(
    const BridgeHelloV1& hello,
    const BridgeExpectation& expected) noexcept {
  if (hello.abi_version != kBootstrapAbiVersion) {
    return AuthenticationError::abi_mismatch;
  }
  if (hello.run_id != expected.run_id || expected.run_id == 0U) {
    return AuthenticationError::run_id_mismatch;
  }
  if (hello.nonce != expected.nonce) {
    return AuthenticationError::nonce_mismatch;
  }
  const auto profile_end =
      std::find(hello.profile_id.begin(), hello.profile_id.end(), '\0');
  if (profile_end == hello.profile_id.end() ||
      profile_end == hello.profile_id.begin()) {
    return AuthenticationError::malformed_profile;
  }
  const std::string_view actual(
      hello.profile_id.data(),
      static_cast<std::size_t>(profile_end - hello.profile_id.begin()));
  return actual == expected.profile_id ? AuthenticationError::none
                                       : AuthenticationError::profile_mismatch;
}

std::string_view to_string(const AuthenticationError error) noexcept {
  switch (error) {
  case AuthenticationError::none: return "none";
  case AuthenticationError::abi_mismatch: return "ABI mismatch";
  case AuthenticationError::run_id_mismatch: return "run ID mismatch";
  case AuthenticationError::nonce_mismatch: return "nonce mismatch";
  case AuthenticationError::malformed_profile: return "malformed profile ID";
  case AuthenticationError::profile_mismatch: return "profile mismatch";
  }
  return "unknown authentication error";
}

} // namespace ht2mp::ipc
