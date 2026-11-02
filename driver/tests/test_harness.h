// Minimal self-registering test harness (no external dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nmtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace nmtest

#define NM_CAT2(a, b) a##b
#define NM_CAT(a, b) NM_CAT2(a, b)

#define TEST(name)                                                                \
    static void name();                                                           \
    static nmtest::Registrar NM_CAT(reg_, name)(#name, name);                     \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++nmtest::failures();                                                         \
        }                                                                                 \
    } while (0)

#define CHECK_MSG(cond, ...)                                                    \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "  %s:%d: CHECK failed: %s — ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, __VA_ARGS__);                                  \
            std::fprintf(stderr, "\n");                                         \
            ++nmtest::failures();                                               \
        }                                                                       \
    } while (0)
