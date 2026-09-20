// Collective Traffic Fabric - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
//
// The framework is deliberately small and dependency free.  It offers exactly
// what this repository's proof obligations need: named cases, rich failure
// context, deterministic ordering, reproduction seeds for randomized cases, and
// a non-zero exit status on any failure.
#ifndef CTF_TESTS_TEST_FRAMEWORK_HPP
#define CTF_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ctf/error.hpp"
#include "ctf/identity.hpp"

// A few of the repository's own types have no canonical operator<<.  Rendering
// them through their documented text form keeps every assertion message
// readable without adding stream operators to the library itself.
[[nodiscard]] inline std::string to_string(const ::std::string& value) { return value; }

[[nodiscard]] inline std::string to_string(const char* value) { return std::string(value); }

namespace ctf::test {

class AssertionFailure : public std::exception {
 public:
  explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct Case {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(std::string suite, std::string name, std::function<void()> body) {
    cases_.push_back(Case{std::move(suite), std::move(name), std::move(body)});
  }

  [[nodiscard]] const std::vector<Case>& cases() const noexcept { return cases_; }

 private:
  std::vector<Case> cases_;
};

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) {
    Registry::instance().add(suite, name, function);
  }
};

// Rendering policy for assertion values, in order of preference:
//   1. an explicit operator<< for the type, if one exists;
//   2. to_string(value), for the repository's own enumeration and value types;
//   3. a textual placeholder naming the type, so a failure never fails to
//      compile merely because a value is not printable.
template <typename T, typename = void>
struct HasStreamOperator : std::false_type {};

template <typename T>
struct HasStreamOperator<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasToTextFunction : std::false_type {};

template <typename T>
struct HasToTextFunction<T, std::void_t<decltype(to_string(std::declval<const T&>()))>> : std::true_type {};

template <typename T>
void render_value(std::ostream& out, const T& value) {
  if constexpr (HasStreamOperator<T>::value) {
    out << value;
  } else if constexpr (HasToTextFunction<T>::value) {
    out << to_string(value);
  } else {
    out << "<value of an unprintable type>";
  }
}

// Accumulates the message for one assertion so that failures carry the exact
// expression, file and line.
class AssertionStream {
 public:
  AssertionStream(const char* expression, const char* file, int line) {
    stream_ << file << ":" << line << ": assertion failed: " << expression;
  }
  template <typename T>
  AssertionStream& operator<<(const T& value) {
    stream_ << " | ";
    render_value(stream_, value);
    return *this;
  }
  [[noreturn]] void fail() { throw AssertionFailure(stream_.str()); }

 private:
  std::ostringstream stream_;
};

struct SeedScope {
  std::uint64_t seed;
  explicit SeedScope(std::uint64_t value) : seed(value) {}
  [[nodiscard]] std::string to_string() const { return std::to_string(seed); }
};

// Every randomized case receives a seed.  The seed is printed before the case
// runs so a failing run can be reproduced exactly with CTF_TEST_SEED.
// Reads an environment variable without the platform deprecation warnings that
// surround getenv, so this header stays clean under /W4 /WX.
[[nodiscard]] inline std::string read_environment(const char* name) {
  std::string value;
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
    value.assign(buffer);
    std::free(buffer);
  }
#else
  const char* raw = std::getenv(name);
  if (raw != nullptr) value.assign(raw);
#endif
  return value;
}

[[nodiscard]] inline std::uint64_t resolve_seed(const char* case_name) {
  const std::string environment = read_environment("CTF_TEST_SEED");
  if (!environment.empty()) {
    return std::strtoull(environment.c_str(), nullptr, 10);
  }
  std::uint64_t hash = 1469598103934665603ull;
  for (const char* cursor = case_name; *cursor != '\0'; ++cursor) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*cursor));
    hash *= 1099511628211ull;
  }
  return hash;
}

[[nodiscard]] inline bool filter_matches(const char* filter, const Case& entry) {
  if (filter == nullptr || *filter == '\0') return true;
  const std::string needle(filter);
  return entry.suite.find(needle) != std::string::npos || entry.name.find(needle) != std::string::npos;
}

