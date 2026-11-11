/*
 * PROJECT:     Minesweeper
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pure board logic: level limits, safe first click mine placement, save/resume packing
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <string.h>
#include "mbcore.h"

void MbClampLevel(unsigned *rows, unsigned *cols, unsigned *mines)
{
    unsigned maxmines;

    if (*rows < MB_MIN_SIDE) *rows = MB_MIN_SIDE;
    if (*rows > MB_MAX_ROWS) *rows = MB_MAX_ROWS;
    if (*cols < MB_MIN_SIDE) *cols = MB_MIN_SIDE;
    if (*cols > MB_MAX_COLS) *cols = MB_MAX_COLS;
    maxmines = *cols * *rows - 9;
    if (*mines < MB_MIN_MINES) *mines = MB_MIN_MINES;
    if (*mines > maxmines) *mines = maxmines;
}

static unsigned Next(unsigned *seed)
{
    unsigned x = *seed ? *seed : 0x9E3779B9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

#define CELL(col, row) grid[(col) * stride + (row)]

int MbPlaceMines(unsigned char *grid, int stride, int cols, int rows, int mines, int sel_col, int sel_row,
                 unsigned *seed)
{
    int c, r, placed = 0, area;
    int free_zone;
    unsigned st = seed && *seed ? *seed : 0x2545F491u;

    /* clean grid incl. border */
    for (c = 0; c <= cols + 1; c++)
        for (r = 0; r <= rows + 1; r++)
            CELL(c, r) = 0;

    /* cells that may not hold a mine: the clicked cell, and its neighbours when the board has room */
    area = 0;
    for (c = sel_col - 1; c <= sel_col + 1; c++)
        for (r = sel_row - 1; r <= sel_row + 1; r++)
            if (c >= 1 && c <= cols && r >= 1 && r <= rows)
                area++;
    free_zone = (cols * rows - area >= mines);
    if (!free_zone)
        area = 1;
    if (mines > cols * rows - area)
        mines = cols * rows - area;              /* cannot happen for a clamped level */

    while (placed < mines)
    {
        c = (int)(Next(&st) % (unsigned)cols) + 1;
        r = (int)(Next(&st) % (unsigned)rows) + 1;
        if (CELL(c, r))
            continue;
        if (free_zone ? (c >= sel_col - 1 && c <= sel_col + 1 && r >= sel_row - 1 && r <= sel_row + 1)
                      : (c == sel_col && r == sel_row))
            continue;
        CELL(c, r) = 1;
        placed++;
    }
    if (seed)
        *seed = st;
    return free_zone;
}

int MbCountAround(const unsigned char *grid, int stride, int col, int row)
{
    int i, j, n = 0;

    for (i = -1; i <= 1; i++)
        for (j = -1; j <= 1; j++)
            n += grid[(col + i) * stride + (row + j)] & MB_CELL_MINE;
    return n;
}

/* ------------------------------------------------------------------ save/resume */

#define MB_PACK_VERSION 1

static void Put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static unsigned Get32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* walks the playing field; returns 0 when a cell breaks the rules */
static int Scan(const unsigned char *cell, unsigned cols, unsigned rows, unsigned *mines, unsigned *revealed)
{
    unsigned c, r, stride = rows + 2;

    *mines = *revealed = 0;
    for (c = 1; c <= cols; c++)
        for (r = 1; r <= rows; r++)
        {
            unsigned char v = cell[c * stride + r];
            if (v & ~(MB_CELL_MINE | MB_CELL_FLAG_MASK))
                return 0;
            if (v & MB_CELL_MINE)
                (*mines)++;
            if (((v & MB_CELL_FLAG_MASK) >> MB_CELL_FLAG_SHIFT) == 3)
            {
                if (v & MB_CELL_MINE)
                    return 0;                   /* a revealed mine is a lost game, not a saved one */
                (*revealed)++;
            }
        }
    return 1;
}

unsigned MbPack(const MbSave *s, unsigned char *out, unsigned cap)
{
    unsigned c, r, n = 0, mines, revealed, stride;

    if (!s || !out || s->cols < MB_MIN_SIDE || s->cols > MB_MAX_COLS || s->rows < MB_MIN_SIDE ||
        s->rows > MB_MAX_ROWS || s->mines < MB_MIN_MINES || s->mines > s->cols * s->rows - 9 || s->difficulty > 3 ||
        s->time > 999)
        return 0;
    if (cap < 16 + s->cols * s->rows)
        return 0;
    if (!Scan(s->cell, s->cols, s->rows, &mines, &revealed) || mines != s->mines || revealed == 0)
        return 0;
    stride = s->rows + 2;
    out[n++] = MB_PACK_VERSION;
    out[n++] = (unsigned char)s->cols;
    out[n++] = (unsigned char)s->rows;
    out[n++] = (unsigned char)s->difficulty;
    Put32(out + n, s->mines); n += 4;
    Put32(out + n, s->time); n += 4;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
    for (c = 1; c <= s->cols; c++)
        for (r = 1; r <= s->rows; r++)
            out[n++] = s->cell[c * stride + r];
    return n;
}

int MbUnpack(const unsigned char *in, unsigned size, MbSave *s)
{
    MbSave t;
    unsigned c, r, n = 16, mines, revealed, stride;

    if (!in || !s || size < 16 || in[0] != MB_PACK_VERSION)
        return 0;
    memset(&t, 0, sizeof(t));
    t.cols = in[1];
    t.rows = in[2];
    t.difficulty = in[3];
    t.mines = Get32(in + 4);
    t.time = Get32(in + 8);
    if (t.cols < MB_MIN_SIDE || t.cols > MB_MAX_COLS || t.rows < MB_MIN_SIDE || t.rows > MB_MAX_ROWS ||
        t.difficulty > 3 || t.mines < MB_MIN_MINES || t.mines > t.cols * t.rows - 9 || t.time > 999)
        return 0;
    if (size != 16 + t.cols * t.rows)
        return 0;
    stride = t.rows + 2;
    for (c = 1; c <= t.cols; c++)
        for (r = 1; r <= t.rows; r++)
            t.cell[c * stride + r] = in[n++];
    if (!Scan(t.cell, t.cols, t.rows, &mines, &revealed) || mines != t.mines || revealed == 0)
        return 0;
    *s = t;
    return 1;
}
