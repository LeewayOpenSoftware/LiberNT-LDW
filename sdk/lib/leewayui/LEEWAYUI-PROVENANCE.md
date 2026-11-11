# LeewayUI — provenance log

Audit trail for the clean-room LeewayUI ribbon (OWNER DECISION 2026-11-08, `RIBBON-DECISIONS.md`). **Every** LeewayUI commit — code, art, or doc — appends an entry here. Design rationale lives in `RIBBON-CONTROLS-DESIGN.md` §11.

## Rules (every entry affirms these)

1. **Original code only.** No third-party source without a per-file licence check recorded below. Nothing GPL-3 / LGPLv3+ / AGPL, and no MPL-pasted code, enters the LGPL-2.1+ tree.
2. **Sources consulted are listed**, per commit, as *inspiration only* — behaviour/idea, never copied expression (clean-room).
3. **Artwork origin recorded**: each icon/glyph names its Aureline master + author and confirms it was not traced or sampled from any Microsoft or other proprietary asset.
4. **IP items stay flagged** (open patent/trademark/licence clearance, `RIBBON-CONTROLS-DESIGN.md` §8) — no commit assumes distribution clearance.
5. **Clean-room restated**: no Microsoft code, markup binaries, control names, resource IDs, metrics tables, exact colours-as-assets, or bitmaps; no LibreOffice/VCL code or `.ui` files.

## Entry format

```
### <date> — <commit/artifact> (<author>)
- Type: code | art | doc
- Change: <what>
- Sources consulted (inspiration only): <public docs / concepts, or "none">
- Third-party code added: none | <name + licence + check result>
- Artwork: n/a | <Aureline master, author, "not traced/sampled from proprietary assets">
- Licence: LGPL-2.1+
- IP clearance: prototyping only — distribution clearance NOT assumed (see design §8)
```

---

## Entries

### 2026-11-08 — RIBBON-NOTEBOOKBAR-RESEARCH.md (Meredith)
- Type: doc (research)
- Change: Legal/architecture research — LibreOffice Notebookbar, MS Office UI licence program, ribbon patents, trademarks; comparison + recommendations.
- Sources consulted (inspiration only): LibreOffice source on github.com/LibreOffice/core + docs.libreoffice.org (architecture concepts only, no code); Microsoft Learn (Windows Ribbon Framework docs); Google Patents / Justia (patent metadata); secondary blogs for the retired Office UI licence. All cited in the doc.
- Third-party code added: none
- Artwork: n/a
- Licence: LGPL-2.1+ (documentation)
- IP clearance: research is NOT distribution clearance; open items tracked.

### 2026-11-08 — RIBBON-CONTROLS-DESIGN.md (Meredith)
- Type: doc (design)
- Change: v0 ribbon control set, keyboard/a11y, DPI, theming, layout template, prototype target; then updated to OWNER DECISION 2026-11-08 — settled decisions, Visual-identity (Aero-inspired Aureline) §9, Declarative-layout §10, Provenance §11, F0/F1/F2 acceptance checks §12.
- Sources consulted (inspiration only): APP-MODERNIZATION-PLAN.md, APP-INVENTORY.md (in-tree); public ribbon/Notebookbar behaviour concepts (idea-level, via the research doc). No Microsoft or LibreOffice code/markup/assets.
- Third-party code added: none
- Artwork: n/a (describes that Aureline art will be used; no art committed)
- Licence: LGPL-2.1+ (documentation)
- IP clearance: design/prototype only; distribution clearance NOT assumed (design §8).

### 2026-11-08 — RIBBON-DECISIONS.md (god, recorded here for the trail)
- Type: doc (decision record)
- Change: OWNER DECISION 2026-11-08 — continue independent clean-room native ribbon; Aureline identity; Win32/ATL/comctl32/uxtheme; milestones F0/F1/F2; one authorized VM behind a proof-of-work gate.
- Sources consulted: the research doc above.
- Third-party code added: none
- Artwork: n/a
- Licence: LGPL-2.1+
- IP clearance: prototyping authorized; distribution clearance pending counsel.

### 2026-11-08 — F0 ribbon skeleton (Leeway Open Software)
- Type: code (F0 milestone)
- Change: LeewayUI static library with command-table-driven ribbon control model. Commit bd4ef28a: 13 files (sdk/include/leewayui/{ribbon,theme,dpi}.h; sdk/lib/leewayui/{ribbon,theme,dpi}.cpp; modules/rostests/apitests/leewayui/{ribbon_test.cpp, ribbon_test.rc, ribbon_test.manifest}; two CMakeLists.txt subdirectory lines). Owner-drawn ribbon on F0 double-buffered base; per-monitor DPI v2 awareness; theme modes (light/dark/high-contrast/classic); test window with 3 sample commands; WM_DPICHANGED handling.
- Sources consulted (inspiration only): APP-INVENTORY.md, RIBBON-CONTROLS-DESIGN.md, Windows Ribbon Framework (public docs), in-tree examples (comctl32 v6, uxtheme, ATL patterns, mspaint/wordpad UI concepts). No Microsoft code, assets, or control templates copied.
- Third-party code added: none (clean-room, original implementation)
- Artwork: placeholder glyphs drawn in code (Aureline art deferred to F1/F2)
- Licence: LGPL-2.1-or-later
- IP clearance: proof-of-work prototype; distribution clearance NOT assumed (design §8).

