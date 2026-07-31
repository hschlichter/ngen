#pragma once

#include <string>
#include <string_view>
#include <vector>

// Minimal test harness shared by the ngen-test-<system> binaries. Knows nothing
// about the system under test: suites register named test functions with
// NGEN_TEST, hold their own state (device, fixtures) at file scope, and wire
// suite-level setup through TestRunOptions hooks.

namespace ngentest {

class TestContext {
public:
    TestContext(std::string_view testName, std::string_view dumpDirectory) : m_name(testName), m_dumpDir(dumpDirectory) {}

    // Records a failure when the condition is false; the test keeps running so
    // one bad expectation doesn't hide the rest.
    auto expect(bool condition, std::string_view message) -> void {
        if (!condition) {
            m_failures.emplace_back(message);
        }
    }

    [[nodiscard]] auto failed() const -> bool { return !m_failures.empty(); }
    [[nodiscard]] auto failures() const -> const std::vector<std::string>& { return m_failures; }
    [[nodiscard]] auto name() const -> std::string_view { return m_name; }
    // Directory for failure artifacts (--dump-dir); empty when dumping is off.
    [[nodiscard]] auto dumpDir() const -> std::string_view { return m_dumpDir; }

private:
    std::string m_name;
    std::string m_dumpDir;
    std::vector<std::string> m_failures;
};

using TestFunc = void (*)(TestContext&);

auto registerTest(const char* name, TestFunc func) -> bool;

struct TestRunOptions {
    // Suite-level hooks run around every test in the same TestContext, so a
    // failed expectation in afterEach (e.g. a validation-counter delta) fails
    // the test it wrapped.
    TestFunc beforeEach = nullptr;
    TestFunc afterEach = nullptr;
};

// Parses --obs-output=<path>, --test=<name>, and --dump-dir=<path>, runs the
// registered tests, reports per-test results on stdout and the observation bus
// (category "Test"), and returns the number of failed tests.
auto runTests(int argc, char** argv, const TestRunOptions& options) -> int;

} // namespace ngentest

#define NGEN_TEST_CONCAT2(a, b) a##b
#define NGEN_TEST_CONCAT(a, b) NGEN_TEST_CONCAT2(a, b)

// NGEN_TEST("name") { ... } — defines and registers a test function. The name
// is a free-form string; uniqueness of the generated identifiers comes from
// __LINE__, so one macro use per line.
#define NGEN_TEST(name)                                                              \
    static void NGEN_TEST_CONCAT(ngenTestFunc, __LINE__)(::ngentest::TestContext&);  \
    static const bool NGEN_TEST_CONCAT(ngenTestRegistered, __LINE__) =               \
        ::ngentest::registerTest((name), &NGEN_TEST_CONCAT(ngenTestFunc, __LINE__)); \
    static void NGEN_TEST_CONCAT(ngenTestFunc, __LINE__)(::ngentest::TestContext & ctx)
