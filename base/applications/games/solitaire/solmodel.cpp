/*
 * PROJECT:     Solitaire
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pure game model of Klondike: save/resume packing, hints, autocomplete plan
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <string.h>
#include "solmodel.h"

/* ------------------------------------------------------------------ validation / packing */

static int InRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

int SolModelValid(const SolModel *m)
{
    int seen[SM_CARDS], i, j, total = 0;

    if (!m)
        return 0;
    memset(seen, 0, sizeof(seen));
#define TAKE(c) do { if (!InRange((c), 0, SM_CARDS - 1) || seen[(c)]) return 0; seen[(c)] = 1; total++; } while (0)
    if (!InRange(m->nDeck, 0, SM_CARDS) || !InRange(m->nWaste, 0, SM_CARDS) || !InRange(m->nPile, 0, m->nWaste))
        return 0;
    for (i = 0; i < m->nDeck; i++) TAKE(m->deck[i]);
    for (i = 0; i < m->nWaste; i++) TAKE(m->waste[i]);
    for (i = 0; i < SM_FOUNDS; i++)
    {
        if (!InRange(m->nFound[i], 0, 13))
            return 0;
        for (j = 0; j < m->nFound[i]; j++)
        {
            int c = m->found[i][j];
            TAKE(c);
            if (SmRank(c) != j + 1 || SmSuit(c) != SmSuit(m->found[i][0]))
                return 0;
        }
    }
    for (i = 0; i < SM_ROWS; i++)
    {
        if (!InRange(m->nRow[i], 0, SM_CARDS) || !InRange(m->nDown[i], 0, m->nRow[i]))
            return 0;
        for (j = 0; j < m->nRow[i]; j++) TAKE(m->row[i][j]);
    }
#undef TAKE
    if (total != SM_CARDS)
        return 0;
    /* the pile shows the end of the waste: same cards, same order */
    for (i = 0; i < m->nPile; i++)
        if (m->pile[i] != m->waste[m->nWaste - m->nPile + i])
            return 0;
    if (m->three != 0 && m->three != 1)
        return 0;
    if (!InRange(m->visible, 0, m->nPile > 3 ? m->nPile : 3))
        return 0;
    return 1;
}

static void Put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static unsigned Get32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

#define PACK_VERSION 1
#define PACK_FIXED 24

unsigned SolPack(const SolModel *m, unsigned char *out, unsigned cap)
{
    unsigned n = 0;
    int i, j;

    if (!out || cap < SM_PACK_MAX || !SolModelValid(m))
        return 0;
    out[n++] = PACK_VERSION;
    out[n++] = (unsigned char)m->three;
    out[n++] = (unsigned char)m->visible;
    out[n++] = 0;
    Put32(out + n, (unsigned)m->score); n += 4;
    Put32(out + n, m->time); n += 4;
    Put32(out + n, m->wasteCount); n += 4;
    Put32(out + n, m->wasteThreshold); n += 4;
    Put32(out + n, m->elapsed); n += 4;
    out[n++] = (unsigned char)m->nDeck;
    out[n++] = (unsigned char)m->nWaste;
    out[n++] = (unsigned char)m->nPile;
    for (i = 0; i < SM_FOUNDS; i++) out[n++] = (unsigned char)m->nFound[i];
    for (i = 0; i < SM_ROWS; i++) { out[n++] = (unsigned char)m->nRow[i]; out[n++] = (unsigned char)m->nDown[i]; }
    for (i = 0; i < m->nDeck; i++) out[n++] = (unsigned char)m->deck[i];
    for (i = 0; i < m->nWaste; i++) out[n++] = (unsigned char)m->waste[i];
    for (i = 0; i < m->nPile; i++) out[n++] = (unsigned char)m->pile[i];
    for (i = 0; i < SM_FOUNDS; i++) for (j = 0; j < m->nFound[i]; j++) out[n++] = (unsigned char)m->found[i][j];
    for (i = 0; i < SM_ROWS; i++) for (j = 0; j < m->nRow[i]; j++) out[n++] = (unsigned char)m->row[i][j];
    return n;
}

