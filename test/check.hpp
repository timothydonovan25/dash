// A 40-line test harness so this repo has no dependency to install.
// Swap for Catch2 if you outgrow it; the CMakeLists is where to do it.
#pragma once
#include <cstdio>
#include <cmath>
#include <string>

namespace check {
inline int g_fail = 0;
inline const char* g_case = "";

inline void begin(const char* name) { g_case = name; }

inline void ok(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        ++g_fail;
        std::printf("  FAIL %s\n       %s\n       %s:%d\n", g_case, expr, file, line);
    }
}

inline void near(double a, double b, double tol, const char* expr,
                 const char* file, int line) {
    if (!(std::fabs(a - b) <= tol)) {
        ++g_fail;
        std::printf("  FAIL %s\n       %s\n       got %.6f, want %.6f (tol %.6f)\n       %s:%d\n",
                    g_case, expr, a, b, tol, file, line);
    }
}

inline int report(const char* suite) {
    if (g_fail == 0) std::printf("PASS %s\n", suite);
    else             std::printf("FAIL %s — %d assertion(s)\n", suite, g_fail);
    return g_fail == 0 ? 0 : 1;
}
}  // namespace check

#define CASE(name)       check::begin(name)
#define CHECK(cond)      check::ok((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a,b,t) check::near((a), (b), (t), #a " ~= " #b, __FILE__, __LINE__)