---

*Seed entries are the F0 design/doc work to date. Code commits (first skeleton onward) append here.*

### 2026-11-10 — theme.h / theme.cpp / dpi.h: named colours, live mode detection, C linkage (Leeway Open Software)
- Type: code (F1)
- Change: the theme object now owns the palette: `LeewayThemeColor` enum with an Aureline light set, a dark set and a system-colour path for high contrast and classic; mode detected live (SPI_GETHIGHCONTRAST, IsThemeActive, AppsUseLightTheme); brushes by name kept over the same table; `LeewayThemeIsFlat`. theme.h and dpi.h get `extern "C"` guards. Rationale: the ribbon never hard-codes a colour (RIBBON-CONTROLS-DESIGN.md §5).
- Sources consulted (inspiration only): RIBBON-CONTROLS-DESIGN.md §3/§5; public Windows API documentation for the calls named above. No Microsoft colour tables or theme files.
- Third-party code added: none
- Artwork: n/a (palette values are our own)
- Licence: LGPL-2.1+
- IP clearance: prototyping only — distribution clearance NOT assumed (see design §8)

### 2026-11-10 — ribbon.h / ribbon.cpp: F1 D-R1 plain-C API and working control (Leeway Open Software)
- Type: code (F1)
- Change: plain-C redesign of the F1 header after the Meredith and Pam reviews (`shell/appframework/review/ribbon-h-review-*.md`): `LeewayUICommandState` from query-state with InvalidateCommand/InvalidateAll (G3); layout tree as arrays + counts with per-placement keytips, app-declared group variant order (G11), contextual tabs (G13), keytip collision check (G18), app menu rows, QAT defaults, help id, app name; collections by command id with the last-choice flag (G4); framework ids 0xF001-0xF005 (spec §0); theme from the F0 theme object; icon resolver returning HICONs for (id, size, variant, fg, bg) with the glyph-on-dark and high-contrast mono rules (G10); accelerator text in tooltips (G22); WM_NOTIFY height/QAT/minimise; QAT persistence `HKCU\Software\Leeway\<app>\QAT`; PreTranslateMessage for keytips and Ctrl+F1. ribbon.cpp implements tab strip, band, column packing and variants, all F1 control kinds, popups (menu, gallery, collapsed group, minimised band, app menu, QAT customise), tooltips, keytip badges, focus walk, double-buffered painting.
- Sources consulted (inspiration only): RIBBON-CONTROLS-DESIGN.md, RIBBON-APPS-SPEC.md, the two reviews, Pam's frame.h; public Win32 API documentation (TrackMouseEvent, CopyAcceleratorTable, GetKeyNameText, WM_NOTIFY, DrawIconEx, DrawState). Behaviour described from the design, not from any product's code; no Microsoft ribbon code, markup, metrics tables or resources.
- Third-party code added: none
- Artwork: placeholder glyph (rounded tile with the label's initial) drawn in code; Aureline masters arrive through the resolver (icons/README.md)
- Licence: LGPL-2.1+
- IP clearance: prototyping only — distribution clearance NOT assumed (see design §8)

### 2026-11-10 — ribbon_test.cpp: the Photo Viewer ribbon on F1; test CMake import libraries (Leeway Open Software)
- Type: code (test)
- Change: the test window builds RIBBON-APPS-SPEC.md §1 from static const tables (Home, View, reserved Animation contextual tab behind a demo toggle, zoom gallery 710 -> 711-720 + Fit/Actual size, rotate and slideshow split menus, QAT defaults with persistence, app menu rows, accelerator table for tooltips, demo icon resolver, query-state predicates). The View tab's Fit placement takes keytip B because F is Full screen there. Test CMake adds advapi32 and uxtheme import libraries.
- Sources consulted (inspiration only): RIBBON-APPS-SPEC.md §1; shimgvw resource ids (in-tree LiberNT source, LGPL/GPL-compatible, used as id values only).
- Third-party code added: none
- Artwork: demo icons are coloured discs drawn in code; not shipped
- Licence: LGPL-2.1+
- IP clearance: prototyping only — distribution clearance NOT assumed (see design §8)
