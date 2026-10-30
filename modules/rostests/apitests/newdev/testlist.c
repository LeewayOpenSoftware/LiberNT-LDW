#define STANDALONE
#include <apitest.h>

extern void func_protocol(void);
extern void func_rollback(void);

const struct test winetest_testlist[] =
{
    { "protocol", func_protocol },
    { "rollback", func_rollback },
    { 0, 0 }
};
