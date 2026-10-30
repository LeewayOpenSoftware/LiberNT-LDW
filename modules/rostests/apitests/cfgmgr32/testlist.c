#define STANDALONE
#include <apitest.h>

extern void func_reenumerate(void);
extern void func_installdevinst(void);

const struct test winetest_testlist[] =
{
    { "reenumerate", func_reenumerate },
    { "installdevinst", func_installdevinst },
    { 0, 0 }
};
