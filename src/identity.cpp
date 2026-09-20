// Collective Traffic Fabric - identity implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/identity.hpp"

#include <atomic>
#include <chrono>
#include <random>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ctf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_hex64(std::string& out, std::uint64_t value) {
  char buffer[16];
  for (int index = 15; index >= 0; --index) {
    buffer[index] = kHexDigits[value & 0xF];
    value >>= 4;
  }
  out.append(buffer, 16);
}

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  return -1;
}

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::atomic<std::uint64_t> g_mint_counter{0};

std::uint64_t entropy_seed() noexcept {
  std::random_device device;
  std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  seed ^= static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#if defined(_WIN32)
  seed ^= static_cast<std::uint64_t>(_getpid()) << 17;
#else
  seed ^= static_cast<std::uint64_t>(::getpid()) << 17;
#endif
  return seed;
}

}  // namespace

std::string Identity::to_string() const {
  std::string out;
  out.reserve(32);
  append_hex64(out, hi_);
  append_hex64(out, lo_);
  return out;
}

bool Identity::parse(std::string_view text, Identity& out) noexcept {
  if (text.size() != 32) return false;
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t index = 0; index < 32; ++index) {
    const int nibble = hex_value(text[index]);
    if (nibble < 0) return false;
    if (index < 16) {
      hi = (hi << 4) | static_cast<std::uint64_t>(nibble);
    } else {
      lo = (lo << 4) | static_cast<std::uint64_t>(nibble);
    }
  }
  out = Identity(hi, lo);
  return true;
}

Identity IdentityMinter::next() noexcept {
  // Both outputs come from the mixed state (never from an unmixed modulus of
  // it), so a one bit difference in the seed cannot produce a matching pair.
  state_ = mix64(state_);
  const std::uint64_t hi = mix64(state_);
  const std::uint64_t lo = mix64(state_ ^ 0xA5A5A5A5A5A5A5A5ull ^ hi);
  return Identity(hi, lo);
}

Identity mint_identity() noexcept {
  static const std::uint64_t entropy = entropy_seed();
  const std::uint64_t counter = g_mint_counter.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t hi = mix64(entropy ^ (counter * 0x9E3779B97F4A7C15ull));
  const std::uint64_t lo = mix64(entropy + 0x165667B19E3779F9ull + counter);
  Identity minted(hi, lo);
  if (minted.is_none()) {
    // Astronomically unlikely; the contract still forbids returning "none".
    return Identity(0x0000000000000001ull, 0x0000000000000001ull);
  }
  return minted;
}

std::string Sequence::to_string() const {
  std::string out = "seq:";
  std::string digits;
  std::uint64_t value = value_;
  if (value == 0) {
    digits = "0";
  }
  while (value != 0) {
    digits.insert(digits.begin(), static_cast<char>('0' + (value % 10)));
    value /= 10;
  }
  out += digits;
  return out;
}

}  // namespace ctf
