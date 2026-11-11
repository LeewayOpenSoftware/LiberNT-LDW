/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Reads the servicing state and runs the actions through libservicing
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * No servicing logic lives here: every decision (verification, state rules, hashes) is libservicing's.
 */

#include "state.h"
#include <libservicing.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

/* The transaction ids a package can have: one per operation (servicing.c: install, apply, rollback) */
static const char *const Operations[] = { "install", "apply", "rollback" };

static const char *
OutcomeOf(const char *phase)
{
    if (!strcmp(phase, "confirmed"))
        return "Succeeded";
    if (!strcmp(phase, "rolled-back"))
        return "Rolled back";
    if (!strcmp(phase, "failed"))
        return "Failed";
    if (!strcmp(phase, "staged"))
        return "Staged";
    return "Waiting";
}

void
ReadState(UpdateState &state, bool verify)
{
    LN_TARGET target;
    LN_JOURNAL journal = { };
    char (*ids)[130] = NULL;
    size_t count = 0, i;
    char anchor[65];
    long long newest = -1;
    int newestPackage = -1;
    DWORD error;

    state = UpdateState();
    error = LnTargetOpen(&target, LnTargetOnline, NULL, FALSE);
    if (error)
    {
        state.Error = error;
        return;
    }
    state.Identity = target.identity;
    error = LnPinnedCertSha256(&target, anchor);
    if (error == ERROR_SUCCESS)
        state.Anchor = anchor;
    error = LnJournalRead(&target, &journal);
    if (error && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
        state.Error = error;
        state.Header = Head::NotSetup;
        return;
    }
    state.TornJournal = journal.tailTruncated != FALSE;

    /* A plan waits for the next boot; the applier journals its first phase before touching any file, so a journal
     * that cannot be appended to makes every boot halt (S1-BOOTSTRAP.md, decision A). Probe without creating. */
    {
        char *plan = NULL;

        if (LnServicingReadPendingPlan(&target, &plan) == ERROR_SUCCESS)
        {
            WCHAR *path = LnTargetPath(&target, L"journal\transactions.jsonl");

            state.PlanPending = true;
            if (path)
            {
                DWORD attrs = GetFileAttributesW(path);
                HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

                if (file != INVALID_HANDLE_VALUE)
                    CloseHandle(file);
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    ((attrs & FILE_ATTRIBUTE_READONLY) || file == INVALID_HANDLE_VALUE))
                    state.JournalLocked = true;
                free(path);
            }
        }
        free(plan);
    }

    error = LnStoreList(&target, &ids, &count);
    if (error && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        state.Error = error;
    for (i = 0; error == ERROR_SUCCESS && i < count; i++)
    {
        PackageRow row;
        LN_MANIFEST manifest;
        char *sep = strchr(ids[i], '\\');
        char idText[65], versionText[65];
        long long last = -1;
        WCHAR dummy = 0;

        UNREFERENCED_PARAMETER(dummy);
        if (!sep)
            continue;
        lstrcpynA(idText, ids[i], (int)std::min<size_t>((size_t)(sep - ids[i]) + 1, sizeof(idText)));
        lstrcpynA(versionText, sep + 1, sizeof(versionText));
        ZeroMemory(&manifest, sizeof(manifest));
        if (LnStoreLoadManifest(&target, idText, versionText, &manifest) != ERROR_SUCCESS)
            continue;
        row.Id = idText;
        row.Version = versionText;
        row.Type = manifest.type;
        row.Files = manifest.itemCount;
        row.TxId[0] = 0;
        row.VerifyError = ERROR_SUCCESS;
        for (size_t op = 0; op < _countof(Operations); op++)
        {
            char tx[65];

            LnTransactionId(target.identity, manifest.packageId, manifest.releaseVersion, manifest.manifestSha256,
                            Operations[op], tx);
            for (size_t e = 0; e < journal.count; e++)
            {
                if (!strcmp(journal.entries[e].transactionId, tx) && journal.entries[e].sequence > last)
                {
                    last = journal.entries[e].sequence;
                    row.Phase = journal.entries[e].phase;
                    lstrcpynA(row.TxId, tx, sizeof(row.TxId));
                }
            }
        }
        if (last > newest)
        {
            newest = last;
            newestPackage = (int)state.Packages.size();
        }
        if (verify)
        {
            LN_MANIFEST checked;

            ZeroMemory(&checked, sizeof(checked));
            row.VerifyError = LnServicingVerifyStored(&target, idText, versionText, &checked);
            LnManifestFree(&checked);
        }
        LnManifestFree(&manifest);
        state.Packages.push_back(row);
    }
    free(ids);

    /* History: one row per transaction, newest first */
    for (size_t p = 0; p < state.Packages.size(); p++)
    {
        const PackageRow &pkg = state.Packages[p];

        for (size_t op = 0; op < _countof(Operations); op++)
        {
            HistoryRow row;
            LN_MANIFEST manifest;
            char tx[65];
            long long lastSeq = -1;
            bool any = false;

            ZeroMemory(&manifest, sizeof(manifest));
            if (LnStoreLoadManifest(&target, pkg.Id.c_str(), pkg.Version.c_str(), &manifest) != ERROR_SUCCESS)
                continue;
            LnTransactionId(target.identity, manifest.packageId, manifest.releaseVersion, manifest.manifestSha256,
                            Operations[op], tx);
            LnManifestFree(&manifest);
            row.Id = pkg.Id;
            row.Version = pkg.Version;
            for (size_t e = 0; e < journal.count; e++)
            {
                const LN_JENTRY &entry = journal.entries[e];
                HistoryLine line;

                if (strcmp(entry.transactionId, tx))
                    continue;
                any = true;
                line.Phase = entry.phase;
                line.Result = entry.result;
                line.Error = entry.error;
                line.Time = entry.timestampUtc;
                line.Op = entry.op;
                row.Lines.push_back(line);
                if (entry.sequence > lastSeq)
                {
                    lastSeq = entry.sequence;
                    row.Outcome = OutcomeOf(entry.phase);
                    row.Time = entry.timestampUtc;
                }
            }
            if (any)
                state.History.push_back(row);
        }
    }
    std::sort(state.History.begin(), state.History.end(),
              [](const HistoryRow &a, const HistoryRow &b) { return a.Time > b.Time; });
    LnJournalFree(&journal);

    /* The header */
    if (state.Anchor.empty())
    {
        state.Header = Head::NotSetup;
        return;
    }
    state.Header = Head::UpToDate;
    for (size_t p = 0; p < state.Packages.size(); p++)
    {
        const std::string &phase = state.Packages[p].Phase;

        if (phase == "pending-first-boot")
        {
            state.Header = Head::Restart;
            state.PackageForBanner = (int)p;
            return;
        }
    }
    for (size_t p = 0; p < state.Packages.size(); p++)
    {
        const std::string &phase = state.Packages[p].Phase;

        if (phase == "applied")
        {
            state.Header = Head::Waiting;
            state.PackageForBanner = (int)p;
            return;
        }
    }
    if (newestPackage >= 0)
    {
        const std::string &phase = state.Packages[newestPackage].Phase;

        if (phase == "rolled-back")
        {
            state.Header = Head::RolledBack;
            state.PackageForBanner = newestPackage;
            return;
        }
        if (phase == "failed")
        {
            state.Header = Head::Failed;
            state.PackageForBanner = newestPackage;
            return;
        }
    }
    for (size_t p = 0; p < state.Packages.size(); p++)
    {
        const std::string &phase = state.Packages[p].Phase;

        if (phase.empty() || phase == "staged" || phase == "rolled-back" || phase == "failed")
        {
            state.Header = Head::Available;
            return;
        }
    }
}

