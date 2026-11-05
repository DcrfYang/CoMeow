// test_urgent.cpp -- the urgent fix notice (mgmp_urgent): the version compare, and (with --live) one real ask of the official server.
// The test exe has no version resource, so it is "0.0.0": any live notice applies to it.
#include "mgmp_urgent.h"
#include "mgmp_log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mgmp {
void log_line(const char* tag, const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap); }
void log_line_lvl(LogLevel, const char* tag, const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap); }
}

static int fails = 0;
static void check(bool ok, const char* what) { printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

int main(int argc, char** argv) {
    using mgmp::urgent_version_cmp;
    check(urgent_version_cmp("0.1.185", "0.1.186") < 0, "0.1.185 < 0.1.186");
    check(urgent_version_cmp("0.1.186", "0.1.186") == 0, "0.1.186 == 0.1.186");
    check(urgent_version_cmp("0.1.200", "0.1.186") > 0, "0.1.200 > 0.1.186");
    check(urgent_version_cmp("0.2.0", "0.1.999") > 0, "0.2.0 > 0.1.999");
    check(urgent_version_cmp("0.1.9", "0.1.10") < 0, "0.1.9 < 0.1.10 (numbers, not text)");
    check(urgent_version_cmp("0.1.186.0", "0.1.186") == 0, "a fourth part of 0 is the same version");
    check(urgent_version_cmp("", "0.1.1") < 0, "an empty version is older");
    if (argc > 1 && strcmp(argv[1], "--live") == 0) {
        mgmp::urgent_start();
        mgmp::UrgentView v;
        bool got = false;
        for (int i = 0; i < 40 && !got; ++i) { Sleep(500); got = mgmp::urgent_view(v); }
        if (argc > 2 && strcmp(argv[2], "--expect-none") == 0) check(!got, "no notice is up");
        else {
            check(got, "the live notice was fetched and applies to 0.0.0");
            if (got) printf("  version %s, mine %s, id %s, text: %s\n", v.version, v.mine, v.id, v.text);
            mgmp::UrgentView n;
            check(mgmp::urgent_take_new(n) && !mgmp::urgent_take_new(n), "the chat line is taken once per notice");
            mgmp::urgent_dismiss();
            check(!mgmp::urgent_view(v), "put away: the banner is gone for this notice");
        }
    }
    printf(fails ? "FAILED %d\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
