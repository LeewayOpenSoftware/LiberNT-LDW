/*
 * Host test of the viewer ribbon model and adapter (not part of the build).
 * Builds with any MinGW on Windows; see host/run.sh. Stubs the LeewayUI
 * framework so vribbon.cpp's F1 layout conversion can be exercised too.
 *
 *   vribbon_host_test <path to lang/en-US.rc>
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <leewayui/ribbon.h>
#include "../resource.h"
#include "../vribbon.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

/* ---- framework stub (ribbon.h, D-R1 plain C) ---- */
struct ILeewayRibbon
{
    HWND hwnd;
    LeewayUILayout layout;
    bool haveLayout;
    struct Coll { UINT32 id, count, flags; UINT32 first, last; WCHAR firstLabel[16]; } coll[8];
    UINT nColl;
    int invalidateAll, themed;
    UINT lastInvalid, dpi;
    bool keyTipsChecked;
};

extern "C" {
HRESULT WINAPI LeewayRibbonCreate(HWND, const LeewayUICommand *t, UINT32 n, LeewayUICommandExecuteHandler,
                                  LeewayUICommandStateHandler, void *, ILeewayRibbon **out)
{
    if (!t || !n)
        return E_INVALIDARG;
    *out = new ILeewayRibbon();
    (*out)->hwnd = (HWND)1;
    return S_OK;
}
HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *r) { delete r; return S_OK; }
HWND WINAPI LeewayRibbonGetHWND(ILeewayRibbon *r) { return r->hwnd; }
UINT32 WINAPI LeewayRibbonGetHeight(ILeewayRibbon *) { return 60; }
HRESULT WINAPI LeewayRibbonSetLayout(ILeewayRibbon *r, const LeewayUILayout *l)
{
    r->layout = *l;
    r->haveLayout = true;
    return S_OK;
}
HRESULT WINAPI LeewayRibbonSetCollection(ILeewayRibbon *r, UINT32 id, const LeewayUICollectionItem *items, UINT32 count, UINT32 flags)
{
    if (r->nColl >= 8 || !count)
        return E_FAIL;
    ILeewayRibbon::Coll &c = r->coll[r->nColl++];
    c.id = id; c.count = count; c.flags = flags; c.first = items[0].id; c.last = items[count - 1].id;
    lstrcpynW(c.firstLabel, items[0].label, 16);
    return S_OK;
}
HRESULT WINAPI LeewayRibbonOnThemeChanged(ILeewayRibbon *r) { ++r->themed; return S_OK; }
HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *r, UINT32 d) { r->dpi = d; return S_OK; }
HRESULT WINAPI LeewayRibbonInvalidateCommand(ILeewayRibbon *r, UINT32 id) { r->lastInvalid = id; return S_OK; }
HRESULT WINAPI LeewayRibbonInvalidateAll(ILeewayRibbon *r) { ++r->invalidateAll; return S_OK; }
}

/* ---- fake host ---- */
struct FakeHost
{
    bool pred[VRB_P_COUNT];
    UINT executed;
    UINT zoom;
};
static BOOL FhPred(void *ctx, VRB_PRED p) { return ((FakeHost *)ctx)->pred[p]; }
static HRESULT FhExec(void *ctx, UINT id) { ((FakeHost *)ctx)->executed = id; return S_OK; }
static UINT FhZoom(void *ctx) { return ((FakeHost *)ctx)->zoom; }

