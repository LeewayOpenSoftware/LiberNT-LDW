/*
 * PROJECT:     Solitaire
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Glue between the live cardlib game and the pure model: save/resume, hint, autocomplete, table colour
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "solitaire.h"
#include "solmodel.h"
#include "resource.h"

#include <string.h>

extern bool bAutoroute;
extern int LastId;

/* ------------------------------------------------------------------ snapshot / restore */

static int ToModel(const CardStack &cs, int *out)
{
    int n = cs.NumCards(), k;

    for (k = 0; k < n; k++)
        out[k] = cs[n - 1 - k].Idx();           /* cardlib index 0 is the top; the model lists bottom first */
    return n;
}

static CardStack ToStack(const int *in, int n)
{
    CardStack cs;
    int k;

    cs.Clear();
    for (k = 0; k < n; k++)
        cs.Push(Card(in[k]));
    return cs;
}

static bool Snapshot(SolModel *m)
{
    int i, down;

    memset(m, 0, sizeof(*m));
    m->nDeck = ToModel(pDeck->GetCardStack(), m->deck);
    m->nWaste = ToModel(activepile, m->waste);
    m->nPile = ToModel(pPile->GetCardStack(), m->pile);
    m->visible = VisiblePileCards;
    m->three = (dwOptions & OPTION_THREE_CARDS) ? 1 : 0;
    for (i = 0; i < 4; i++)
        m->nFound[i] = ToModel(pSuitStack[i]->GetCardStack(), m->found[i]);
    for (i = 0; i < NUM_ROW_STACKS; i++)
    {
        m->nRow[i] = ToModel(pRowStack[i]->GetCardStack(), m->row[i]);
        down = 0;
        pRowStack[i]->GetFaceDirection(&down);
        m->nDown[i] = down < 0 ? 0 : (down > m->nRow[i] ? m->nRow[i] : down);
    }
    m->score = lScore;
    m->time = dwTime;
    m->wasteCount = dwWasteCount;
    m->wasteThreshold = dwWasteTreshold;
    m->elapsed = LwgShellElapsedSeconds(g_shell);
    return SolModelValid(m) != 0;
}

static void Restore(const SolModel *m)
{
    int i;

    SolWnd.EmptyStacks();
    if (m->three)
        dwOptions |= OPTION_THREE_CARDS;
    else
        dwOptions &= ~OPTION_THREE_CARDS;
    lScore = m->score;
    dwTime = m->time;
    dwWasteCount = m->wasteCount;
    dwWasteTreshold = m->wasteThreshold;
    VisiblePileCards = m->visible;
    LastId = 0;
    activepile = ToStack(m->waste, m->nWaste);

    pDeck->SetCardStack(ToStack(m->deck, m->nDeck));
    // the pile is fanned only while three cards are shown (DeckClickProc / FixIfOneCardLeft)
    if (m->three && m->visible > 1)
        pPile->SetOffsets(CS_DEFXOFF, 1);
    else
        pPile->SetOffsets(0, 0);
    pPile->SetCardStack(ToStack(m->pile, m->nPile));
    for (i = 0; i < 4; i++)
        pSuitStack[i]->SetCardStack(ToStack(m->found[i], m->nFound[i]));
    for (i = 0; i < NUM_ROW_STACKS; i++)
    {
        pRowStack[i]->SetFaceDirection(CS_FACE_DOWNUP, m->nDown[i]);
        pRowStack[i]->SetCardStack(ToStack(m->row[i], m->nRow[i]));
    }
    pDeck->Update();
    dwPrevMode = GetScoreMode();
    ClearUndo();
}

// Saves the open game so the next start resumes it. Nothing is written for an untouched deal.
void SolSaveGame(void)
{
    SolModel m;
    unsigned char packed[SM_PACK_MAX];
    unsigned n;

    if (!g_shell || !fGameStarted || !Snapshot(&m))
        return;
    n = SolPack(&m, packed, sizeof(packed));
    if (n)
        LwgShellSave(g_shell, packed, n);
}