int SolUnpack(const unsigned char *in, unsigned size, SolModel *m)
{
    SolModel t;
    unsigned n, need;
    int i, j;

    if (!in || !m || size < PACK_FIXED + 3 + SM_FOUNDS + 2 * SM_ROWS || in[0] != PACK_VERSION)
        return 0;
    memset(&t, 0, sizeof(t));
    t.three = in[1];
    t.visible = in[2];
    t.score = (long)(int)Get32(in + 4);
    t.time = Get32(in + 8);
    t.wasteCount = Get32(in + 12);
    t.wasteThreshold = Get32(in + 16);
    t.elapsed = Get32(in + 20);
    n = PACK_FIXED;
    t.nDeck = in[n++]; t.nWaste = in[n++]; t.nPile = in[n++];
    for (i = 0; i < SM_FOUNDS; i++) t.nFound[i] = in[n++];
    for (i = 0; i < SM_ROWS; i++) { t.nRow[i] = in[n++]; t.nDown[i] = in[n++]; }
    need = n + (unsigned)(t.nDeck + t.nWaste + t.nPile);
    for (i = 0; i < SM_FOUNDS; i++) need += (unsigned)t.nFound[i];
    for (i = 0; i < SM_ROWS; i++) need += (unsigned)t.nRow[i];
    if (need != size)                                   /* exact length: nothing missing, nothing trailing */
        return 0;
    /* the counts index fixed arrays: range-check before copying */
    if (t.nDeck > SM_CARDS || t.nWaste > SM_CARDS || t.nPile > t.nWaste)
        return 0;
    for (i = 0; i < SM_FOUNDS; i++) if (t.nFound[i] > 13) return 0;
    for (i = 0; i < SM_ROWS; i++) if (t.nRow[i] > SM_CARDS) return 0;
    for (i = 0; i < t.nDeck; i++) t.deck[i] = in[n++];
    for (i = 0; i < t.nWaste; i++) t.waste[i] = in[n++];
    for (i = 0; i < t.nPile; i++) t.pile[i] = in[n++];
    for (i = 0; i < SM_FOUNDS; i++) for (j = 0; j < t.nFound[i]; j++) t.found[i][j] = in[n++];
    for (i = 0; i < SM_ROWS; i++) for (j = 0; j < t.nRow[i]; j++) t.row[i][j] = in[n++];
    if (!SolModelValid(&t))
        return 0;
    *m = t;
    return 1;
}

/* ------------------------------------------------------------------ rules */

static int RowTop(const SolModel *m, int r) { return m->nRow[r] ? m->row[r][m->nRow[r] - 1] : -1; }
static int WasteTop(const SolModel *m) { return m->nWaste ? m->waste[m->nWaste - 1] : -1; }

/* foundation index that accepts card c, or -1 (an empty one takes an ace, the first empty as the game does) */
static int FoundFor(const SolModel *m, int c)
{
    int i;

    for (i = 0; i < SM_FOUNDS; i++)
    {
        if (m->nFound[i])
        {
            int top = m->found[i][m->nFound[i] - 1];
            if (SmSuit(top) == SmSuit(c) && SmRank(c) == SmRank(top) + 1)
                return i;
        }
        else if (SmRank(c) == 1)
            return i;
    }
    return -1;
}

/* can the run that starts with card c go on row t? */
static int RowAccepts(const SolModel *m, int t, int c)
{
    int top = RowTop(m, t);

    if (top < 0)
        return SmRank(c) == 13;
    if (m->nDown[t] >= m->nRow[t])
        return 0;                                       /* a face-down top is not a target */
    return SmRank(top) == SmRank(c) + 1 && SmRed(top) != SmRed(c);
}

static void FlipIfNeeded(SolModel *m, int r)
{
    if (m->nRow[r] > 0 && m->nDown[r] >= m->nRow[r])
        m->nDown[r] = m->nRow[r] - 1;                   /* the exposed card turns over */
}

static void SetPile(SolModel *m)
{
    /* draw three shows the last `visible` cards fanned; with one visible card (or draw one) the game
     * keeps every drawn card in the pile (FixIfOneCardLeft) */
    int k, n = (m->three && m->visible > 1) ? m->visible : m->nWaste;

    if (n > m->nWaste) n = m->nWaste;
    m->nPile = n;
    for (k = 0; k < n; k++)
        m->pile[k] = m->waste[m->nWaste - n + k];
}

int SolApply(SolModel *m, const SolMove *mv)
{
    int i, c;

    if (!m || !mv)
        return 0;
    switch (mv->kind)
    {
    case SM_DRAW:
    {
        int n = m->three ? 3 : 1;
        if (!m->nDeck)
            return 0;
        if (n > m->nDeck) n = m->nDeck;
        for (i = 0; i < n; i++)
            m->waste[m->nWaste++] = m->deck[--m->nDeck];    /* popped in order, so reversed, as cardlib */
        m->visible = n;
        SetPile(m);
        return 1;
    }
    case SM_RECYCLE:
        if (m->nDeck || !m->nWaste)
            return 0;
        for (i = m->nWaste - 1; i >= 0; i--)
            m->deck[m->nDeck++] = m->waste[i];
        m->nWaste = 0; m->nPile = 0; m->visible = 0;
        m->wasteCount++;
        return 1;
    case SM_WASTE_FOUND:
        c = WasteTop(m);
        if (c < 0 || (i = FoundFor(m, c)) < 0)
            return 0;
        m->found[i][m->nFound[i]++] = c;
        m->nWaste--;
        if (m->visible > 1) m->visible--;
        SetPile(m);
        return 1;
    case SM_WASTE_ROW:
        c = WasteTop(m);
        if (c < 0 || !InRange(mv->to, 0, SM_ROWS - 1) || !RowAccepts(m, mv->to, c))
            return 0;
        m->row[mv->to][m->nRow[mv->to]++] = c;
        m->nWaste--;
        if (m->visible > 1) m->visible--;
        SetPile(m);
        return 1;
    case SM_ROW_FOUND:
        if (!InRange(mv->from, 0, SM_ROWS - 1) || m->nRow[mv->from] <= m->nDown[mv->from])
            return 0;
        c = RowTop(m, mv->from);
        if ((i = FoundFor(m, c)) < 0)
            return 0;
        m->found[i][m->nFound[i]++] = c;
        m->nRow[mv->from]--;
        FlipIfNeeded(m, mv->from);
        return 1;
    case SM_ROW_ROW:
    {
        int f = mv->from, t = mv->to, k = mv->count;
        if (!InRange(f, 0, SM_ROWS - 1) || !InRange(t, 0, SM_ROWS - 1) || f == t || k < 1 ||
            k > m->nRow[f] - m->nDown[f] || !RowAccepts(m, t, m->row[f][m->nRow[f] - k]))
            return 0;
        for (i = 0; i < k; i++)
            m->row[t][m->nRow[t]++] = m->row[f][m->nRow[f] - k + i];
        m->nRow[f] -= k;
        FlipIfNeeded(m, f);
        return 1;
    }
    }
    return 0;
}

