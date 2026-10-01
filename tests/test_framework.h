#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include "common/status.h"

namespace lsm::test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& Instance() {
        static TestRegistry registry;
        return registry;
    }

    void Register(const std::string& name, std::function<void()> func) {
        tests_.push_back({name, std::move(func)});
    }

    int RunAll() {
        int passed = 0;
        int failed = 0;
        std::cout << "========================================" << std::endl;
        std::cout << " Running " << tests_.size() << " Unit Tests" << std::endl;
        std::cout << "========================================" << std::endl;

        for (const auto& test : tests_) {
            std::cout << "[ RUN      ] " << test.name << std::endl;
            try {
                test.func();
                std::cout << "[       OK ] " << test.name << std::endl;
                passed++;
            } catch (const std::exception& e) {
                std::cout << "[  FAILED  ] " << test.name << ": " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "[  FAILED  ] " << test.name << ": Unknown exception" << std::endl;
                failed++;
            }
        }

        std::cout << "========================================" << std::endl;
        std::cout << " Test Summary: " << passed << " passed, " << failed << " failed, "
                  << tests_.size() << " total." << std::endl;
        std::cout << "========================================" << std::endl;

        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
};

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        TestRegistry::Instance().Register(name, std::move(func));
    }
};

class TestFailureException : public std::exception {
public:
    explicit TestFailureException(std::string msg) : msg_(std::move(msg)) {}
    [[nodiscard]] const char* what() const noexcept override {
        return msg_.c_str();
    }
private:
    std::string msg_;
};

} // namespace lsm::test

#define TEST_CASE(name) \
    static void name(); \
    static ::lsm::test::TestRegistrar registrar_##name(#name, name); \
    static void name()

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: (" #condition ") at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

#define ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            std::ostringstream ss; \
            ss << "Assertion failed: !(" #condition ") at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: " #a " == " #b " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: " #a " != " #b " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

#define ASSERT_STATUS_OK(status) \
    do { \
        const ::lsm::Status& _lsm_eval_status = (status); \
        if (!_lsm_eval_status.ok()) { \
            std::ostringstream ss; \
            ss << "Expected Status::Ok, got: " << _lsm_eval_status.ToString() << " at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

#define ASSERT_STATUS_CODE(status, expected_code) \
    do { \
        const ::lsm::Status& _lsm_eval_status = (status); \
        if (_lsm_eval_status.code() != (expected_code)) { \
            std::ostringstream ss; \
            ss << "Expected status code " << static_cast<int>(expected_code) \
               << ", got " << static_cast<int>(_lsm_eval_status.code()) << " (" << _lsm_eval_status.ToString() << ") at " << __FILE__ << ":" << __LINE__; \
            throw ::lsm::test::TestFailureException(ss.str()); \
        } \
    } while (0)