static std::string ReadFile(const char *path)
{
    std::string s;
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static bool HasStringId(const std::string &rc, UINT id)
{
    char needle[32];
    sprintf(needle, "\n    %u \"", id);
    return rc.find(needle) != std::string::npos;
}

static void TestModel(const std::string &rc)
{
    WCHAR err[96];
    UINT n = VRB_ValidateModel(err, 96);
    if (n)
        wprintf(L"model problem: %ls\n", err);
    CHECK(n == 0);

    /* Every command has a label string; every group, tab has one. */
    for (UINT i = 0; i < g_VrbCommandCount; ++i)
    {
        bool ok = HasStringId(rc, VRB_LabelStringId(g_VrbCommands[i].id));
        if (!ok) printf("no label string for command %u\n", g_VrbCommands[i].id);
        CHECK(ok);
    }
    for (UINT t = 0; t < g_VrbLayout.tab_count; ++t)
    {
        CHECK(HasStringId(rc, g_VrbLayout.tabs[t].ids_label));
        for (UINT g = 0; g < g_VrbLayout.tabs[t].count; ++g)
            CHECK(HasStringId(rc, g_VrbLayout.tabs[t].groups[g].ids_label));
    }

    /* Existing ids keep their toolbar values (spec: id == WM_COMMAND id). */
    CHECK(VRB_FindCommand(IDC_PREV_PIC) && IDC_PREV_PIC == 500);
    CHECK(VRB_FindCommand(IDC_HELP_TOC) && IDC_HELP_TOC == 515);
    CHECK(VRB_FindCommand(IDC_TOGGLEFULLSCREEN) && IDC_TOGGLEFULLSCREEN == 600);
    CHECK(VRB_FindCommand(VRB_CMD_OPEN) && VRB_CMD_OPEN == 700);

    /* Spec 1.3: app menu order. */
    static const UINT menu[] = { 700, IDC_SAVEAS, IDC_PRINT, IDC_MODIFY, 701, IDC_HELP_TOC, 702 };
    CHECK(g_VrbLayout.appmenu_count == 7);
    for (UINT i = 0; i < 7 && i < g_VrbLayout.appmenu_count; ++i)
        CHECK(g_VrbLayout.appmenu[i].id == menu[i]);

    /* Spec 1.4: default QAT. */
    CHECK(g_VrbLayout.qat_count == 4);
    CHECK(g_VrbLayout.qat_default[0] == IDC_PREV_PIC && g_VrbLayout.qat_default[3] == IDC_ROT_CLOCKW);

    /* Spec 1.5/1.6: tabs and groups. */
    CHECK(g_VrbLayout.tab_count == 2);
    CHECK(g_VrbLayout.tabs[0].count == 5);
    CHECK(g_VrbLayout.tabs[1].count == 3);
    /* Alt, H, L rotates left; Alt, V, F is full screen; Alt, F, O is Open. */
    bool rotL = false, fs = false;
    for (UINT g = 0; g < g_VrbLayout.tabs[0].count; ++g)
        for (UINT c = 0; c < g_VrbLayout.tabs[0].groups[g].count; ++c)
        {
            const VRB_CONTROL &k = g_VrbLayout.tabs[0].groups[g].controls[c];
            if (k.id == IDC_ROT_COUNCW && !wcscmp(k.keytip, L"L")) rotL = true;
        }
    for (UINT g = 0; g < g_VrbLayout.tabs[1].count; ++g)
        for (UINT c = 0; c < g_VrbLayout.tabs[1].groups[g].count; ++c)
        {
            const VRB_CONTROL &k = g_VrbLayout.tabs[1].groups[g].controls[c];
            if (k.id == IDC_TOGGLEFULLSCREEN && !wcscmp(k.keytip, L"F")) fs = true;
        }
    CHECK(rotL);
    CHECK(fs);
    CHECK(!wcscmp(g_VrbLayout.app_keytip, L"F") && !wcscmp(g_VrbLayout.appmenu[0].keytip, L"O"));

    /* The zoom gallery is the ten levels plus Fit and Actual size. */
    const VRB_COLLECTION *z = nullptr;
    for (UINT c = 0; c < g_VrbLayout.tabs[0].groups[1].count; ++c)
        if (g_VrbLayout.tabs[0].groups[1].controls[c].id == VRB_CMD_ZOOMLEVEL)
            z = g_VrbLayout.tabs[0].groups[1].controls[c].collection;
    CHECK(z && z->count == 12 && z->ids[10] == IDC_BEST_FIT && z->ids[11] == IDC_REAL_SIZE);
    UINT pct = 0;
    CHECK(VRB_ZoomPercentOfCommand(711, &pct) && pct == 10);
    CHECK(VRB_ZoomPercentOfCommand(720, &pct) && pct == 1600);
    CHECK(!VRB_ZoomPercentOfCommand(721, &pct));
    CHECK(VRB_NearestZoomIndex(100) == 4);
    CHECK(VRB_NearestZoomIndex(37) == 1);
    CHECK(VRB_NearestZoomIndex(5) == 0);
    CHECK(VRB_NearestZoomIndex(4000) == 9);
}

static void TestState()
{
    FakeHost fh = {};
    VRB_HOST h = { &fh, FhPred, FhExec, FhZoom };
    BOOL en, ck;

    /* No image: everything image-bound is off, help and open stay on. */
    CHECK(VRB_EvalState(&h, IDC_ZOOM_IN, &en, &ck) && !en);
    CHECK(VRB_EvalState(&h, IDC_HELP_TOC, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, VRB_CMD_OPEN, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, VRB_CMD_EXIT, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, IDC_ROT_CLOCKW, &en, &ck) && !en);
    CHECK(VRB_EvalState(&h, IDC_INCTIMER, &en, &ck) && en);   /* always */
    CHECK(!VRB_EvalState(&h, 9999, &en, &ck) && !en);

    fh.pred[VRB_P_HAS_IMAGE] = true;
    fh.pred[VRB_P_IS_FIT] = true;
    fh.pred[VRB_P_CAN_ZOOM_IN] = true;
    CHECK(VRB_EvalState(&h, IDC_BEST_FIT, &en, &ck) && en && ck);   /* checked + enabled */
    CHECK(VRB_EvalState(&h, IDC_REAL_SIZE, &en, &ck) && en && !ck);
    CHECK(VRB_EvalState(&h, IDC_ZOOM_IN, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, IDC_ZOOM_OUT, &en, &ck) && !en);
    CHECK(VRB_EvalState(&h, IDC_ROT_CWSAVE, &en, &ck) && !en);       /* CanSave off */
    fh.pred[VRB_P_CAN_SAVE] = true;
    CHECK(VRB_EvalState(&h, IDC_ROT_CWSAVE, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, IDC_SAVEAS, &en, &ck) && en);
    CHECK(VRB_EvalState(&h, IDC_DELETE, &en, &ck) && !en);           /* verb/file gate */
    fh.pred[VRB_P_IS_FULLSCREEN] = true;
    CHECK(VRB_EvalState(&h, IDC_TOGGLEFULLSCREEN, &en, &ck) && en && ck);
    fh.pred[VRB_P_STATUSBAR_SHOWN] = true;
    CHECK(VRB_EvalState(&h, VRB_LWUI_STATUSBAR_TOGGLE, &en, &ck) && en && ck);
}

static void TestQat()
{
    const WCHAR *key = L"Software\\Leeway\\PhotoViewerHostTest\\QAT";
    UINT ids[VRB_QAT_MAX];
    UINT n;

    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    n = VRB_QatLoadFrom(key, ids, VRB_QAT_MAX);
    CHECK(n == 4 && ids[0] == IDC_PREV_PIC);                 /* missing = defaults */

    UINT save[] = { IDC_ZOOM_IN, IDC_DELETE, IDC_ZOOM_IN, 9999, VRB_LWUI_RIBBON_MINIMIZE, IDC_PRINT };
    CHECK(VRB_QatSaveTo(key, save, 6));
    n = VRB_QatLoadFrom(key, ids, VRB_QAT_MAX);
    CHECK(n == 3);                                            /* dup, unknown, framework dropped */
    CHECK(ids[0] == IDC_ZOOM_IN && ids[1] == IDC_DELETE && ids[2] == IDC_PRINT);

    UINT junk[] = { 9998, 9999 };
    CHECK(VRB_QatSaveTo(key, junk, 2));
    n = VRB_QatLoadFrom(key, ids, VRB_QAT_MAX);
    CHECK(n == 4);                                            /* nothing usable = defaults */

    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\PhotoViewerHostTest");
}

static void TestAdapter()
{
    FakeHost fh = {};
    fh.zoom = 37;
    VRB_HOST h = { &fh, FhPred, FhExec, FhZoom };
    VRB_RIBBON *r = nullptr;

    CHECK(SUCCEEDED(VRB_RibbonCreate((HWND)1, GetModuleHandleW(NULL), &h, &r)) && r);
    if (!r) return;

    LeewayUICommandState st = {};
    CHECK(SUCCEEDED(VRB_RibbonQueryState(r, VRB_CMD_ZOOMLEVEL, &st)));
    CHECK(!wcscmp(st.label, L"37%"));                         /* label = current zoom */
    CHECK(!st.enabled);                                       /* no image */
    fh.pred[VRB_P_HAS_IMAGE] = true;
    CHECK(SUCCEEDED(VRB_RibbonQueryState(r, VRB_CMD_ZOOMLEVEL, &st)) && st.enabled);
    CHECK(FAILED(VRB_RibbonQueryState(r, 4242, &st)));

    /* Execute: ids route to the host; the gallery opener itself does not run. */
    CHECK(VRB_RibbonExecute(r, IDC_ROT_CLOCKW) == S_OK && fh.executed == IDC_ROT_CLOCKW);
    fh.executed = 0;
    CHECK(VRB_RibbonExecute(r, VRB_CMD_ZOOMLEVEL) == S_FALSE && fh.executed == 0);
    CHECK(VRB_RibbonExecute(r, 713) == S_OK && fh.executed == 713);
    fh.pred[VRB_P_HAS_IMAGE] = false;
    fh.executed = 0;
    CHECK(VRB_RibbonExecute(r, 713) == S_FALSE && fh.executed == 0);   /* zoom cell, no image */
    CHECK(FAILED(VRB_RibbonExecute(r, 4242)));

    /* Layout as the framework would see it. */
    ILeewayRibbon *fw = *(ILeewayRibbon **)r;                  /* first member */
    CHECK(fw->haveLayout);
    if (fw->haveLayout)
    {
        const LeewayUILayout &L = fw->layout;
        CHECK(L.app_name && !wcscmp(L.app_name, L"PhotoViewer"));
        CHECK(L.tab_count == 2);
        /* Labels come from the string table, linked in from lang/en-US.rc. */
        CHECK(!wcscmp(L.tabs[0].label, L"Home") && !wcscmp(L.tabs[1].label, L"View"));
        CHECK(!wcscmp(L.tabs[0].groups[1].label, L"Zoom"));
        CHECK(!wcscmp(L.tabs[0].keytip, L"H") && !wcscmp(L.tabs[1].keytip, L"V"));
        CHECK(L.tabs[0].id != L.tabs[1].id);
        CHECK(L.tabs[0].groups[1].control_count == 5);
        CHECK(L.tabs[0].groups[1].controls[4].kind == LWUI_CONTROL_GALLERY &&
              L.tabs[0].groups[1].controls[4].command_id == VRB_CMD_ZOOMLEVEL);
        CHECK(L.tabs[0].groups[2].controls[0].kind == LWUI_CONTROL_SPLIT_LARGE);
        CHECK(L.tabs[0].groups[1].variant_order != 0);
        CHECK(L.appmenu_count == 7 && L.appmenu_ids[0] == VRB_CMD_OPEN);
        CHECK(L.qat_count >= 1 && L.qat_default_ids[0] == IDC_PREV_PIC);
        CHECK(L.help_command_id == 0);
    }
    /* Collections: the zoom gallery (710) with the cells, the slide show split menu. */
    {
        bool gal = false, split = false;
        for (UINT i = 0; i < fw->nColl; ++i)
        {
            if (fw->coll[i].id == VRB_CMD_ZOOMLEVEL)
            {
                gal = fw->coll[i].count == 12 && fw->coll[i].first == VRB_CMD_ZOOM_FIRST &&
                      !wcscmp(fw->coll[i].firstLabel, L"10%");
            }
            if (fw->coll[i].id == IDC_SLIDE_SHOW && fw->coll[i].count == 2)
                split = true;
        }
        CHECK(gal);
        CHECK(split);
    }

    /* Framework-only buttons not in the model route to the host too. */
    CHECK(SUCCEEDED(VRB_RibbonQueryState(r, 0xF004, &st)) && st.enabled);
    fh.executed = 0;
    CHECK(VRB_RibbonExecute(r, 0xF004) == S_OK && fh.executed == 0xF004);

    VRB_RibbonInvalidate(r, IDC_ZOOM_IN);
    CHECK(fw->lastInvalid == IDC_ZOOM_IN);
    VRB_RibbonInvalidateAll(r);
    CHECK(fw->invalidateAll == 1);
    CHECK(VRB_RibbonHeight(r) == 60);
    VRB_RibbonOnDpiChanged(r, 144);
    CHECK(fw->dpi == 144);
    VRB_RibbonOnThemeChanged(r);
    CHECK(fw->themed == 1);

    VRB_RibbonDestroy(r);
}

int main(int argc, char **argv)
{
    std::string rc = ReadFile(argc > 1 ? argv[1] : "../lang/en-US.rc");
    CHECK(!rc.empty());
    for (size_t p = 0; (p = rc.find("\r\n", p)) != std::string::npos; ) rc.erase(p, 1);

    TestModel(rc);
    TestState();
    TestQat();
    TestAdapter();

    if (g_fail)
    {
        printf("vribbon_host_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("vribbon_host_test: ok\n");
    return 0;
}