// Resumes the saved game. FALSE (and the saved data is dropped) when there is none or it is not a legal game.
bool SolResumeGame(void)
{
    void *data = NULL;
    UINT32 size = 0;
    SolModel m;
    bool ok = false;

    if (!g_shell || LwgShellResume(g_shell, &data, &size) != LWG_SAVE_OK)
    {
        LwgFree(data);
        return false;
    }
    if (SolUnpack((const unsigned char *)data, size, &m))
    {
        Restore(&m);
        LwgShellSetVariant(g_shell, m.three ? L"Draw3" : L"Draw1");
        GameStarted();
        LwgShellSetElapsedSeconds(g_shell, m.elapsed);
        SolWnd.Redraw();
        UpdateStatusBar();
        ok = true;
    }
    else
    {
        LwgShellDiscardSave(g_shell);
    }
    LwgFree(data);
    return ok;
}

/* ------------------------------------------------------------------ hint */

static CardRegion *SourceRegion(const SolMove &mv)
{
    switch (mv.kind)
    {
    case SM_DRAW:
    case SM_RECYCLE:
        return pDeck;
    case SM_WASTE_FOUND:
    case SM_WASTE_ROW:
        return pPile;
    default:
        return pRowStack[mv.from];
    }
}

static CardRegion *TargetRegion(const SolMove &mv)
{
    switch (mv.kind)
    {
    case SM_WASTE_FOUND:
    case SM_ROW_FOUND:
        return pSuitStack[mv.to];
    case SM_WASTE_ROW:
    case SM_ROW_ROW:
        return pRowStack[mv.to];
    default:
        return NULL;
    }
}

// Flashes the source and the target of the best next move.
void SolShowHint(void)
{
    SolModel m;
    SolMove mv;

    if (!Snapshot(&m))
        return;
    mv = SolHint(&m);
    if (mv.kind == SM_NONE)
    {
        MessageBox(hwndMain, TEXT("There are no more moves."), szAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    GameStarted();
    SourceRegion(mv)->Flash(6, 120);
    if (TargetRegion(mv))
        TargetRegion(mv)->Flash(6, 120);
}

/* ------------------------------------------------------------------ autocomplete */

// Plays the remaining cards to the foundations with the usual card animation.
void SolAutocomplete(void)
{
    SolModel m;
    SolMove plan[SM_CARDS + 4];
    int n, i;

    if (!Snapshot(&m) || (n = SolAutoPlan(&m, plan)) == 0)
    {
        MessageBox(hwndMain, TEXT("Autocomplete is available when every card is face up and the deck is empty."),
                   szAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    GameStarted();
    SetPlayTimer();
    for (i = 0; i < n; i++)
    {
        CardRegion *src = SourceRegion(plan[i]);
        CardRegion *dst = TargetRegion(plan[i]);

        bAutoroute = true;
        src->SimulateDrag(dst, 1, true);
        bAutoroute = false;
        if (!fGameStarted)      // the last card won the game and a new deal was started or the table cleared
            break;
    }
}

/* ------------------------------------------------------------------ table colour */

static const COLORREF TableColors[SOL_NUM_TABLES] = {
    PALETTERGB(0, 80, 0),       // green felt (default)
    PALETTERGB(0, 58, 100),     // blue
    PALETTERGB(96, 20, 40),     // burgundy
    PALETTERGB(52, 62, 70),     // slate
    PALETTERGB(20, 20, 20)      // black
};

DWORD dwTable = 0;

void SolSetTable(DWORD index)
{
    if (index >= SOL_NUM_TABLES)
        index = 0;
    dwTable = index;
    SolWnd.SetBackColor(TableColors[index]);
    SolWnd.Redraw();
    if (hGameMenu)
    {
        // the colours sit in a "Table" popup inside the Game menu
        int i, count = GetMenuItemCount(hGameMenu);

        for (i = 0; i < count; i++)
        {
            HMENU sub = GetSubMenu(hGameMenu, i);
            if (sub && GetMenuItemID(sub, 0) == (UINT)IDM_TABLE_FIRST)
                CheckMenuRadioItem(sub, IDM_TABLE_FIRST, IDM_TABLE_FIRST + SOL_NUM_TABLES - 1,
                                   IDM_TABLE_FIRST + index, MF_BYCOMMAND);
        }
    }
}
