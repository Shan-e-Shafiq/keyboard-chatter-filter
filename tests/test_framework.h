// SPDX-License-Identifier: MIT
//
// Minimal self-contained test harness. Keeps the project free of build-time downloads.
//
//   TEST(group, name) { EXPECT_EQ(a, b); ASSERT_TRUE(x); }
//
// Each test binary runs every registered test, or only those whose "group.name" contains the
// first command-line argument.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace kcf_test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

struct AssertionAbort {};

template <typename T>
std::string describe(const T& value) {
    if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else {
        return "<unprintable>";
    }
}

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

inline int run_all(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int run = 0;
    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        ++run;
        const int before = failure_count();
        try {
            test.body();
        } catch (const AssertionAbort&) {
        } catch (const std::exception& e) {
            report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        if (failure_count() != before) {
            ++failed_tests;
            std::fprintf(stderr, "[ FAIL ] %s\n", test.name.c_str());
        } else {
            std::printf("[  OK  ] %s\n", test.name.c_str());
        }
    }
    std::printf("\n%d test(s) run, %d failed\n", run, failed_tests);
    if (run == 0) {
        std::fprintf(stderr, "no tests matched filter '%.*s'\n", static_cast<int>(filter.size()), filter.data());
        return 1;
    }
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace kcf_test

#define KCF_TEST_CONCAT_INNER(a, b) a##b
#define KCF_TEST_CONCAT(a, b) KCF_TEST_CONCAT_INNER(a, b)

#define TEST(group, name)                                                                                  \
    static void KCF_TEST_CONCAT(test_body_, KCF_TEST_CONCAT(group, KCF_TEST_CONCAT(_, name)))();            \
    static const ::kcf_test::Registrar KCF_TEST_CONCAT(test_registrar_,                                     \
                                                       KCF_TEST_CONCAT(group, KCF_TEST_CONCAT(_, name))){   \
        #group "." #name, &KCF_TEST_CONCAT(test_body_, KCF_TEST_CONCAT(group, KCF_TEST_CONCAT(_, name)))};  \
    static void KCF_TEST_CONCAT(test_body_, KCF_TEST_CONCAT(group, KCF_TEST_CONCAT(_, name)))()

#define KCF_CHECK_BINARY(op, a, b, fatal)                                                                   \
    do {                                                                                                     \
        const auto& kcf_lhs_ = (a);                                                                          \
        const auto& kcf_rhs_ = (b);                                                                          \
        if (!(kcf_lhs_ op kcf_rhs_)) {                                                                       \
            ::kcf_test::report_failure(__FILE__, __LINE__,                                                   \
                                       std::string("expected ") + #a " " #op " " #b ", got " +               \
                                           ::kcf_test::describe(kcf_lhs_) + " vs " +                         \
                                           ::kcf_test::describe(kcf_rhs_));                                  \
            if (fatal) throw ::kcf_test::AssertionAbort{};                                                   \
        }                                                                                                    \
    } while (false)

#define KCF_CHECK_BOOL(cond, expected, fatal)                                                               \
    do {                                                                                                     \
        if (static_cast<bool>(cond) != (expected)) {                                                         \
            ::kcf_test::report_failure(__FILE__, __LINE__,                                                   \
                                       std::string("expected ") + #cond " to be " #expected);                \
            if (fatal) throw ::kcf_test::AssertionAbort{};                                                   \
        }                                                                                                    \
    } while (false)

#define EXPECT_EQ(a, b) KCF_CHECK_BINARY(==, a, b, false)
#define EXPECT_NE(a, b) KCF_CHECK_BINARY(!=, a, b, false)
#define EXPECT_LT(a, b) KCF_CHECK_BINARY(<, a, b, false)
#define EXPECT_LE(a, b) KCF_CHECK_BINARY(<=, a, b, false)
#define EXPECT_GE(a, b) KCF_CHECK_BINARY(>=, a, b, false)
#define EXPECT_TRUE(c) KCF_CHECK_BOOL(c, true, false)
#define EXPECT_FALSE(c) KCF_CHECK_BOOL(c, false, false)
#define ASSERT_EQ(a, b) KCF_CHECK_BINARY(==, a, b, true)
#define ASSERT_TRUE(c) KCF_CHECK_BOOL(c, true, true)
#define ASSERT_FALSE(c) KCF_CHECK_BOOL(c, false, true)

#define KCF_TEST_MAIN()                                                                                     \
    int main(int argc, char** argv) { return ::kcf_test::run_all(argc, argv); }
