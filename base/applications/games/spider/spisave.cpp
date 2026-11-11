/*
 * PROJECT:      Spider Solitaire
 * LICENSE:      GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:      Save/resume of the open game on the leewayui game shell, status text
 * COPYRIGHT:    Copyright 2026 Leeway Open Software
 *
 * Payload (the shell wraps it with magic, version, game tag and CRC-32):
 *   byte  version (1)
 *   byte  difficulty: 1, 2 or 4 suits
 *   byte  cards finished (0, 13, ... 104)
 *   byte  cards left in the fake deck (0..5)
 *   byte  cards left in the real deck (a multiple of 10)
 *   u32   elapsed seconds
 *   10 x  { byte count, byte face down }
 *   then the card indices (0..51): deck, then stack 0..9, each bottom first
 * A payload is accepted only when every count is in range and the cards add up to 104.
 */

#include "spider.h"

#include <stdio.h>
#include <string.h>

extern CardStack deck;
extern CardRegion *pDeck;
extern CardRegion *pStack[];
extern int cardsFinished;

#define SPI_SAVE_VERSION 1
#define SPI_TOTAL        104
#define SPI_MAX_BLOB     (5 + 4 + NUM_STACKS * 2 + SPI_TOTAL)

static const WCHAR *VariantName(DWORD difficulty)
{
    switch (difficulty)
    {
    case IDC_DIF_TWOCOLORS:  return L"2Suits";
    case IDC_DIF_FOURCOLORS: return L"4Suits";
    default:                 return L"1Suit";
    }
}

void SpiSetVariant(void)
{
    LwgShellSetVariant(g_shell, VariantName(dwDifficulty));
}

void SpiUpdateStatus(void)
{
    WCHAR text[96];
    UINT32 t = LwgShellElapsedSeconds(g_shell);

    if (!g_shell)
        return;
    _snwprintf(text, 96, L"Time %u:%02u    Suits completed: %d of 8", t / 60, t % 60, cardsFinished / 13);
    text[95] = 0;
    LeewayFrameSetStatusText(LwgShellGetFrame(g_shell), 0, text);
}

static void PutCards(BYTE *out, UINT *n, const CardStack &cs)
{
    int k, count = cs.NumCards();

    for (k = 0; k < count; k++)
        out[(*n)++] = (BYTE)cs[count - 1 - k].Idx();     /* bottom first */
}

void SpiSaveGame(void)
{
    BYTE blob[SPI_MAX_BLOB];
    UINT n = 0, total;
    int i, down, fake = pDeck->NumCards();
    UINT32 t = LwgShellElapsedSeconds(g_shell);

    if (!g_shell || !fGameStarted)
        return;
    total = (UINT)(deck.NumCards() + cardsFinished);
    for (i = 0; i < NUM_STACKS; i++)
        total += (UINT)pStack[i]->NumCards();
    if (total != SPI_TOTAL || fake > 5 || deck.NumCards() % 10 || cardsFinished % 13)
        return;                                          /* not a state we can restore: do not save */

    blob[n++] = SPI_SAVE_VERSION;
    blob[n++] = dwDifficulty == IDC_DIF_TWOCOLORS ? 2 : (dwDifficulty == IDC_DIF_FOURCOLORS ? 4 : 1);
    blob[n++] = (BYTE)cardsFinished;
    blob[n++] = (BYTE)fake;
    blob[n++] = (BYTE)deck.NumCards();
    blob[n++] = (BYTE)t; blob[n++] = (BYTE)(t >> 8); blob[n++] = (BYTE)(t >> 16); blob[n++] = (BYTE)(t >> 24);
    for (i = 0; i < NUM_STACKS; i++)
    {
        down = 0;
        pStack[i]->GetFaceDirection(&down);
        blob[n++] = (BYTE)pStack[i]->NumCards();
        blob[n++] = (BYTE)(down > pStack[i]->NumCards() ? pStack[i]->NumCards() : down);
    }
    PutCards(blob, &n, deck);
    for (i = 0; i < NUM_STACKS; i++)
        PutCards(blob, &n, pStack[i]->GetCardStack());
    LwgShellSave(g_shell, blob, n);
}

static bool Parse(const BYTE *b, UINT size)
{
    UINT n = 0, need, pos;
    int i, j, diff, finished, fake, deckCount, count[NUM_STACKS], down[NUM_STACKS];
    UINT32 elapsed;
    CardStack newDeck, stacks[NUM_STACKS];
    UINT total;

    if (size < 5 + 4 + NUM_STACKS * 2 || b[0] != SPI_SAVE_VERSION)
        return false;
    diff = b[1]; finished = b[2]; fake = b[3]; deckCount = b[4];
    elapsed = (UINT32)b[5] | ((UINT32)b[6] << 8) | ((UINT32)b[7] << 16) | ((UINT32)b[8] << 24);
    n = 9;
    if ((diff != 1 && diff != 2 && diff != 4) || finished > SPI_TOTAL || finished % 13 || fake > 5 ||
        deckCount > SPI_TOTAL || deckCount % 10 || deckCount != fake * 10)
        return false;
    total = (UINT)(finished + deckCount);
    for (i = 0; i < NUM_STACKS; i++)
    {
        count[i] = b[n++];
        down[i] = b[n++];
        if (down[i] > count[i])
            return false;
        total += (UINT)count[i];
    }
    need = n + (UINT)deckCount;
    for (i = 0; i < NUM_STACKS; i++)
        need += (UINT)count[i];
    if (total != SPI_TOTAL || need != size)
        return false;
    for (pos = n; pos < size; pos++)
        if (b[pos] > 51)
            return false;

    /* all checks passed: build, then apply */
    newDeck.Clear();
    for (j = 0; j < deckCount; j++)
        newDeck.Push(Card(b[n++]));
    for (i = 0; i < NUM_STACKS; i++)
    {
        stacks[i].Clear();
        for (j = 0; j < count[i]; j++)
            stacks[i].Push(Card(b[n++]));
    }
    SpiderWnd.EmptyStacks();
    dwDifficulty = diff == 2 ? IDC_DIF_TWOCOLORS : (diff == 4 ? IDC_DIF_FOURCOLORS : IDC_DIF_ONECOLOR);
    deck = newDeck;
    cardsFinished = finished;
    for (i = 0; i < NUM_STACKS; i++)
    {
        pStack[i]->SetFaceDirection(CS_FACE_DOWNUP, down[i]);
        pStack[i]->SetCardStack(stacks[i]);
    }
    {
        CardStack fakeDeck;                              /* only the number of cards matters */
        fakeDeck.Clear();
        for (j = 0; j < fake; j++)
            fakeDeck.Push(Card(0));
        pDeck->SetCardStack(fakeDeck);
    }
    pDeck->Update();
    SpiSetVariant();
    GameStarted();
    LwgShellSetElapsedSeconds(g_shell, elapsed);
    SpiderWnd.Redraw();
    SpiUpdateStatus();
    return true;
}

bool SpiResumeGame(void)
{
    void *data = NULL;
    UINT32 size = 0;
    bool ok = false;

    if (!g_shell || LwgShellResume(g_shell, &data, &size) != LWG_SAVE_OK)
    {
        LwgFree(data);
        return false;
    }
    ok = Parse((const BYTE *)data, size);
    if (!ok)
        LwgShellDiscardSave(g_shell);
    LwgFree(data);
    return ok;
}
