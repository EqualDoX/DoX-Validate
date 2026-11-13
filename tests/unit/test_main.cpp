#include <cstring>

#include "unit.h"

namespace eqx_test {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int failures = 0;
int skipped = 0;
void fail(const char* file, int line, const std::string& msg) {
    ++failures;
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace eqx_test

// ./eqx-unit-tests [substring]   runs the tests whose name contains the substring
int main(int argc, char** argv) {
    int run = 0, failed = 0;
    for (auto& c : eqx_test::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        int before = eqx_test::failures;
        try {
            c.fn();
        } catch (std::exception& e) {
            eqx_test::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        ++run;
        if (eqx_test::failures != before) {
            ++failed;
            std::fprintf(stderr, "FAILED %s\n", c.name);
        }
    }
    if (eqx_test::skipped)
        std::printf("%d test(s), %d failed, %d skipped (no EQX_FIXTURES_DIR)\n", run, failed, eqx_test::skipped);
    else
        std::printf("%d test(s), %d failed\n", run, failed);
    return failed ? 1 : 0;
}
