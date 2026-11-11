/*
 * PROJECT:     Minesweeper
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pure board logic: level limits, safe first click mine placement, save/resume packing
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * No window, no GDI. A board is a byte grid with a one cell border: cell (col,row), both 1-based,
 * lives at grid[col * stride + row], stride = rows + 2. The border cells are always 0.
 */

#ifndef MBCORE_H
#define MBCORE_H

#define MB_MIN_SIDE   9
#define MB_MAX_COLS   99
#define MB_MAX_ROWS   99
#define MB_MIN_MINES  10

/* Clamps a custom board to the legal range: both sides 9..99, mines 10..cols*rows-9 (so the
 * first click always has a mine-free 3x3 area). */
void MbClampLevel(unsigned *rows, unsigned *cols, unsigned *mines);

/* Places exactly `mines` mines. The clicked cell and its neighbours stay free when
 * mines <= cols*rows-9 (always true for a clamped level); on a denser board only the clicked cell
 * is guaranteed free. Returns 1 when the 3x3 area was kept free, 0 when only the cell was.
 * `seed` is the state of a private generator (any value; 0 is replaced). Cells are overwritten:
 * 1 = mine, 0 = no mine. */
int MbPlaceMines(unsigned char *grid, int stride, int cols, int rows, int mines, int sel_col, int sel_row,
                 unsigned *seed);

/* number of mines in the 3x3 block around (col,row), the cell itself included (as the game labels) */
int MbCountAround(const unsigned char *grid, int stride, int col, int row);

/* ---- save/resume of a game in progress ---- */
#define MB_CELL_MINE      0x01
#define MB_CELL_FLAG_MASK 0x06      /* 0 normal, 1 question, 2 flag, 3 revealed */
#define MB_CELL_FLAG_SHIFT 1
#define MB_PACK_MAX (16 + MB_MAX_COLS * MB_MAX_ROWS)

typedef struct {
    unsigned cols, rows, mines, difficulty, time;
    unsigned char cell[(MB_MAX_COLS + 2) * (MB_MAX_ROWS + 2)];   /* same grid layout, flags in bits 1-2 */
} MbSave;

/* Returns the byte count, 0 when the game cannot be saved (illegal sizes, mine count, a revealed mine). */
unsigned MbPack(const MbSave *s, unsigned char *out, unsigned cap);
/* Returns 1 and fills *s only for a complete legal game: exact length, sizes and mine count in
 * range, mine count in the cells equal to `mines`, no revealed mine, at least one revealed cell. */
int MbUnpack(const unsigned char *in, unsigned size, MbSave *s);

#endif
