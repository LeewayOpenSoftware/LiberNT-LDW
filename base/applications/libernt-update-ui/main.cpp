/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LiberNT Update window: status header, packages, history, roll back
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Layout and calls: shared/leeway/libernt-update/ui/DESIGN.md. Same frame, fonts and palette as SystemSettings.
 */

#include "state.h"
#include <shellframe.h>
#include <shlobj.h>
#include <commdlg.h>
#include <strsafe.h>
#include <shlwapi.h>
#include <algorithm>

using namespace ShellFrame;

#define WM_UPDATE_STATE  (WM_APP + 1)
#define WM_UPDATE_ACTION (WM_APP + 2)

enum class Page
{
    Main,
    History,
    Options
};

enum class HitKind
{
    None,
    Primary,
    Collapse,
    ApplyRow,
    RemoveRow,
    AddPackage,
    InstallPackage,
    UninstallRow,
    ConfirmBanner,
    OpenHistory,
    OpenOptions,
    RollbackRow,
    ExportLog,
    Back,
    HistoryRow
};

struct Hit
{
    RECT Rect;
    HitKind Kind;
    int Arg;
};

struct ActionJob
{
    HWND Window;
    Action What;
    std::string Id, Version;
    std::wstring Path;
    bool Reboot;
};

class UpdateFrame : public FrameWindow
{
public:
    UpdateFrame() : m_Page(Page::Main), m_Busy(false), m_Collapsed(false), m_Scroll(0), m_ContentHeight(0), m_Hot(-1),
                     m_Pressed(-1), m_Expanded(-1), m_Notice(L""), m_NoticeError(false), m_Checked(false)
    {
        m_Checked = false;
        m_LastChecked[0] = UNICODE_NULL;
    }

protected:
    /* the standard system caption: this window draws none of it */
    BOOL UseSystemCaption() const override { return TRUE; }
    int CaptionHeight() const override { return 0; }

    void OnCreate() override
    {
        HICON icon = CreateGlyphIcon(L"arrow_sync_20_regular", GetSystemMetricsForDpi(SM_CXSMICON, m_Theme.Dpi()),
                                      m_Theme.Colors().Accent);

        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        StartRead(true);
    }

    void OnThemeChanged() override { Invalidate(); }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) override
    {
        switch (msg)
        {
            case WM_UPDATE_STATE:
            {
                UpdateState *state = (UpdateState *)lParam;

                m_State = *state;
                delete state;
                m_Busy = false;
                if (wParam)
                {
                    SYSTEMTIME st;

                    GetLocalTime(&st);
                    StringCchPrintfW(m_LastChecked, _countof(m_LastChecked), L"Last checked: today, %02u:%02u",
                                     st.wHour, st.wMinute);
                    m_Checked = true;
                }
                Invalidate();
                return 0;
            }
            case WM_UPDATE_ACTION:
            {
                ActionJob *job = (ActionJob *)lParam;
                DWORD error = (DWORD)wParam;

                m_NoticeError = error != ERROR_SUCCESS;
                if (error)
                {
                    WCHAR text[200];

                    StringCchPrintfW(text, _countof(text), L"%s (0x%08lX)", ErrorSentence(error), (unsigned long)error);
                    m_Notice = text;
                }
                else if ((job->What == Action::Apply || job->What == Action::InstallPackageExe) && job->Reboot)
                    m_Notice = L"Restart to finish this update.";
                else
                    m_Notice = L"Done.";
                delete job;
                StartRead(true);
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                int hot = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

                if (hot != m_Hot)
                {
                    m_Hot = hot;
                    Invalidate();
                }
                if (!m_Tracking)
                {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };

                    TrackMouseEvent(&tme);
                    m_Tracking = true;
                }
                break;
            }
            case WM_MOUSELEAVE:
                m_Tracking = false;
                m_Hot = -1;
                Invalidate();
                break;
            case WM_LBUTTONDOWN:
                m_Pressed = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                break;
            case WM_LBUTTONUP:
            {
                int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

                if (hit >= 0 && hit == m_Pressed)
                    Activate(m_Hits[hit]);
                m_Pressed = -1;
                break;
            }
            case WM_MOUSEWHEEL:
            {
                RECT client;

                GetClientRect(m_hwnd, &client);
                m_Scroll -= GET_WHEEL_DELTA_WPARAM(wParam) * m_Theme.Scale(60) / WHEEL_DELTA;
                ClampScroll(client);
                Invalidate();
                return 0;
            }
            case WM_CLOSE:
                if (m_Busy && MessageBoxW(m_hwnd, L"An update step is still running. Wait for it to finish.",
                                          L"LiberNT Update", MB_OK | MB_ICONINFORMATION))
                    return 0;
                break;
        }
        return FrameWindow::HandleMessage(msg, wParam, lParam);
    }

    void OnPaint(Canvas &canvas, const RECT &client) override
    {
        const Palette &c = m_Theme.Colors();
        int y, left, right, width;

        canvas.Fill(client, c.Window);
        m_Hits.clear();

        width = (std::min)((int)client.right - m_Theme.Scale(48), m_Theme.Scale(760));
        left = (client.right - width) / 2;
        right = left + width;
        y = CaptionHeight() + m_Theme.Scale(8) - m_Scroll;
        switch (m_Page)
        {
            case Page::Main: y = PaintMain(canvas, left, right, y); break;
            case Page::History: y = PaintHistory(canvas, left, right, y); break;
            case Page::Options: y = PaintOptions(canvas, left, right, y); break;
        }
        m_ContentHeight = y + m_Scroll + m_Theme.Scale(24);
    }