/* ------------------------------------------------------------------ hint */

static void Consider(SolMove *best, int kind, int from, int to, int count, int score)
{
    if (score > best->score)
    {
        best->kind = kind; best->from = from; best->to = to; best->count = count; best->score = score;
    }
}

SolMove SolHint(const SolModel *m)
{
    SolMove best;
    int r, t, c, k, f;

    memset(&best, 0, sizeof(best));
    if (!m)
        return best;

    for (r = 0; r < SM_ROWS; r++)
    {
        int up = m->nRow[r] - m->nDown[r];

        if (up > 0 && (f = FoundFor(m, RowTop(m, r))) >= 0)
            Consider(&best, SM_ROW_FOUND, r, f, 1, 50 + ((up == 1 && m->nDown[r] > 0) ? 20 : 0));
        /* a run move is only worth a hint when it turns a face-down card over */
        if (up > 0 && m->nDown[r] > 0)
        {
            c = m->row[r][m->nDown[r]];                 /* the lowest face-up card = the whole run */
            for (t = 0; t < SM_ROWS; t++)
                if (t != r && RowAccepts(m, t, c))
                    Consider(&best, SM_ROW_ROW, r, t, up, 60);
        }
        /* a shorter run never reveals anything, but its top card may still be worth moving to
         * free a card the foundation or the waste needs: not hinted (kept simple and predictable) */
        (void)k;
    }
    if ((c = WasteTop(m)) >= 0)
    {
        if ((f = FoundFor(m, c)) >= 0)
            Consider(&best, SM_WASTE_FOUND, 0, f, 1, 45);
        for (t = 0; t < SM_ROWS; t++)
            if (RowAccepts(m, t, c))
                Consider(&best, SM_WASTE_ROW, 0, t, 1, 40);
    }
    if (best.kind == SM_NONE)
    {
        if (m->nDeck)
            Consider(&best, SM_DRAW, 0, 0, 0, 5);
        else if (m->nWaste)
            Consider(&best, SM_RECYCLE, 0, 0, 0, 3);
    }
    return best;
}

/* ------------------------------------------------------------------ autocomplete */

int SolAutoPlan(const SolModel *m, SolMove *plan)
{
    SolModel t;
    int r, n = 0, progress, done;

    if (!m || !plan || m->nDeck)
        return 0;
    for (r = 0; r < SM_ROWS; r++)
        if (m->nDown[r])
            return 0;
    t = *m;
    do
    {
        SolMove mv;
        int best = -1, bestRank = 99, c;

        progress = 0;
        memset(&mv, 0, sizeof(mv));
        mv.count = 1;
        if ((c = WasteTop(&t)) >= 0 && FoundFor(&t, c) >= 0)
        {
            best = -2;
            bestRank = SmRank(c);
        }
        for (r = 0; r < SM_ROWS; r++)
        {
            c = RowTop(&t, r);
            if (c >= 0 && FoundFor(&t, c) >= 0 && SmRank(c) < bestRank)
            {
                best = r;
                bestRank = SmRank(c);
            }
        }
        if (best == -2)
        {
            mv.kind = SM_WASTE_FOUND;
            mv.to = FoundFor(&t, WasteTop(&t));
        }
        else if (best >= 0)
        {
            mv.kind = SM_ROW_FOUND;
            mv.from = best;
            mv.to = FoundFor(&t, RowTop(&t, best));
        }
        if (mv.kind && n < SM_CARDS && SolApply(&t, &mv))
        {
            plan[n++] = mv;
            progress = 1;
        }
    } while (progress);

    done = 0;
    for (r = 0; r < SM_FOUNDS; r++)
        done += t.nFound[r];
    return done == SM_CARDS ? n : 0;
}
