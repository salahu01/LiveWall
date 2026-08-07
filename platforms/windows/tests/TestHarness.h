// A test harness in one header.
//
// The alternative was GoogleTest, which would have been the project's only
// dependency and would have doubled the build. The suite is four files of pure
// arithmetic — coverage geometry, frame pacing, output sizing, index decoding —
// and this is enough for that: register a case, assert, report.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace livewall::test {

struct Case {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

inline std::string& currentCase() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::printf("  FAIL %s\n    %s:%d\n    %s\n", currentCase().c_str(), file, line,
                message.c_str());
}

inline int runAll() {
    std::printf("running %zu tests\n", registry().size());
    for (const Case& testCase : registry()) {
        currentCase() = testCase.name;
        const int before = failureCount();
        testCase.body();
        if (failureCount() == before) std::printf("  ok   %s\n", testCase.name.c_str());
    }

    if (failureCount() == 0) {
        std::printf("all %zu tests passed\n", registry().size());
        return 0;
    }
    std::printf("%d assertion(s) failed\n", failureCount());
    return 1;
}

}  // namespace livewall::test

#define LIVEWALL_CONCAT_INNER(a, b) a##b
#define LIVEWALL_CONCAT(a, b) LIVEWALL_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                            \
    static void LIVEWALL_CONCAT(livewall_test_, __LINE__)();                       \
    static ::livewall::test::Registrar LIVEWALL_CONCAT(livewall_registrar_,        \
                                                       __LINE__)(                  \
        name, &LIVEWALL_CONCAT(livewall_test_, __LINE__));                         \
    static void LIVEWALL_CONCAT(livewall_test_, __LINE__)()

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            ::livewall::test::reportFailure(__FILE__, __LINE__,                    \
                                            "expected: " #condition);              \
        }                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                 \
    do {                                                                           \
        const auto actualValue = (actual);                                         \
        const auto expectedValue = (expected);                                     \
        if (!(actualValue == expectedValue)) {                                     \
            ::livewall::test::reportFailure(                                       \
                __FILE__, __LINE__,                                                \
                std::string(#actual " == " #expected) + " — got " +                \
                    ::livewall::test::describe(actualValue) + ", expected " +      \
                    ::livewall::test::describe(expectedValue));                    \
        }                                                                          \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                    \
    do {                                                                           \
        const double actualValue = static_cast<double>(actual);                    \
        const double expectedValue = static_cast<double>(expected);                \
        if (std::fabs(actualValue - expectedValue) > (tolerance)) {                \
            char buffer[160];                                                      \
            std::snprintf(buffer, sizeof(buffer),                                  \
                          "%s — got %.6f, expected %.6f (tolerance %.6f)",         \
                          #actual, actualValue, expectedValue,                     \
                          static_cast<double>(tolerance));                         \
            ::livewall::test::reportFailure(__FILE__, __LINE__, buffer);           \
        }                                                                          \
    } while (false)

namespace livewall::test {

// One constrained template rather than an overload set per width.
//
// An overload per integer type is a trap: `size_t` is `unsigned long long` on
// 64-bit MSVC, `unsigned int` on 32-bit, and `unsigned long` elsewhere, so any
// fixed set either misses a type or redefines one, and the failure is an
// "ambiguous call" inside a macro expansion rather than anything that names the
// cause. std::to_string already covers every arithmetic type.
template <typename T>
std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, std::string>
describe(T value) {
    return std::to_string(value);
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(const char* value) { return std::string("\"") + value + "\""; }

}  // namespace livewall::test
