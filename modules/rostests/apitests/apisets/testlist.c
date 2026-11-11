
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_win11_fallback(void);
extern void func_apisetmap(void);
extern void func_win32versionvalue(void);
extern void func_win32versionvalue_child(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "win11_fallback", func_win11_fallback },
    { "apisetmap", func_apisetmap },
    { "win32versionvalue", func_win32versionvalue },
    { "win32versionvalue_child", func_win32versionvalue_child },
    { 0, 0 }
};
