/* Host test for the pure Minesweeper board logic. Prints "mbcore_test: ok". */
#include <stdio.h>
#include <string.h>
#include "../mbcore.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static unsigned char grid[(MB_MAX_COLS + 2) * (MB_MAX_ROWS + 2)];

static int CountMines(int cols, int rows)
{
    int c, r, n = 0, stride = rows + 2;
    for (c = 0; c <= cols + 1; c++)
        for (r = 0; r <= rows + 1; r++) {
            if (c == 0 || r == 0 || c == cols + 1 || r == rows + 1) { if (grid[c * stride + r]) return -1; }
            else n += grid[c * stride + r];
        }
    return n;
}

static void TestClamp(void)
{
    unsigned r = 3, c = 500, m = 0;
    MbClampLevel(&r, &c, &m);
    CHECK(r == 9 && c == 99 && m == 10);
    r = 9; c = 9; m = 500;
    MbClampLevel(&r, &c, &m);
    CHECK(m == 72);                                  /* 81 - 9 */
    r = 16; c = 30; m = 99;
    MbClampLevel(&r, &c, &m);
    CHECK(r == 16 && c == 30 && m == 99);            /* expert unchanged */
    r = 200; c = 200; m = 99999;
    MbClampLevel(&r, &c, &m);
    CHECK(r == 99 && c == 99 && m == 99 * 99 - 9);
}

static void TestPlace(void)
{
    unsigned seed = 1;
    int trial, cols, rows, sc, sr, i, j, stride;
    for (trial = 0; trial < 400; trial++) {
        cols = 9 + (trial * 7) % 40; rows = 9 + (trial * 11) % 40;
        stride = rows + 2;
        sc = 1 + (trial * 5) % cols; sr = 1 + (trial * 3) % rows;       /* includes corners and edges */
        if (trial % 4 == 0) { sc = 1; sr = 1; }
        if (trial % 4 == 1) { sc = cols; sr = rows; }
        {
            int mines = 10 + (trial * 13) % (cols * rows - 19);
            if (trial % 10 == 0) mines = cols * rows - 9;                 /* densest legal board */
            CHECK(MbPlaceMines(grid, stride, cols, rows, mines, sc, sr, &seed) == 1);
            CHECK(CountMines(cols, rows) == mines);
            for (i = -1; i <= 1; i++) for (j = -1; j <= 1; j++)
                CHECK(!grid[(sc + i) * stride + (sr + j)]);               /* the 3x3 area is free */
            CHECK(MbCountAround(grid, stride, sc, sr) == 0);              /* so the first click opens an area */
        }
        if (g_fail) return;
    }
    /* a board denser than legal: only the clicked cell is guaranteed free */
    cols = rows = 9; stride = 11;
    CHECK(MbPlaceMines(grid, stride, cols, rows, 80, 5, 5, &seed) == 0);
    CHECK(CountMines(cols, rows) == 80 && !grid[5 * stride + 5]);
    /* deterministic for a seed, different for another */
    {
        unsigned char a[11 * 11], b[11 * 11]; unsigned s1 = 77, s2 = 77, s3 = 78;
        MbPlaceMines(grid, stride, 9, 9, 10, 1, 1, &s1); memcpy(a, grid, sizeof(a));
        MbPlaceMines(grid, stride, 9, 9, 10, 1, 1, &s2); memcpy(b, grid, sizeof(b));
        CHECK(!memcmp(a, b, sizeof(a)));
        MbPlaceMines(grid, stride, 9, 9, 10, 1, 1, &s3);
        CHECK(memcmp(a, grid, sizeof(a)) != 0);
    }
    /* a dirty grid is cleaned */
    memset(grid, 1, sizeof(grid));
    MbPlaceMines(grid, 11, 9, 9, 10, 4, 4, &seed);
    CHECK(CountMines(9, 9) == 10);
}

static void TestSave(void)
{
    static MbSave s, r; unsigned char buf[MB_PACK_MAX], bad[MB_PACK_MAX]; unsigned n, stride, i;
    unsigned seed = 5;
    memset(&s, 0, sizeof(s));
    s.cols = 12; s.rows = 10; s.mines = 15; s.difficulty = 3; s.time = 42;
    stride = s.rows + 2;
    MbPlaceMines(s.cell, stride, s.cols, s.rows, s.mines, 6, 5, &seed);
    s.cell[6 * stride + 5] |= 3 << MB_CELL_FLAG_SHIFT;               /* revealed */
    s.cell[1 * stride + 1] |= 2 << MB_CELL_FLAG_SHIFT;               /* a flag (on a free or mined cell) */
    n = MbPack(&s, buf, sizeof(buf));
    CHECK(n == 16 + 12 * 10);
    memset(&r, 0, sizeof(r));
    CHECK(MbUnpack(buf, n, &r) && r.cols == 12 && r.rows == 10 && r.mines == 15 && r.time == 42 && r.difficulty == 3);
    CHECK(!memcmp(r.cell, s.cell, sizeof(s.cell)));
    CHECK(MbPack(&s, buf, 20) == 0);                                  /* cap too small */
    CHECK(!MbUnpack(buf, n - 1, &r) && !MbUnpack(buf, 5, &r));
    memcpy(bad, buf, n); bad[n] = 0;
    CHECK(!MbUnpack(bad, n + 1, &r));
    memcpy(bad, buf, n); bad[0] = 2; CHECK(!MbUnpack(bad, n, &r));
    memcpy(bad, buf, n); bad[1] = 5; CHECK(!MbUnpack(bad, n, &r));    /* width out of range */
    memcpy(bad, buf, n); bad[3] = 9; CHECK(!MbUnpack(bad, n, &r));    /* difficulty */
    memcpy(bad, buf, n); bad[4] = 14; CHECK(!MbUnpack(bad, n, &r));   /* mine count disagrees with the cells */
    memcpy(bad, buf, n); bad[8] = 0xE8; bad[9] = 0x03; bad[10] = 0; CHECK(!MbUnpack(bad, n, &r)); /* time 1000 */
    memcpy(bad, buf, n); bad[20] |= 0x80; CHECK(!MbUnpack(bad, n, &r)); /* junk bits */
    for (i = 16; i < n; i++) { memcpy(bad, buf, n); bad[i] ^= 1; MbUnpack(bad, n, &r); }  /* never crashes */
    /* a revealed mine, or nothing revealed, is not a saved game */
    { MbSave x = s; for (i = 0; i < sizeof(x.cell); i++) if (x.cell[i] & MB_CELL_MINE) { x.cell[i] |= 3 << MB_CELL_FLAG_SHIFT; break; }
      CHECK(MbPack(&x, buf, sizeof(buf)) == 0); }
    { MbSave x = s; x.cell[6 * stride + 5] &= ~MB_CELL_FLAG_MASK; CHECK(MbPack(&x, buf, sizeof(buf)) == 0); }
    { MbSave x = s; x.mines = 16; CHECK(MbPack(&x, buf, sizeof(buf)) == 0); }
    { MbSave x = s; x.time = 1500; CHECK(MbPack(&x, buf, sizeof(buf)) == 0); }
}

int main(void)
{
    TestClamp(); TestPlace(); TestSave();
    if (g_fail) { printf("mbcore_test: %d FAILED\n", g_fail); return 1; }
    printf("mbcore_test: ok\n");
    return 0;
}
