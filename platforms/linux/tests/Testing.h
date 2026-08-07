// A test harness in one header.
//
// No GoogleTest, no Catch2. The whole point of this project is that it has no
// dependencies, and a test framework is still a dependency — it would be the
// only thing in the tree that has to be fetched, and it would make `cmake
// --preset default && cmake --build --preset default` stop being the entire
// setup.
//
// What is here is what the suite uses: registration, four assertions, and a
// name filter so ctest can address groups. Assertions record a failure and
// carry on rather than throwing, since the app is built with -fno-exceptions
// and the tests share its flags.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> registry;
    return registry;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline std::string& currentCase() {
    static std::string name;
    return name;
}

inline void reportFailure(const char* file, int line, const std::string& detail) {
    ++failureCount();
    std::fprintf(stderr, "  FAIL %s\n    %s:%d: %s\n", currentCase().c_str(), file, line,
                 detail.c_str());
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        cases().push_back({suite, name, std::move(body)});
    }
};

inline int runAll(const std::string& filter) {
    int ran = 0;
    for (const Case& test : cases()) {
        if (!filter.empty() && test.suite != filter) continue;
        currentCase() = test.suite + "." + test.name;
        const int before = failureCount();
        test.body();
        ++ran;
        if (failureCount() == before && std::getenv("VERBOSE_TESTS") != nullptr) {
            std::fprintf(stderr, "  ok   %s\n", currentCase().c_str());
        }
    }

    if (ran == 0) {
        std::fprintf(stderr, "no tests matched \"%s\"\n", filter.c_str());
        return 1;
    }
    std::fprintf(stderr, "%d test%s, %d failure%s\n", ran, ran == 1 ? "" : "s", failureCount(),
                 failureCount() == 1 ? "" : "s");
    return failureCount() == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(suite, name)                                                             \
    static void suite##_##name();                                                     \
    static ::testing::Registrar registrar_##suite##_##name(#suite, #name,             \
                                                           suite##_##name);           \
    static void suite##_##name()

#define EXPECT_TRUE(condition)                                                        \
    do {                                                                              \
        if (!(condition)) ::testing::reportFailure(__FILE__, __LINE__, #condition);    \
    } while (false)

#define EXPECT_FALSE(condition) EXPECT_TRUE(!(condition))

#define EXPECT_EQ(actual, expected)                                                   \
    do {                                                                              \
        const auto actualValue = (actual);                                            \
        const auto expectedValue = (expected);                                        \
        if (!(actualValue == expectedValue)) {                                        \
            ::testing::reportFailure(__FILE__, __LINE__,                              \
                                     std::string(#actual) + " != " + #expected);      \
        }                                                                             \
    } while (false)

// Floating point comparisons need a tolerance, and every one in this suite is a
// fraction between 0 and 1, so an absolute epsilon is the right shape.
#define EXPECT_NEAR(actual, expected, tolerance)                                      \
    do {                                                                              \
        const double actualValue = (actual);                                          \
        const double expectedValue = (expected);                                      \
        if (std::fabs(actualValue - expectedValue) > (tolerance)) {                   \
            char buffer[160];                                                         \
            std::snprintf(buffer, sizeof(buffer), "%s: %.6f is not within %g of %.6f", \
                          #actual, actualValue, static_cast<double>(tolerance),       \
                          expectedValue);                                             \
            ::testing::reportFailure(__FILE__, __LINE__, buffer);                     \
        }                                                                             \
    } while (false)