private:
    UpdateState m_State;
    Page m_Page;
    bool m_Busy, m_Collapsed;
    int m_Scroll, m_ContentHeight, m_Hot, m_Pressed, m_Expanded;
    std::vector<Hit> m_Hits;
    std::wstring m_Notice;
    bool m_NoticeError, m_Checked;
    bool m_Tracking = false;
    WCHAR m_LastChecked[48];

    static DWORD WINAPI ReadThread(LPVOID param)
    {
        HWND window = (HWND)param;
        UpdateState *state = new UpdateState;

        ReadState(*state, true);
        PostMessageW(window, WM_UPDATE_STATE, 1, (LPARAM)state);
        return 0;
    }

    static DWORD WINAPI ActionThread(LPVOID param)
    {
        ActionJob *job = (ActionJob *)param;
        bool reboot = false;
        DWORD error = RunAction(job->What, job->Id, job->Version, job->Path, &reboot);

        job->Reboot = reboot;
        PostMessageW(job->Window, WM_UPDATE_ACTION, error, (LPARAM)job);
        return 0;
    }

    void StartRead(bool)
    {
        HANDLE thread;

        m_Busy = true;
        thread = CreateThread(NULL, 0, ReadThread, m_hwnd, 0, NULL);
        if (thread)
            CloseHandle(thread);
        else
            m_Busy = false;
        Invalidate();
    }

    void StartAction(Action what, const std::string &id, const std::string &version, const std::wstring &path)
    {
        ActionJob *job;
        HANDLE thread;

        if (m_Busy)
            return;
        job = new ActionJob{ m_hwnd, what, id, version, path, false };
        m_Busy = true;
        m_Notice.clear();
        thread = CreateThread(NULL, 0, ActionThread, job, 0, NULL);
        if (thread)
            CloseHandle(thread);
        else
        {
            delete job;
            m_Busy = false;
        }
        Invalidate();
    }

    int HitAt(int x, int y) const
    {
        POINT pt = { x, y };

        /* the last registered rectangle is the one painted on top */
        for (size_t i = m_Hits.size(); i > 0; i--)
        {
            if (PtInRect(&m_Hits[i - 1].Rect, pt))
                return (int)(i - 1);
        }
        return -1;
    }

    void ClampScroll(const RECT &client)
    {
        int limit = (std::max)(0, m_ContentHeight - (int)client.bottom);

        m_Scroll = (std::max)(0, (std::min)(m_Scroll, limit));
    }

    void AddHit(const RECT &rc, HitKind kind, int arg = 0)
    {
        Hit hit = { rc, kind, arg };

        m_Hits.push_back(hit);
    }

    bool IsHot(HitKind kind, int arg = 0) const
    {
        return m_Hot >= 0 && m_Hits[m_Hot].Kind == kind && m_Hits[m_Hot].Arg == arg;
    }

    void Activate(const Hit &hit)
    {
        const PackageRow *banner = m_State.PackageForBanner >= 0 ? &m_State.Packages[m_State.PackageForBanner] : NULL;

        switch (hit.Kind)
        {
            case HitKind::Primary:
                switch (m_State.Header)
                {
                    case Head::Available:
                        for (size_t i = 0; i < m_State.Packages.size(); i++)
                        {
                            const PackageRow &p = m_State.Packages[i];

                            if (CanApply(p))
                            {
                                StartAction(Action::Apply, p.Id, p.Version, L"");
                                break;
                            }
                        }
                        break;
                    case Head::Restart:
                        if (MessageBoxW(m_hwnd, L"Restart now to finish the update?", L"LiberNT Update",
                                        MB_YESNO | MB_ICONQUESTION) == IDYES)
                        {
                            HANDLE token;
                            TOKEN_PRIVILEGES tp = { 1 };

                            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
                            {
                                LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
                                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                                AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
                                CloseHandle(token);
                            }
                            ExitWindowsEx(EWX_REBOOT, 0);
                        }
                        break;
                    case Head::Waiting:
                        if (banner)
                            StartAction(Action::Confirm, banner->Id, banner->Version, L"");
                        break;
                    case Head::Failed:
                        if (banner)
                            StartAction(Action::Rollback, banner->Id, banner->Version, L"");
                        break;
                    default:
                        StartRead(true);
                        break;
                }
                break;
            case HitKind::ConfirmBanner:
                if (banner)
                    StartAction(Action::Confirm, banner->Id, banner->Version, L"");
                break;
            case HitKind::Collapse:
                m_Collapsed = !m_Collapsed;
                Invalidate();
                break;
            case HitKind::ApplyRow:
            {
                const PackageRow &p = m_State.Packages[hit.Arg];

                StartAction(Action::Apply, p.Id, p.Version, L"");
                break;
            }
            case HitKind::RemoveRow:
            {
                const PackageRow &p = m_State.Packages[hit.Arg];

                StartAction(Action::Remove, p.Id, p.Version, L"");
                break;
            }
            case HitKind::RollbackRow:
            {
                const PackageRow &p = m_State.Packages[hit.Arg];

                if (MessageBoxW(m_hwnd, L"The files this update changed will be restored.", L"Roll back",
                                MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                    StartAction(Action::Rollback, p.Id, p.Version, L"");
                break;
            }
            case HitKind::UninstallRow:
            {
                const PackageRow &p = m_State.Packages[hit.Arg];

                if (MessageBoxW(m_hwnd, L"The files this update changed will be restored and the package removed.",
                                L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                    StartAction(Action::Remove, p.Id, p.Version, L"");
                break;
            }
            case HitKind::InstallPackage:
            {
                WCHAR path[MAX_PATH] = L"";
                OPENFILENAMEW ofn = { };

                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = L"LiberNT update packages (*.exe)\0*.exe\0All files\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = _countof(path);
                ofn.lpstrTitle = L"Install an update package";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                if (GetOpenFileNameW(&ofn))
                    StartAction(Action::InstallPackageExe, "", "", path);
                break;
            }
            case HitKind::AddPackage:
            case HitKind::ExportLog:
            {
                WCHAR path[MAX_PATH];
                BROWSEINFOW bi = { };
                PIDLIST_ABSOLUTE pidl;

                bi.hwndOwner = m_hwnd;
                bi.pszDisplayName = path;
                bi.lpszTitle = hit.Kind == HitKind::AddPackage ? L"Choose the folder of the package" : L"Choose where to save the log";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                pidl = SHBrowseForFolderW(&bi);
                if (pidl)
                {
                    if (SHGetPathFromIDListW(pidl, path))
                        StartAction(hit.Kind == HitKind::AddPackage ? Action::AddPackage : Action::ExportLog, "", "", path);
                    CoTaskMemFree(pidl);
                }
                break;
            }
            case HitKind::OpenHistory:
                m_Page = Page::History;
                m_Scroll = 0;
                Invalidate();
                break;
            case HitKind::OpenOptions:
                m_Page = Page::Options;
                m_Scroll = 0;
                Invalidate();
                break;
            case HitKind::Back:
                m_Page = Page::Main;
                m_Scroll = 0;
                Invalidate();
                break;
            case HitKind::HistoryRow:
                m_Expanded = m_Expanded == hit.Arg ? -1 : hit.Arg;
                Invalidate();
                break;
            default:
                break;
        }
    }

    static bool CanApply(const PackageRow &p)
    {
        return p.VerifyError == ERROR_SUCCESS &&
               (p.Phase.empty() || p.Phase == "staged" || p.Phase == "rolled-back" || p.Phase == "failed");
    }

    /* installed, or applied and waiting for confirmation: remove-package restores its files and drops it */
    static bool CanUninstall(const PackageRow &p)
    {
        return p.Phase == "confirmed" || p.Phase == "applied";
    }

    /* ---- drawing helpers ---- */

    RECT Card(Canvas &canvas, int left, int right, int top, int height, bool hot = false)
    {
        const Palette &c = m_Theme.Colors();
        RECT rc = { left, top, right, top + height };

        canvas.FillRound(rc, m_Theme.ScaleF(4), hot ? c.CardHover : c.Card);
        canvas.FrameRound(rc, m_Theme.ScaleF(4), c.CardBorder);
        return rc;
    }

    void Button(Canvas &canvas, const RECT &rc, LPCWSTR text, HitKind kind, int arg, bool accent, bool enabled = true)
    {
        const Palette &c = m_Theme.Colors();
        bool hot = enabled && IsHot(kind, arg);
        COLORREF fill = accent ? c.Accent : c.ControlFill;

        if (!enabled)
            fill = c.ControlFill;
        canvas.FillRound(rc, m_Theme.ScaleF(4), fill);
        if (hot)
            canvas.FrameRound(rc, m_Theme.ScaleF(4), accent ? c.AccentText : c.ControlStrongStroke);
        canvas.Text(text, rc, m_Theme.Font(TextStyle::Body), enabled ? (accent ? c.AccentText : c.Text) : c.TextDisabled,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (enabled && !m_Busy)
            AddHit(rc, kind, arg);
    }

    void Link(Canvas &canvas, const RECT &rc, LPCWSTR text, HitKind kind, int arg, DWORD align)
    {
        const Palette &c = m_Theme.Colors();

        canvas.Text(text, rc, m_Theme.Font(TextStyle::Body), IsHot(kind, arg) ? c.TextSecondary : c.Accent,
                    align | DT_VCENTER | DT_SINGLELINE);
        if (!m_Busy)
            AddHit(rc, kind, arg);
    }

    void HeaderIcon(Canvas &canvas, const RECT &rc)
    {
        const Palette &c = m_Theme.Colors();
        COLORREF color = c.Accent;
        LPCWSTR icon = L"arrow_sync_20_regular";

        switch (m_State.Header)
        {
            case Head::UpToDate: color = RGB(0x6C, 0xCB, 0x5F); icon = L"checkmark_circle_20_regular"; break;
            case Head::Available: break;
            case Head::Restart: color = RGB(0xFC, 0xE1, 0x00); icon = L"power_20_regular"; break;
            case Head::Waiting: color = RGB(0xFC, 0xE1, 0x00); icon = L"shield_20_regular"; break;
            case Head::RolledBack: color = RGB(0xFF, 0x99, 0xA4); icon = L"arrow_counterclockwise_20_regular"; break;
            case Head::Failed: color = RGB(0xFF, 0x99, 0xA4); icon = L"info_20_regular"; break;
            case Head::NotSetup: color = c.TextDisabled; icon = L"lock_closed_20_regular"; break;
        }
        canvas.Icon(icon, rc, color);
    }

    int PaintMain(Canvas &canvas, int left, int right, int y)
    {
        const Palette &c = m_Theme.Colors();
        LPCWSTR status, primary = NULL;
        RECT rc, iconRc, btn;
        int iconSize = m_Theme.Scale(72);
        int cardTop;

        switch (m_State.Header)
        {
            case Head::UpToDate: status = L"You're up to date"; primary = L"Check for updates"; break;
            case Head::Available: status = L"Updates available"; primary = L"Update all"; break;
            case Head::Restart: status = L"Restart required"; primary = L"Restart now"; break;
            case Head::Waiting: status = L"Waiting for confirmation"; primary = L"Confirm health"; break;
            case Head::RolledBack: status = L"The update was rolled back"; primary = L"Check for updates"; break;
            case Head::Failed: status = L"The update failed"; primary = L"Roll back"; break;
            default: status = L"Updates are not set up"; break;
        }
        if (m_Busy)
            status = L"Working...";

        rc = { left, y, right, y + m_Theme.Scale(44) };
        canvas.Text(L"LiberNT Update", rc, m_Theme.Font(TextStyle::Title), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += m_Theme.Scale(60);

        iconRc = { left + m_Theme.Scale(8), y, left + m_Theme.Scale(8) + iconSize, y + iconSize };
        HeaderIcon(canvas, iconRc);
        rc = { iconRc.right + m_Theme.Scale(20), y + m_Theme.Scale(6), right - m_Theme.Scale(220), y + m_Theme.Scale(40) };
        canvas.Text(status, rc, m_Theme.Font(TextStyle::Subtitle), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rc.top = rc.bottom;
        rc.bottom = rc.top + m_Theme.Scale(26);
        if (m_State.Header == Head::NotSetup && m_State.Error)
            canvas.Text(ErrorSentence(m_State.Error), rc, m_Theme.Font(TextStyle::Body), c.TextSecondary,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        else if (m_Checked)
            canvas.Text(m_LastChecked, rc, m_Theme.Font(TextStyle::Body), c.TextSecondary,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (primary)
        {
            btn = { right - m_Theme.Scale(200), y + m_Theme.Scale(14), right, y + m_Theme.Scale(52) };
            Button(canvas, btn, primary, HitKind::Primary, 0, true, !m_Busy);
        }
        y += iconSize + m_Theme.Scale(24);

        /* a waiting plan whose journal is locked: every boot would stop before touching a file */
        if (m_State.PlanPending && m_State.JournalLocked)
        {
            rc = Card(canvas, left, right, y, m_Theme.Scale(40));
            rc.left += m_Theme.Scale(16);
            canvas.Text(L"An update is waiting, but the update history can't be written. Fix it, then restart.", rc,
                        m_Theme.Font(TextStyle::Body), RGB(0xFF, 0x99, 0xA4),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += m_Theme.Scale(52);
        }

        /* the notice of the last action */
        if (!m_Notice.empty())
        {
            rc = Card(canvas, left, right, y, m_Theme.Scale(40));
            rc.left += m_Theme.Scale(16);
            canvas.Text(m_Notice.c_str(), rc, m_Theme.Font(TextStyle::Body), m_NoticeError ? RGB(0xFF, 0x99, 0xA4) : c.Text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += m_Theme.Scale(52);
        }

        /* the pending-transaction banner */
        if (m_State.PackageForBanner >= 0 && (m_State.Header == Head::Restart || m_State.Header == Head::Waiting ||
                                              m_State.Header == Head::RolledBack || m_State.Header == Head::Failed))
        {
            const PackageRow &p = m_State.Packages[m_State.PackageForBanner];
            WCHAR line[200];
            std::wstring id(p.Id.begin(), p.Id.end()), ver(p.Version.begin(), p.Version.end());
            WCHAR tx[9] = { };

            for (int i = 0; i < 8 && p.TxId[i]; i++)
                tx[i] = p.TxId[i];
            StringCchPrintfW(line, _countof(line), L"%s %s   (transaction %s)", id.c_str(), ver.c_str(), tx);
            rc = Card(canvas, left, right, y, m_Theme.Scale(56));
            {
                RECT text = { rc.left + m_Theme.Scale(16), rc.top, rc.right - m_Theme.Scale(180), rc.bottom };

                canvas.Text(line, text, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (m_State.Header == Head::Waiting)
            {
                RECT b = { rc.right - m_Theme.Scale(164), rc.top + m_Theme.Scale(10), rc.right - m_Theme.Scale(12),
                           rc.bottom - m_Theme.Scale(10) };

                Button(canvas, b, L"Confirm health", HitKind::ConfirmBanner, 0, false, !m_Busy);
            }
            y += m_Theme.Scale(68);
        }

        /* available packages */
        if (!m_State.Packages.empty())
        {
            int rows = m_Collapsed ? 0 : (int)m_State.Packages.size();
            int headerHeight = m_Theme.Scale(96);
            int rowHeight = m_Theme.Scale(52);
            RECT head;

            cardTop = y;
            head = Card(canvas, left, right, cardTop, headerHeight);
            {
                RECT t = { head.left + m_Theme.Scale(16), head.top + m_Theme.Scale(10), head.right - m_Theme.Scale(48),
                           head.top + m_Theme.Scale(34) };
                RECT d = { t.left, t.bottom, t.right, head.bottom - m_Theme.Scale(36) };
                RECT chevron = { head.right - m_Theme.Scale(40), head.top + m_Theme.Scale(30), head.right - m_Theme.Scale(20),
                                 head.top + m_Theme.Scale(50) };
                RECT add = { head.right - m_Theme.Scale(40) - m_Theme.Scale(130), head.bottom - m_Theme.Scale(32),
                             head.right - m_Theme.Scale(48), head.bottom - m_Theme.Scale(6) };
                RECT install = { add.left - m_Theme.Scale(200), add.top, add.left - m_Theme.Scale(16), add.bottom };

                canvas.Text(L"Packages", t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                canvas.Text(L"Staged, installed and verified. Updating a package changes system files; a restart may be needed.", d,
                            m_Theme.Font(TextStyle::Caption), c.TextSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
                canvas.Icon(m_Collapsed ? L"chevron_down_12_regular" : L"chevron_up_12_regular", chevron, c.Text);
                AddHit(head, HitKind::Collapse);
                Link(canvas, install, L"Install update package...", HitKind::InstallPackage, 0, DT_RIGHT);
                Link(canvas, add, L"Add package...", HitKind::AddPackage, 0, DT_RIGHT);
            }
            y = head.bottom + m_Theme.Scale(4);
            for (int i = 0; i < rows; i++)
            {
                const PackageRow &p = m_State.Packages[i];
                RECT row = Card(canvas, left + m_Theme.Scale(2), right - m_Theme.Scale(2), y, rowHeight);
                RECT t = { row.left + m_Theme.Scale(16), row.top, row.right - m_Theme.Scale(200), row.bottom };
                RECT a = { row.right - m_Theme.Scale(110), row.top, row.right - m_Theme.Scale(14), row.bottom };
                RECT r = { row.right - m_Theme.Scale(200), row.top, row.right - m_Theme.Scale(112), row.bottom };
                WCHAR line[200];
                std::wstring id(p.Id.begin(), p.Id.end()), ver(p.Version.begin(), p.Version.end()),
                             type(p.Type.begin(), p.Type.end());

                StringCchPrintfW(line, _countof(line), L"%s   %s   (%s, %u files)", id.c_str(), ver.c_str(), type.c_str(),
                                 (unsigned)p.Files);
                canvas.Text(line, t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (p.VerifyError != ERROR_SUCCESS)
                    canvas.Text(L"Cannot be installed", a, m_Theme.Font(TextStyle::Caption), RGB(0xFF, 0x99, 0xA4),
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                else if (CanApply(p))
                {
                    Link(canvas, a, L"Update", HitKind::ApplyRow, i, DT_RIGHT);
                    Link(canvas, r, L"Remove", HitKind::RemoveRow, i, DT_RIGHT);
                }
                else if (CanUninstall(p))
                {
                    canvas.Text(p.Phase == "confirmed" ? L"Installed" : L"Waiting", a, m_Theme.Font(TextStyle::Caption),
                                c.TextSecondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    Link(canvas, r, L"Uninstall", HitKind::UninstallRow, i, DT_RIGHT);
                }
                else
                    canvas.Text(L"In progress", a, m_Theme.Font(TextStyle::Caption), c.TextSecondary,
                                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                y += rowHeight + m_Theme.Scale(4);
            }
            y += m_Theme.Scale(16);
        }
        else
        {
            RECT add = { right - m_Theme.Scale(140), y, right, y + m_Theme.Scale(28) };
            RECT install = { add.left - m_Theme.Scale(200), y, add.left - m_Theme.Scale(16), y + m_Theme.Scale(28) };

            Link(canvas, install, L"Install update package...", HitKind::InstallPackage, 0, DT_RIGHT);
            Link(canvas, add, L"Add package...", HitKind::AddPackage, 0, DT_RIGHT);
            y += m_Theme.Scale(44);
        }

        /* more options */
        rc = { left, y, right, y + m_Theme.Scale(32) };
        canvas.Text(L"More options", rc, m_Theme.Font(TextStyle::BodyStrong), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += m_Theme.Scale(40);
        y = NavCard(canvas, left, right, y, L"history_20_regular", L"Update history", NULL, HitKind::OpenHistory);
        y = NavCard(canvas, left, right, y, L"shield_checkmark_20_regular", L"Advanced options",
                    L"Roll back an update, trusted source, export the log", HitKind::OpenOptions);
        return y;
    }

    int NavCard(Canvas &canvas, int left, int right, int y, LPCWSTR icon, LPCWSTR title, LPCWSTR text, HitKind kind)
    {
        const Palette &c = m_Theme.Colors();
        int height = text ? m_Theme.Scale(68) : m_Theme.Scale(56);
        RECT rc = Card(canvas, left, right, y, height, IsHot(kind));
        RECT ic = { rc.left + m_Theme.Scale(16), rc.top + (height - m_Theme.Scale(22)) / 2,
                    rc.left + m_Theme.Scale(38), rc.top + (height - m_Theme.Scale(22)) / 2 + m_Theme.Scale(22) };
        RECT t = { rc.left + m_Theme.Scale(56), rc.top + (text ? m_Theme.Scale(10) : 0), rc.right - m_Theme.Scale(48),
                   rc.top + (text ? m_Theme.Scale(34) : height) };
        RECT d = { t.left, t.bottom, t.right, rc.bottom - m_Theme.Scale(8) };
        RECT chevron = { rc.right - m_Theme.Scale(36), rc.top + (height - m_Theme.Scale(16)) / 2, rc.right - m_Theme.Scale(20),
                         rc.top + (height - m_Theme.Scale(16)) / 2 + m_Theme.Scale(16) };

        canvas.Icon(icon, ic, c.Text);
        canvas.Text(title, t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (text)
            canvas.Text(text, d, m_Theme.Font(TextStyle::Caption), c.TextSecondary, DT_LEFT | DT_TOP | DT_SINGLELINE);
        canvas.Icon(L"chevron_right_16_regular", chevron, c.Text);
        AddHit(rc, kind);
        return y + height + m_Theme.Scale(6);
    }

    int PageHeader(Canvas &canvas, int left, int right, int y, LPCWSTR title)
    {
        const Palette &c = m_Theme.Colors();
        RECT back = { left, y, left + m_Theme.Scale(80), y + m_Theme.Scale(30) };
        RECT rc = { left, y + m_Theme.Scale(34), right, y + m_Theme.Scale(80) };

        Link(canvas, back, L"< Back", HitKind::Back, 0, DT_LEFT);
        canvas.Text(title, rc, m_Theme.Font(TextStyle::Title), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return y + m_Theme.Scale(96);
    }

    int PaintHistory(Canvas &canvas, int left, int right, int y)
    {
        const Palette &c = m_Theme.Colors();

        y = PageHeader(canvas, left, right, y, L"Update history");
        if (m_State.TornJournal)
        {
            RECT rc = { left, y, right, y + m_Theme.Scale(26) };

            canvas.Text(L"The last entry was incomplete and was ignored.", rc, m_Theme.Font(TextStyle::Caption),
                        c.TextSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y += m_Theme.Scale(30);
        }
        if (m_State.History.empty())
        {
            RECT rc = { left, y, right, y + m_Theme.Scale(30) };

            canvas.Text(L"No updates have been installed yet.", rc, m_Theme.Font(TextStyle::Body), c.TextSecondary,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return y + m_Theme.Scale(40);
        }
        for (size_t i = 0; i < m_State.History.size(); i++)
        {
            const HistoryRow &h = m_State.History[i];
            bool open = m_Expanded == (int)i;
            int height = m_Theme.Scale(52);
            RECT rc = Card(canvas, left, right, y, height, IsHot(HitKind::HistoryRow, (int)i));
            RECT t = { rc.left + m_Theme.Scale(16), rc.top, rc.right - m_Theme.Scale(260), rc.bottom };
            RECT o = { rc.right - m_Theme.Scale(256), rc.top, rc.right - m_Theme.Scale(130), rc.bottom };
            RECT w = { rc.right - m_Theme.Scale(126), rc.top, rc.right - m_Theme.Scale(14), rc.bottom };
            WCHAR line[200];
            std::wstring id(h.Id.begin(), h.Id.end()), ver(h.Version.begin(), h.Version.end()),
                         outcome(h.Outcome.begin(), h.Outcome.end()), when(h.Time.begin(), h.Time.end());

            StringCchPrintfW(line, _countof(line), L"%s   %s", id.c_str(), ver.c_str());
            canvas.Text(line, t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            canvas.Text(outcome.c_str(), o, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            canvas.Text(when.c_str(), w, m_Theme.Font(TextStyle::Caption), c.TextSecondary,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            AddHit(rc, HitKind::HistoryRow, (int)i);
            y += height + m_Theme.Scale(4);
            if (open)
            {
                for (size_t l = 0; l < h.Lines.size(); l++)
                {
                    const HistoryLine &ln = h.Lines[l];
                    RECT lr = { left + m_Theme.Scale(24), y, right, y + m_Theme.Scale(24) };
                    std::wstring ph(ln.Phase.begin(), ln.Phase.end()), rs(ln.Result.begin(), ln.Result.end()),
                                 er(ln.Error.begin(), ln.Error.end());
                    WCHAR text[260];

                    StringCchPrintfW(text, _countof(text), L"%s  %s  %s", ph.c_str(), rs.c_str(), er.c_str());
                    canvas.Text(text, lr, m_Theme.Font(TextStyle::Caption), c.TextSecondary,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    y += m_Theme.Scale(24);
                }
                y += m_Theme.Scale(8);
            }
        }
        return y;
    }

    int PaintOptions(Canvas &canvas, int left, int right, int y)
    {
        const Palette &c = m_Theme.Colors();
        RECT rc;
        WCHAR line[200];

        y = PageHeader(canvas, left, right, y, L"Advanced options");

        rc = { left, y, right, y + m_Theme.Scale(32) };
        canvas.Text(L"Roll back an update", rc, m_Theme.Font(TextStyle::BodyStrong), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += m_Theme.Scale(40);
        {
            bool any = false;

            for (size_t i = 0; i < m_State.Packages.size(); i++)
            {
                const PackageRow &p = m_State.Packages[i];
                std::wstring id(p.Id.begin(), p.Id.end()), ver(p.Version.begin(), p.Version.end());
                RECT row, t, b;

                if (p.Phase != "confirmed" && p.Phase != "applied" && p.Phase != "pending-first-boot")
                    continue;
                any = true;
                row = Card(canvas, left, right, y, m_Theme.Scale(52));
                t = { row.left + m_Theme.Scale(16), row.top, row.right - m_Theme.Scale(130), row.bottom };
                b = { row.right - m_Theme.Scale(116), row.top + m_Theme.Scale(8), row.right - m_Theme.Scale(12), row.bottom - m_Theme.Scale(8) };
                StringCchPrintfW(line, _countof(line), L"%s   %s", id.c_str(), ver.c_str());
                canvas.Text(line, t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                Button(canvas, b, L"Roll back", HitKind::RollbackRow, (int)i, false, !m_Busy);
                y += m_Theme.Scale(58);
            }
            if (!any)
            {
                rc = { left, y, right, y + m_Theme.Scale(30) };
                canvas.Text(L"There is no update to roll back.", rc, m_Theme.Font(TextStyle::Body), c.TextSecondary,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                y += m_Theme.Scale(40);
            }
        }

        rc = { left, y, right, y + m_Theme.Scale(32) };
        canvas.Text(L"This device", rc, m_Theme.Font(TextStyle::BodyStrong), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += m_Theme.Scale(40);
        {
            std::wstring identity(m_State.Identity.begin(), m_State.Identity.end());
            std::wstring anchor(m_State.Anchor.begin(), m_State.Anchor.end());
            RECT card = Card(canvas, left, right, y, m_Theme.Scale(96));
            RECT a = { card.left + m_Theme.Scale(16), card.top + m_Theme.Scale(10), card.right - m_Theme.Scale(16),
                       card.top + m_Theme.Scale(42) };
            RECT b = { a.left, a.bottom + m_Theme.Scale(4), a.right, card.bottom - m_Theme.Scale(8) };

            StringCchPrintfW(line, _countof(line), L"Identity: %s", identity.c_str());
            canvas.Text(line, a, m_Theme.Font(TextStyle::Caption), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            StringCchPrintfW(line, _countof(line), L"Trusted source (certificate sha256): %s",
                             anchor.empty() ? L"none" : anchor.c_str());
            canvas.Text(line, b, m_Theme.Font(TextStyle::Caption), c.Text, DT_LEFT | DT_TOP | DT_WORDBREAK);
            y += m_Theme.Scale(108);
        }
        {
            RECT row = Card(canvas, left, right, y, m_Theme.Scale(52));
            RECT t = { row.left + m_Theme.Scale(16), row.top, row.right - m_Theme.Scale(130), row.bottom };
            RECT b = { row.right - m_Theme.Scale(116), row.top + m_Theme.Scale(8), row.right - m_Theme.Scale(12), row.bottom - m_Theme.Scale(8) };

            canvas.Text(L"Export the update log", t, m_Theme.Font(TextStyle::Body), c.Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Button(canvas, b, L"Export...", HitKind::ExportLog, 0, false, !m_Busy);
            y += m_Theme.Scale(58);
        }
        return y;
    }
};

int WINAPI
wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR commandLine, int showCmd)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    UpdateFrame window;
    MSG msg;

    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(commandLine);
    UNREFERENCED_PARAMETER(instance);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        HWND existing = FindWindowW(L"LiberNTUpdateFrame", NULL);

        if (existing)
        {
            if (IsIconic(existing))
                ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
            return 0;
        }
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControlsEx(&icc);
    if (!Startup())
        return 1;
    if (!window.Create(L"LiberNTUpdateFrame", L"LiberNT Update", NULL, 1000, 700, showCmd))
    {
        Shutdown();
        return 1;
    }
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    CoUninitialize();
    return (int)msg.wParam;
}