inline int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : "";
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const Case& entry : Registry::instance().cases()) {
    if (!filter_matches(filter, entry)) continue;
    std::cout << "[ RUN  ] " << entry.suite << "." << entry.name << std::endl;
    try {
      entry.body();
      ++passed;
      std::cout << "[  OK  ] " << entry.suite << "." << entry.name << std::endl;
    } catch (const AssertionFailure& failure) {
      ++failed;
      std::cout << "[ FAIL ] " << entry.suite << "." << entry.name << "\n         " << failure.what()
                << std::endl;
      failures.push_back(entry.suite + "." + entry.name + ": " + failure.what());
    } catch (const std::exception& error) {
      ++failed;
      std::cout << "[ FAIL ] " << entry.suite << "." << entry.name
                << "\n         unexpected exception: " << error.what() << std::endl;
      failures.push_back(entry.suite + "." + entry.name + std::string(": exception ") + error.what());
    } catch (...) {
      ++failed;
      std::cout << "[ FAIL ] " << entry.suite << "." << entry.name << "\n         unknown exception"
                << std::endl;
      failures.push_back(entry.suite + "." + entry.name + ": unknown exception");
    }
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
  for (const std::string& failure : failures) {
    std::cout << "  FAILED " << failure << std::endl;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace ctf::test

#define CTF_TEST_CONCAT_INNER(a, b) a##b
#define CTF_TEST_CONCAT(a, b) CTF_TEST_CONCAT_INNER(a, b)

// Declares a test case.  The suite and name are also used as the deterministic
// seed source for randomized cases.
#define CTF_TEST(suite_name, case_name)                                                          \
  static void CTF_TEST_CONCAT(ctf_test_body_, __LINE__)();                                       \
  static ::ctf::test::Registrar CTF_TEST_CONCAT(ctf_test_registrar_, __LINE__)(                   \
      suite_name, case_name, &CTF_TEST_CONCAT(ctf_test_body_, __LINE__));                        \
  static void CTF_TEST_CONCAT(ctf_test_body_, __LINE__)()

#define CTF_CHECK(expression)                                                                      \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      ::ctf::test::AssertionStream(#expression, __FILE__, __LINE__).fail();                        \
    }                                                                                              \
  } while (false)

// Assertion with an extra explanatory value.  The message argument may be any
// streamable expression; it is evaluated only on failure.
#define CTF_CHECK_MSG(expression, message)                                                         \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      ::ctf::test::AssertionStream ctf_stream(#expression, __FILE__, __LINE__);                    \
      ctf_stream << (message);                                                                     \
      ctf_stream.fail();                                                                           \
    }                                                                                              \
  } while (false)

#define CTF_CHECK_EQ(actual, expected)                                                             \
  do {                                                                                             \
    const auto& ctf_actual_value = (actual);                                                       \
    const auto& ctf_expected_value = (expected);                                                   \
    if (!(ctf_actual_value == ctf_expected_value)) {                                               \
      ::ctf::test::AssertionStream ctf_stream(#actual " == " #expected, __FILE__, __LINE__);        \
      ctf_stream << "actual=" << ctf_actual_value << " expected=" << ctf_expected_value;            \
      ctf_stream.fail();                                                                           \
    }                                                                                              \
  } while (false)

#define CTF_CHECK_NE(actual, unexpected)                                                           \
  do {                                                                                             \
    const auto& ctf_actual_value = (actual);                                                       \
    const auto& ctf_unexpected_value = (unexpected);                                               \
    if (ctf_actual_value == ctf_unexpected_value) {                                                \
      ::ctf::test::AssertionStream ctf_stream(#actual " != " #unexpected, __FILE__, __LINE__);      \
      ctf_stream << "both=" << ctf_actual_value;                                                    \
      ctf_stream.fail();                                                                           \
    }                                                                                              \
  } while (false)

#define CTF_REQUIRE(expression)                                                                    \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      ::ctf::test::AssertionStream("REQUIRE " #expression, __FILE__, __LINE__).fail();              \
    }                                                                                              \
  } while (false)

// Fails unless the status is ok, reporting the deterministic error code.
#define CTF_CHECK_OK(status_expression)                                                            \
  do {                                                                                             \
    const ::ctf::Status ctf_status_value = (status_expression);                                     \
    if (!ctf_status_value.is_ok()) {                                                               \
      ::ctf::test::AssertionStream ctf_stream("is_ok(" #status_expression ")", __FILE__, __LINE__); \
      ctf_stream << "code=" << ::ctf::to_string(ctf_status_value.code())                            \
                 << " message=" << ctf_status_value.message();                                      \
      ctf_stream.fail();                                                                           \
    }                                                                                              \
  } while (false)

#define CTF_CHECK_CODE(status_expression, expected_code)                                            \
  do {                                                                                             \
    const ::ctf::Status ctf_status_value = (status_expression);                                     \
    if (ctf_status_value.code() != (expected_code)) {                                               \
      ::ctf::test::AssertionStream ctf_stream(#status_expression " has code " #expected_code,       \
                                              __FILE__, __LINE__);                                  \
      ctf_stream << "actual=" << ::ctf::to_string(ctf_status_value.code())                          \
                 << " message=" << ctf_status_value.message();                                      \
      ctf_stream.fail();                                                                           \
    }                                                                                              \
  } while (false)

#define CTF_TEST_MAIN()                                                                            \
  int main(int argc, char** argv) { return ::ctf::test::run_all(argc, argv); }

#endif  // CTF_TESTS_TEST_FRAMEWORK_HPP
