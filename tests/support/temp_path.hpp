// Collective Traffic Fabric - unique temporary paths for the test suites.
// Copyright 2026 Summon Software Labs.
//
// Suites are expected to be runnable concurrently (ctest does exactly that), so
// a temporary file name must be unique across processes as well as within one.
// The name combines the process id, a per-process counter and the requested tag.
#ifndef CTF_TESTS_TEMP_PATH_HPP
#define CTF_TESTS_TEMP_PATH_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ctf::test {

[[nodiscard]] inline unsigned long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

// Builds a path in the current working directory.  A snapshot written by one
// suite must never be visible to another running suite.
[[nodiscard]] inline std::string unique_temp_path(const char* prefix, const std::string& tag) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t index = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::string(prefix) + tag + "-" + std::to_string(current_process_id()) + "-" +
         std::to_string(index) + ".bin";
}

}  // namespace ctf::test

#endif  // CTF_TESTS_TEMP_PATH_HPP
