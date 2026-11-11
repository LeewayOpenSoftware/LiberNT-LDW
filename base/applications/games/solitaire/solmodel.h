/*
 * PROJECT:     Solitaire
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pure game model of Klondike: save/resume packing, hints, autocomplete plan
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * No window, no cardlib: cards are plain 0..51 indices (cardlib Card::Idx: suit = idx % 4,
 * rank = idx / 4 + 1, ace = 1, king = 13; suits 0 and 3 are black). Stack order is the cardlib
 * order: the last element is the top (the card that is drawn, played or exposed).
 */

#ifndef SOLMODEL_H
#define SOLMODEL_H

#define SM_ROWS 7
#define SM_FOUNDS 4
#define SM_CARDS 52

struct SolModel {
    int nDeck, deck[SM_CARDS];              /* the stock; the last card is drawn next */
    int nWaste, waste[SM_CARDS];            /* every drawn card still in play (cardlib activepile) */
    int nPile, pile[SM_CARDS];              /* what the face-up pile shows (the exposed end of waste) */
    int visible;                            /* VisiblePileCards */
    int three;                              /* 1 = draw three, 0 = draw one */
    int nFound[SM_FOUNDS], found[SM_FOUNDS][13];
    int nRow[SM_ROWS], row[SM_ROWS][SM_CARDS];
    int nDown[SM_ROWS];                     /* the first nDown cards of a row are face down */
    long score;
    unsigned time, wasteCount, wasteThreshold;
    unsigned elapsed;                       /* game clock of the shell, seconds (statistics) */
};

inline int SmRank(int c) { return c / 4 + 1; }
inline int SmSuit(int c) { return c % 4; }
inline int SmRed(int c) { return SmSuit(c) == 1 || SmSuit(c) == 2; }

/* ---- save/resume payload (the leewayui save blob wraps it with magic, version and CRC) ---- */
#define SM_PACK_MAX 512
/* Returns the byte count, 0 if the model is not a legal game or cap < SM_PACK_MAX. */
unsigned SolPack(const SolModel *m, unsigned char *out, unsigned cap);
/* Returns 1 and fills *m only when the bytes are a complete legal game (52 distinct cards in
 * play, foundations ascending per suit, pile the end of the waste, counts in range, exact length). */
int SolUnpack(const unsigned char *in, unsigned size, SolModel *m);
int SolModelValid(const SolModel *m);

/* ---- moves, hints, autocomplete ---- */
enum SolMoveKind {
    SM_NONE = 0,
    SM_DRAW,            /* click the stock */
    SM_RECYCLE,         /* click the empty stock: the waste goes back */
    SM_WASTE_FOUND,     /* waste top to a foundation */
    SM_WASTE_ROW,       /* waste top to row `to` */
    SM_ROW_FOUND,       /* row `from` top to a foundation */
    SM_ROW_ROW          /* the top `count` cards of row `from` to row `to` */
};

struct SolMove {
    int kind;
    int from;           /* row index for ROW_* */
    int to;             /* row index for *_ROW, foundation index for *_FOUND */
    int count;          /* cards moved */
    int score;          /* ranking value (hints) */
};

/* Applies a move to the model (rules only, no animation). 0 if illegal. */
int SolApply(SolModel *m, const SolMove *mv);
/* Best next move for the player, SM_NONE when there is nothing (stock and waste exhausted, no move). */
SolMove SolHint(const SolModel *m);
/* Autocomplete: only offered when the stock is empty, no card is face down and the remaining
 * cards can all be played to the foundations in order. Fills plan (room for SM_CARDS moves)
 * and returns the move count, 0 when not available. */
int SolAutoPlan(const SolModel *m, SolMove *plan);

#endif
