// Tiny dependency-free test harness shared by host-side tests.
#pragma once
#include <cstdio>
#include <functional>
#include <vector>

namespace minitest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
inline int run_all() {
    for (auto& c : registry()) {
        int before = failures();
        c.fn();
        std::printf("%s %s\n", failures() == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures());
    return failures() == 0 ? 0 : 1;
}
}  // namespace minitest

#define MT_CAT2(a, b) a##b
#define MT_CAT(a, b) MT_CAT2(a, b)
#define TEST(name) static void name(); static minitest::Reg MT_CAT(reg_, name)(#name, name); static void name()
#define CHECK(cond) do { if (!(cond)) { std::printf("  %s:%d CHECK(%s)\n", __FILE__, __LINE__, #cond); ++minitest::failures(); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::printf("  %s:%d CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); ++minitest::failures(); } } while (0)
