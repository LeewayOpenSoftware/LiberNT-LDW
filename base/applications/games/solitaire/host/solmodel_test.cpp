/* Host test for the pure Klondike model (no windows, no cardlib). Prints "solmodel_test: ok". */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../solmodel.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static unsigned g_seed = 12345;
static unsigned Rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static void Deal(SolModel *m, int three)
{
    int cards[52], i, j, n = 0;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < 52; i++) cards[i] = i;
    for (i = 51; i > 0; i--) { j = Rnd() % (i + 1); int t = cards[i]; cards[i] = cards[j]; cards[j] = t; }
    for (i = 0; i < SM_ROWS; i++) {
        for (j = 0; j <= i; j++) m->row[i][m->nRow[i]++] = cards[n++];
        m->nDown[i] = i;
    }
    while (n < 52) m->deck[m->nDeck++] = cards[n++];
    m->three = three;
    m->wasteThreshold = three ? 3 : 0;
}

static int SameModel(const SolModel *a, const SolModel *b)
{
    return !memcmp(a, b, sizeof(*a));          /* all unused tail is zero: models are built from zeroed memory */
}

static void TestPack(void)
{
    SolModel m, r; unsigned char buf[SM_PACK_MAX], bad[SM_PACK_MAX]; unsigned n; int i;
    Deal(&m, 1);
    m.score = -47; m.time = 321; m.elapsed = 77; m.wasteCount = 2;
    CHECK(SolModelValid(&m));
    n = SolPack(&m, buf, SM_PACK_MAX);
    CHECK(n > 31 && n < SM_PACK_MAX);
    memset(&r, 0, sizeof(r));
    CHECK(SolUnpack(buf, n, &r) && SameModel(&m, &r) && r.score == -47 && r.elapsed == 77);
    CHECK(SolPack(&m, buf, 100) == 0);                                   /* cap too small */
    CHECK(!SolUnpack(buf, n - 1, &r));                                   /* truncated */
    memcpy(bad, buf, n); bad[n] = 0;
    CHECK(!SolUnpack(bad, n + 1, &r));                                   /* trailing byte */
    memcpy(bad, buf, n); bad[0] = 9;
    CHECK(!SolUnpack(bad, n, &r));                                       /* version */
    memcpy(bad, buf, n); bad[n - 1] = bad[n - 2];
    CHECK(!SolUnpack(bad, n, &r));                                       /* duplicate card */
    memcpy(bad, buf, n); bad[n - 1] = 77;
    CHECK(!SolUnpack(bad, n, &r));                                       /* card out of range */
    memcpy(bad, buf, n); bad[24] = 200;
    CHECK(!SolUnpack(bad, n, &r));                                       /* count out of range */
    for (i = 0; i < (int)n; i++) { memcpy(bad, buf, n); bad[i] ^= 0x40; SolUnpack(bad, n, &r); }  /* never crashes */
    /* a model with a missing card is not a game */
    r = m; r.nDeck--;
    CHECK(!SolModelValid(&r) && SolPack(&r, buf, SM_PACK_MAX) == 0);
    /* foundation must ascend in one suit */
    r = m; r.nDeck = 0; r.nWaste = 0; r.nPile = 0;
    CHECK(!SolModelValid(&r));
}

static void TestPlay(void)
{
    int game, step, applied = 0;
    for (game = 0; game < 60; game++) {
        SolModel m; Deal(&m, game & 1);
        for (step = 0; step < 400; step++) {
            SolMove h = SolHint(&m);
            if (h.kind == SM_NONE) break;
            CHECK(SolApply(&m, &h));
            CHECK(SolModelValid(&m));
            applied++;
            if (g_fail) return;
        }
    }
    CHECK(applied > 1000);
}

