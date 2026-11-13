// Minimal unit-test registry (no dependencies). Tests register with EQX_TEST and use EXPECT / EXPECT_EQ.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace eqx_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};
std::vector<Case>& registry();
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
extern int failures;
void fail(const char* file, int line, const std::string& msg);

inline std::string fixture(const std::string& name) {
#ifdef EQX_FIXTURES_DIR
    return std::string(EQX_FIXTURES_DIR) + "/" + name;
#else
    return name;
#endif
}

// True when the optional fixture directory (CMake EQX_FIXTURES_DIR) was configured.
inline bool have_fixtures() {
#ifdef EQX_FIXTURES_DIR
    return true;
#else
    return false;
#endif
}

extern int skipped;

}  // namespace eqx_test

// Skips the current test when the optional fixture directory is not configured.
#define EQX_REQUIRE_FIXTURES()                                                \
    do {                                                                      \
        if (!::eqx_test::have_fixtures()) {                                   \
            ++::eqx_test::skipped;                                            \
            return;                                                           \
        }                                                                     \
    } while (0)

#define EQX_CAT2(a, b) a##b
#define EQX_CAT(a, b) EQX_CAT2(a, b)
#define EQX_TEST(name)                                                        \
    static void name();                                                       \
    static ::eqx_test::Reg EQX_CAT(eqx_reg_, name)(#name, name);              \
    static void name()

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) ::eqx_test::fail(__FILE__, __LINE__, "EXPECT(" #cond ")"); \
    } while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto _va = (a);                                                       \
        auto _vb = (b);                                                       \
        if (!(_va == _vb)) {                                                  \
            std::ostringstream _os;                                           \
            _os << "EXPECT_EQ(" #a ", " #b "): " << _va << " != " << _vb;     \
            ::eqx_test::fail(__FILE__, __LINE__, _os.str());                  \
        }                                                                     \
    } while (0)
