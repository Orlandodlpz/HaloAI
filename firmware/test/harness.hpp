// A 60 line test harness, so the host tests need no third party dependency and no
// network to build. Run all tests: ./halo_tests   One test: ./halo_tests <substring>
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace halo::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

inline std::string& current() {
    static std::string s;
    return s;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::printf("  FAIL %s\n    %s:%d: %s\n", current().c_str(), file, line, what.c_str());
}

inline int run(const char* filter) {
    int ran = 0;
    for (const Case& c : registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        current() = c.name;
        const int before = failures();
        ++ran;
        c.fn();
        if (failures() == before) std::printf("  ok   %s\n", c.name);
    }
    std::printf("\n%d test(s) run, %d failure(s)\n", ran, failures());
    if (ran == 0) {
        std::printf("no test matched the filter: that is a failure, not a pass\n");
        return 1;
    }
    return failures() == 0 ? 0 : 1;
}

}  // namespace halo::test

#define HALO_TEST(name)                                                       \
    static void name();                                                       \
    static ::halo::test::Register halo_reg_##name(#name, &name);              \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) ::halo::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_STATE(sm, expected)                                             \
    do {                                                                      \
        if ((sm).state() != (expected))                                       \
            ::halo::test::fail(__FILE__, __LINE__,                            \
                               std::string("expected ") + ::halo::name(expected) + \
                                   ", got " + ::halo::name((sm).state()));    \
    } while (0)

#define CHECK_EVENT(ev, expected)                                             \
    do {                                                                      \
        if ((ev) != (expected))                                               \
            ::halo::test::fail(__FILE__, __LINE__,                            \
                               std::string("expected ") + ::halo::name(expected) + \
                                   ", got " + ::halo::name(ev));              \
    } while (0)