DWORD
RunAction(Action action, const std::string &id, const std::string &version, const std::wstring &path,
          bool *rebootRequired)
{
    LN_TARGET target;
    char tx[65];
    DWORD error;

    error = LnTargetOpen(&target, LnTargetOnline, NULL, action == Action::AddPackage || action == Action::InstallPackageExe);
    if (error)
        return error;
    switch (action)
    {
        case Action::Apply:
        {
            BOOL reboot = FALSE;

            error = LnServicingApplyPackage(&target, id.c_str(), version.c_str(), &reboot, tx);
            if (rebootRequired)
                *rebootRequired = reboot != FALSE;
            return error;
        }
        case Action::Confirm:
            return LnServicingConfirmHealth(&target, id.c_str(), version.c_str());
        case Action::Rollback:
            return LnServicingRollback(&target, id.c_str(), version.c_str(), tx);
        case Action::Remove:
            return LnServicingRemovePackage(&target, id.c_str(), version.c_str());
        case Action::AddPackage:
            return LnServicingAddPackage(&target, path.c_str(), tx);
        case Action::InstallPackageExe:
        {
            /* the id and version come from the package itself: its manifest, read through the table hash */
            char *json = NULL;
            size_t len = 0;
            LN_MANIFEST manifest;
            BOOL reboot = FALSE;

            error = LnPackageExeInspect(path.c_str(), &json, &len);
            if (error)
                return error;
            ZeroMemory(&manifest, sizeof(manifest));
            error = LnManifestParse(json, len, &manifest);
            free(json);
            if (error)
                return error;
            error = LnServicingAddPackageExe(&target, path.c_str(), tx);
            if (error == ERROR_SUCCESS)
                error = LnServicingApplyPackage(&target, manifest.packageId, manifest.releaseVersion, &reboot, tx);
            LnManifestFree(&manifest);
            if (rebootRequired)
                *rebootRequired = reboot != FALSE;
            return error;
        }
        case Action::ExportLog:
            return LnServicingExportLog(&target, path.c_str());
    }
    return ERROR_INVALID_PARAMETER;
}

const wchar_t *
ErrorSentence(DWORD error)
{
    switch (error)
    {
        case ERROR_SUCCESS:
            return L"";
        case LN_E_UNSIGNED:
        case LN_E_BAD_SIGNATURE:
        case LN_E_CATALOG_INVALID:
        case LN_E_CERT_MISMATCH:
            return L"This package is not signed by a trusted source.";
        case LN_E_HASH_MISMATCH:
        case LN_E_VERIFY_FINAL:
            return L"A file does not match what was signed.";
        case LN_E_PRECONDITION:
            return L"This update does not fit the current system.";
        case LN_E_FORBIDDEN_TARGET:
            return L"This update touches something that cannot be updated.";
        case LN_E_BAD_STATE:
            return L"This can't be done in the current state.";
        case LN_E_NO_TRUST_ANCHOR:
            return L"No trusted source is set up.";
        case LN_E_NO_BOOT_APPLIER:
            return L"The boot updater is not installed.";
        case LN_E_INSECURE_TARGET:
            return L"The update folder is not protected.";
        case LN_E_JOURNAL_CORRUPT:
            return L"The update history is damaged.";
        case LN_E_MANIFEST_INVALID:
            return L"The package description is not valid.";
        case LN_E_PACKAGE_INVALID:
            return L"This file is not a LiberNT update package, or it is damaged.";
        case LN_E_NOT_APPLICABLE:
            return L"This update needs another update that is not installed.";
        case LN_E_SUPERSEDED:
            return L"A newer update already replaces this one.";
        default:
            return L"Something went wrong.";
    }
}
