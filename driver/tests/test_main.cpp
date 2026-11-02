#include <cstring>

#include "test_harness.h"

// Usage: netmic_tests [name-substring]
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;
    for (auto& c : nmtest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = nmtest::failures();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        bool ok = nmtest::failures() == before;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
        ++run;
        if (!ok) ++failed;
    }
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed == 0 && run > 0 ? 0 : 1;
}
