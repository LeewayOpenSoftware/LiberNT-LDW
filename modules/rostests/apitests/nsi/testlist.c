#define STANDALONE
#include <apitest.h>

extern void func_notify(void);
extern void func_tables(void);

const struct test winetest_testlist[] =
{
    { "notify", func_notify },
    { "tables", func_tables },
    { 0, 0 }
};
