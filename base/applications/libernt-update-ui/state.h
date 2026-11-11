/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     View model of the update window, read from libservicing
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windows.h>
#include <string>
#include <vector>

/* The header of the window; derived from the journal and the attempt, never stored (DESIGN.md section 3) */
enum class Head
{
    UpToDate,
    Available,
    Restart,
    Waiting,
    RolledBack,
    Failed,
    NotSetup
};

struct PackageRow
{
    std::string Id;
    std::string Version;
    std::string Type;
    size_t Files;
    std::string Phase;       /* newest journal phase of the package, "" when it was never applied */
    char TxId[65];           /* transaction that phase belongs to */
    DWORD VerifyError;       /* ERROR_SUCCESS when the stored copy verifies */
};

struct HistoryLine
{
    std::string Phase;
    std::string Result;
    std::string Error;
    std::string Time;
    long long Op;
};

struct HistoryRow
{
    std::string Id;
    std::string Version;
    std::string Outcome;     /* Succeeded, Rolled back, Failed, Waiting */
    std::string Time;
    std::vector<HistoryLine> Lines;
};

struct UpdateState
{
    Head Header = Head::NotSetup;
    DWORD Error = ERROR_SUCCESS;           /* why the state could not be read */
    std::string Identity;
    std::string Anchor;                    /* sha256 of the pinned certificate, "" when none */
    std::vector<PackageRow> Packages;      /* staged or applied packages */
    std::vector<HistoryRow> History;       /* newest first */
    bool TornJournal = false;
    bool PlanPending = false;              /* pending\plan.json waits for the next boot */
    bool JournalLocked = false;            /* the journal cannot be appended to: the boot applier would halt */
    int PackageForBanner = -1;             /* index in Packages of the package the header is about */
};

/* Reads everything; runs on the worker thread */
void ReadState(UpdateState &state, bool verify);

/* One action of the window (DESIGN.md section 1); returns a Win32 or LN_E_* code */
enum class Action
{
    Apply,
    Confirm,
    Rollback,
    Remove,
    AddPackage,
    InstallPackageExe,       /* a standalone package .exe: add-package --exe, then apply (one click) */
    ExportLog
};

DWORD RunAction(Action action, const std::string &id, const std::string &version, const std::wstring &path,
                bool *rebootRequired);

/* Plain sentence for an error code (DESIGN.md section 6) */
const wchar_t *ErrorSentence(DWORD error);
