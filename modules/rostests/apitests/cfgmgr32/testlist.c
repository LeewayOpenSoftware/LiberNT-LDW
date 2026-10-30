#define STANDALONE
#include <apitest.h>

extern void func_reenumerate(void);

const struct test winetest_testlist[] =
{
    { "reenumerate", func_reenumerate },
    { 0, 0 }
};