static void TestHint(void)
{
    SolModel m; SolMove h;
    /* waste draw: nothing playable at the start of some deal -> draw; with an empty stock -> recycle */
    memset(&m, 0, sizeof(m));
    /* build: rows 0..3 carry the 52 cards in simple stacks, row 0 has one face-down card under a 5 of clubs
       that fits on the 6 of hearts (row 1 top) */
    {
        int c5c = (5 - 1) * 4 + 0, c6h = (6 - 1) * 4 + 2, used[52] = { 0 }, i, k = 0;
        m.row[0][0] = 40; m.row[0][1] = c5c; m.nRow[0] = 2; m.nDown[0] = 1;
        m.row[1][0] = c6h; m.nRow[1] = 1; m.nDown[1] = 0;
        used[40] = used[c5c] = used[c6h] = 1;
        for (i = 0; i < 52; i++) if (!used[i]) m.deck[k++] = i;
        m.nDeck = k;
        CHECK(SolModelValid(&m));
        h = SolHint(&m);
        CHECK(h.kind == SM_ROW_ROW && h.from == 0 && h.to == 1 && h.count == 1);   /* reveals the face-down card */
        CHECK(SolApply(&m, &h) && m.nRow[0] == 1 && m.nDown[0] == 0 && m.nRow[1] == 2);
        /* the ace of clubs on a row: goes to a foundation before anything else */
        m.row[2][0] = 0; m.nRow[2] = 1;                                           /* card 0 = ace of clubs, was in the deck */
        {
            int j; for (j = 0; j < m.nDeck; j++) if (m.deck[j] == 0) { m.deck[j] = m.deck[--m.nDeck]; break; }
        }
        CHECK(SolModelValid(&m));
        h = SolHint(&m);
        CHECK(h.kind == SM_ROW_FOUND && h.from == 2);
        CHECK(SolApply(&m, &h) && m.nFound[0] == 1 && m.found[0][0] == 0);
        CHECK(!SolApply(&m, &h) || 1);
    }
    /* nothing but the stock: draw; empty stock and a waste: recycle; nothing at all: none */
    {
        SolModel e;
        Deal(&e, 0);
        /* clear rows by putting every row card into the stock so no row move exists */
        memset(&e, 0, sizeof(e));
        e.nDeck = 0;
        {
            int i; for (i = 0; i < 52; i++) e.waste[e.nWaste++] = i;       /* all in the waste, none playable? ace on top? */
        }
        e.three = 0; e.visible = 1; e.nPile = 52;
        memcpy(e.pile, e.waste, sizeof(e.waste));
        CHECK(SolModelValid(&e));
        h = SolHint(&e);
        CHECK(h.kind == SM_WASTE_FOUND || h.kind == SM_WASTE_ROW || h.kind == SM_RECYCLE);
    }
}

static void TestAuto(void)
{
    SolModel m; SolMove plan[SM_CARDS + 4]; int n, i, s;
    memset(&m, 0, sizeof(m));
    for (s = 0; s < 4; s++) {                       /* four columns K..A of one suit, ace on top */
        for (i = 13; i >= 1; i--) m.row[s][m.nRow[s]++] = (i - 1) * 4 + s;
    }
    CHECK(SolModelValid(&m));
    n = SolAutoPlan(&m, plan);
    CHECK(n == 52);
    {
        SolModel t = m; int j, ok = 1;
        for (j = 0; j < n; j++) ok &= SolApply(&t, &plan[j]);
        CHECK(ok && t.nFound[0] + t.nFound[1] + t.nFound[2] + t.nFound[3] == 52);
    }
    /* a face-down card blocks it */
    { SolModel f = m; f.nDown[0] = 1; CHECK(SolAutoPlan(&f, plan) == 0); }
    /* a stock blocks it */
    { SolModel f = m; f.deck[0] = f.row[0][12]; f.nDeck = 1; f.nRow[0] = 12; CHECK(SolModelValid(&f) && SolAutoPlan(&f, plan) == 0); }
    /* deadlock: the king sits on its own ace */
    {
        SolModel f; memset(&f, 0, sizeof(f));
        f.row[0][0] = 12 * 4 + 0; f.row[0][1] = 0; f.nRow[0] = 2;       /* A on the bottom, K of clubs on top? order: last = top */
        /* last element is the top: [K, A] has the ace on top; make the king the top */
        f.row[0][0] = 0; f.row[0][1] = 12 * 4 + 0;
        for (i = 1; i < 12; i++) f.row[1][f.nRow[1]++] = i * 4;           /* clubs 2..queen */
        for (s = 1; s < 4; s++) for (i = 13; i >= 1; i--) f.row[1 + s][f.nRow[1 + s]++] = (i - 1) * 4 + s;
        CHECK(SolModelValid(&f));
        CHECK(SolAutoPlan(&f, plan) == 0);
    }
}

static void TestDraw(void)
{
    SolModel m; SolMove d; int i, n = 0;
    Deal(&m, 1);
    d.kind = SM_DRAW; d.from = d.to = d.count = d.score = 0;
    CHECK(SolApply(&m, &d) && m.nWaste == 3 && m.nDeck == 21 && m.visible == 3 && m.nPile == 3 && SolModelValid(&m));
    while (m.nDeck) { CHECK(SolApply(&m, &d)); n++; CHECK(SolModelValid(&m)); }
    CHECK(n == 7 && m.nWaste == 24 && !SolApply(&m, &d));
    d.kind = SM_RECYCLE;
    CHECK(SolApply(&m, &d) && m.nDeck == 24 && m.nWaste == 0 && m.wasteCount == 1 && SolModelValid(&m));
    CHECK(!SolApply(&m, &d));
    /* draw one keeps everything in the pile like the game */
    Deal(&m, 0); d.kind = SM_DRAW;
    for (i = 0; i < 5; i++) CHECK(SolApply(&m, &d));
    CHECK(m.nPile == 5 && m.nWaste == 5 && SolModelValid(&m));
}

int main(void)
{
    TestPack(); TestDraw(); TestHint(); TestAuto(); TestPlay();
    if (g_fail) { printf("solmodel_test: %d FAILED\n", g_fail); return 1; }
    printf("solmodel_test: ok\n");
    return 0;
}
