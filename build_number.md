# LiberNT-LDW build identity and integration history

This file follows the owner's LBI policy of 2026-11-08 (`shared/libernt/lbi/OWNER-LBI-SPEC-INTEGRATION-CUTS-2026-11-08.md` in the hive) and his correction of the same day.

## Identity format

`integration.build.archtype.channel.stamp`, for example `1058.1.amd64fre.ldw_dev.261108-1200`.

| Field | Meaning |
|---|---|
| integration | The current official development baseline. |
| build | The compilation attempt within that integration. It starts at 1 for each new integration, is shared across all channels, is never reused, and failed attempts consume a number. It is allocated atomically by the ledger. |
| archtype | Architecture and configuration, for example `amd64fre`, `amd64chk`. |
| channel | `ldw_dev`, `ldw_ci`, `ldw_lab` or `ldw_rel`. |
| stamp | When the build was attempted (YYMMDD-HHMM). |

The exact git SHA of every build is recorded in its build manifest, not in the label.

## Policy

- **The integration number advances on an integration cut that the owner explicitly orders, once its publication is approved.** Code changes, merges, CI successes, lab passes and new builds do not advance it by themselves.
- **+1 per cut by default.** A larger jump requires the owner's explicit authorization, with its rationale recorded here. Skipped numbers are not missing builds.
- **Builds within the same integration increment the second number:** `1058.1`, `1058.2`, …
- **A cut goes through these states:** CUT REQUESTED, then CUT IN PROGRESS, then CUT VALIDATION, then PUBLISHED. The previous baseline then becomes SUPERSEDED.
  - A published cut's source snapshot is immutable (a commit or tag).
  - Builds compiled while preparing a cut use the TARGET integration's normal build sequence (for cut 1059: `1059.1`, `1059.2`, …), failed attempts included. They keep their identities even if the cut is delayed or rejected. Publication approval is a separate decision. There is no provisional numbering scheme (owner clarification, 2026-11-08).
- **No limit on builds per integration.** An integration may run to `1059.598` or beyond, through merges, features, fixes, upstream syncs and rebuilds. The build counter tracks compilation activity; the integration counter tracks owner-ordered consolidation milestones.
- **Separate concepts:** build success, validation success, a cut and its publication are different things. Publication is not a production-stability declaration.

## Cut boundaries

Every cut records four git boundaries:

1. the exact commit of the previous published cut;
2. the commits accumulated since that cut;
3. the commit boundary captured when the owner orders the next cut;
4. the final validated commit that is published as the new integration.

When the owner says "Cut integration N", god consolidates every relevant commit accumulated since boundary 1, resolves the integration issues, validates the result and publishes it only after the owner's approval.

| Cut | 1. Previous published cut | 2. Accumulated since | 3. Boundary at the order (2026-11-08) | 4. Validated and published |
|---|---|---|---|---|
| 1059 | 1058: `4fadd7fbf47` | `4fadd7fbf47..3146b7f39d5` on `leeway/main` (3 `[CMAKE]` fixes and 5 `[DOC]` commits), plus `test/integration-2026-11-08-zf` (merged re-authored as `824a9c4f7b6`) and the device write-cache flush port `d543e79c6b0` | `leeway/main` `177d092b22c` | **`82643adf6a4`** (`cut/1059-candidate`, build 1059.6), tag `integration-1059`, published 2026-11-10 on the owner's approval |

SHAs before the 2026-11-09 authorship rewrite, for traceability: `73fb270537b` → `177d092b22c`, `6e541e6d75f` → `be158d4fec5` (`-zf` tip), `2f1be760b5d` → `d543e79c6b0`, `1605a09126e` → `8d8d7be8c52` (same tree).

**Reconciliation note.** The 16 advances are the 16 landings on `leeway/main` listed below, read from its first-parent history and the hive integration records:
- integration branches 1029–1032;
- batches 1–7;
- the upstream syncs, including the sync-by-cherry-pick runs 1104–1106.

No authorized numbering jump is on record, so each advance counts +1. Build numbers within those historical integrations were not allocated at the time and are not invented here. The 32 sequences in the legacy ledger (`leeway-lbi-ledger`, allocated 2026-11-07 on LBI test trees) use the earlier sequence semantics, and their mapping is pending the owner's approval.

## Integration history (newest on top)

The rows numbered 1043–1058 are **historical integration cuts**. They are not compilation builds. The earlier version of this file labelled them "Build 1" to "Build 16"; that label is kept in the second column for traceability. No compilation-build numbers (the second LBI field) were recorded for these integrations, and none are invented. The "Build 19 (+3)" proposed earlier for today's work is withdrawn: today's work is the cut-1059 candidate, +1, and a larger jump needs the owner's explicit authorization.

Dates are the commit dates of the landing tip (UTC-3). "—" rows are direct commits between integrations; they belong to the integration then active.

| Integration | Former label in the earlier version of this file | Date | Landing (commit) | Summary |
|---|---|---|---|---|
| — | — | 2026-11-09 | `3de3d658206` … `049621fa4ce` (44 first-parent landings after the candidate merge) | After the 1059 order boundary, so outside the 1059 scope unless the owner widens it; rides with the next integration. Branch cleanup batches 1–3 (CI-green branches, 11:22–12:28); NET-S2/S4 notification and endpoint fixes `668434d8dff`; r149 round three via `leeway/integration-2026-11-09e` `91971777d06`; netstat/ipconfig fixes `ece5f9afeca`; durable `bootex.log` for autochk `b38b94934bf`; this file `049621fa4ce`. |
| **1059** | — | 2026-11-09 | merge `adab569c06b` of `cut/1059-candidate` `82643adf6a4` | **PUBLISHED 2026-11-10 (owner approval) — ACTIVE BASELINE.** Published snapshot `82643adf6a4`, tag `integration-1059`. Ordered by the owner on 2026-11-08: 1058 → 1059 (+1, no jump authorized), scope "consolidate all of them". Part of 1059: the `[DOC]` build_number.md commits of 2026-11-08 (`75b6798b958`, `c2222b5116f`, `5ebeeec0941`, `177d092b22c`, `3146b7f39d5`: the LBI integration-cut policy and this file), then the candidate chain on `177d092b22c`: `-zf` merge `824a9c4f7b6`, write-cache flush port `d543e79c6b0`, build fixes `8d8d7be8c52` and `70d9de08022`, write-barrier deadlock fix `0dc4db9af4a`, hiberfil ordering `82643adf6a4`. Builds, in order of attempt: `1059.1` CI 42532982863 `d1f1df03406` FAILED; `1059.2` CI 42533468443 `6e541e6d75f` FAILED; `1059.3` CI 42537294225 `1605a09126e`; `1059.4` CI 42538701061 `8d8d7be8c52` FAILED (winload link); `1059.5` CI 42544665247 `70d9de08022` SUCCESS; `1059.6` CI 42571856875 `82643adf6a4` SUCCESS (img 13654948190), validated at the labs (Lab03 net smoke PASS, Lab01, Lab04). Note: the candidate was merged into `leeway/main` before publication. Known issues at publication: the netio WSK functional gaps, the W3 wedge (not in scope), the KNOWN networking rows, and the 2-40 checker gap (pre-existing, no data loss). |
| — | — | 2026-11-08 09:08–09:10 | `8c9e8b2a981`, `58c7caf229b`, `a29687ab92e` | Intermediate step on 1058, carried into 1059: `[CMAKE]` LLVMpipe payload download: retry on HTTP 504 with backoff, hash verified inside the retry loop, backoff index fix. |
| **1058** | Build 16 | 2026-11-06 | upstream sync 1104–1106, tip `4fadd7fbf47` | **SUPERSEDED by 1059 (2026-11-10).** About 493 LiberNT `main-nt10` commits brought forward in one landing: clean-prefix merge `5214e29251f` (28), 210 cherry-picks of run 1105 with the NUMA adaptation `5a371715618` (`leeway/upstream-sync-1105`), 248 cherry-picks of run 1106 (`leeway/upstream-sync-1106`, tip `6133fa62a7a`), the ancestry-reconciliation merge `2498bdfe839` (465) and four keep-ours resolutions (loader-lock cluster revert, NUMA bodies, debugging startup entries, API-set schema mapping). |
| — | — | 2026-11-04 | `89c1f67e6f5`, `36a28891bae` | `[CI]` cancel superseded runs, clang amd64 only outside `leeway/main`; drop the i386 jobs. `leeway/main` stood at `36a28891bae` from 2026-11-04 to 2026-11-06 (the CI images the labs used). |
| **1057** | Build 15 | 2026-11-03 | upstream sync `25739f4d792` | 145 LiberNT `main-nt10` commits (mirror `0a41bd81`), one conflict in kernelbase.spec. |
| **1056** | Build 14 | 2026-11-03 | batch 7, tip `e25b5a9b91d` | 17 cherry-picks fast-forwarded from `65bfb3bdbab`: `fix-applier-anchor-write-nov1` (9 servicing commits), `telnetd-auth` (2), `feature/ntdll-ldr-gaps`, `fix/smss-pfro-backup-intent`, `fix/sos-show-drivers` (2), `leeway/fix-mouhid-absolute-flag2`, `leeway/fix-ndis-diag-flood`. |
| **1055** | Build 13 | 2026-11-02 | upstream sync `47f79e61c8c` + `49430a918dc`, tip `65bfb3bdbab` | 140 + 44 LiberNT `main-nt10` commits (2026-10-27 to 11-02); kbdclass/mouclass taken whole from upstream, build.yml kept x64 + i386. Landed with the direct commits `f7b25a4c7dd` (ci: x64 and i386 only), `4391e0a5f66`/`2eef070f5de` (PeekMessage spin), `58891f54139` (kbdclass headers), `65bfb3bdbab` (forwarder-loop check bounded). |
| **1054** | Build 12 | 2026-11-02 | batch 6, observability, tip `2f64b5ac2f1` | `d274cb15181` observability A+B+C (event log limits, performance counters, WMI hardware identity) and `2f64b5ac2f1` observability C (WMI LoadPercentage, working set, Win32 services); fast-forward `feb339bac5d` to `2f64b5ac2f1`. |
| **1053** | Build 11 | 2026-11-02 | batch 5, `530fd2a15cc` | `leeway/cursors-v2` (owner cursor set as default system cursors). Followed by the direct ATAPI fixes `a8971d5bfd1`, `c52e53c7b5f`, `feb339bac5d` (ScsiPortN collisions). |
| **1052** | Build 10 | 2026-11-02 | batch 4, tip `b6e4adfbd46` | `leeway/fix-harderror-hwmsg` `eaf1f8058da`, `leeway/fix-getcurrentpackageid` `652c88c280a`, servicing journal rewrite `05f231db480`, the USB port (`de110eae780` usbstor, `9d052a6353d` umpnpmgr, `e9c29079a17` shell32, `46613903372`/`55c5ccf38fb` usbhub, `6ddf9f0ec60` shell32 volumes, `13e0165fd38` pnp), `9e5ec11dcae` NDIS GEN counters, `dbf03036c78` netstat -e, `b6e4adfbd46` tcpip per-interface counters. Brace fix `588c99e76c3` followed ("batch 4 + i386 braces"). |
| **1051** | Build 9 | 2026-11-02 | batch 3, tip `be6015c6872` | `wt/p2-terminal-caption-paint` `6b0cbb8e7c7`, `leeway/fix-win32k-absolute-origin` `9cdd9e48e7a`, `leeway/fix-conhost-syskeys` `28701c0bffc`, `leeway/fix-authz-selfforward` `3226dce835f`, README update `be6015c6872`. |
| **1050** | Build 8 | 2026-11-01 | batch 2, tip `3d6fddcafcb` | `62e254ab599` kbdclass/mouclass read under one lock, `leeway/fix-loadlibrary-no-harderror` `76390469b0b`, IsDebuggerPresent export and forwarder loop check `cbe5aec1c37`, `leeway/fix-shellframe-close-glyph` `4c0b369769f`, `leeway/fix-msgbox-setforeground` `3d6fddcafcb`. |
| **1049** | Build 7 | 2026-11-01 | batch 1, tip `b0b3ceb5d4b` | `feature/caption-theme-v3` `eee73524280`, `feature/cpl-category-view` `9647b3a6a04`, `feature/cpl-combo` `f4b4917cf55`, `feature/libernt-update-s1-next` `69f37a57c73`, `feature/libernt-update-ui` `716ceea8352`, `feature/reactos-input-1031` `4ccdc7845de`, `feature/reactos-picks-1031` `608493d219f`; with `916ab113a3d` (SystemSettings drivers page), `59206037b44`/`37893bb132d` (LiberNT-Update build), `b0b3ceb5d4b` (ExplorerFrame caption buttons). |
| **1048** | Build 6 | 2026-11-01 | upstream sync `9df7f0e84a3` | 101 LiberNT `main-nt10` commits, plus `d4ef09fb9aa` (kernelbase standard handle constants). |
| **1047** | Build 5 | 2026-10-31 | integration-1032, tip `a28501bdce4` | `leeway/integration-1032` on 1031: `feature/aureline-icons-temp` `6e0de6d7665`, `feature/cpl-small-batch` `109753848b8`, `feature/cpl-owner-bugs` `df9c1a27c29`, `feature/lv-icon-scroll` `4825ac9ce99`, `feature/1031-test-fixes` `2a1b79cdc7d`, upstream `346eba3d85` `845a034ec74` (35); shell32 and windows.storage fixes `cbf18786a57`, `7e140e09322`, `5562022a749`; then the direct `df180dec687` (LiberNT-LDW repository identity), NDIS 6 bridge fixes and diagnostics `805061f4436`, `4c611dac278`, `989023fda4f`, `861ea54cf53`, ACPI `_PRT` on bridges `a28501bdce4`. |
| **1046** | Build 4 | 2026-10-30 | integration-1031, tip `7c01ed6072e` | `leeway/integration-1031` (Tramo D) on 1030, 15 merges: Tramo D Frente 2 DLL directories `cac7cedc451`, Frente 3 API sets `37a2003f519`, manifests `306146cbcc7`, PCI compatible-ID ladder `c176404df15`, driver store staging `05d7eb8151f`, Frente 4 G1-G2 `cfe41aea197`, navpane context menu `1f5b306ee68`, Frente 1 inversion lots `9a1b9c657d9`/`b88b3bc2280`, USB unique instance id `e7cd4979b9c`, DrvInst M6 signing `ff7d3ad2f4c`, UTF-8 ACP `f59f04f1462`, Tramo E `483daf4db0d`, Tramo E2 `f09872d0bf6`, setupapi stubs `7c01ed6072e`. |
| **1045** | Build 3 | 2026-10-30 | integration-1030, tip `2b538f6a38f` | `leeway/integration-1030` on 1029: upstream `df45cb8b8e`..`f7d9cc07f7` in three merges `40b79ef65d5` (5), `2cf81ed722f` (268), `03a561bd829` (66); `fix/ke-h-clang-pragma` `6edf730aac1`, `feature/admintools-shortcuts` `bc746da606e`, `feature/driver-ux-phase2` `d3b2952a50b`, HAL and SDK fixes `3e20f4cd4d2`/`92b84de0eb9`, `feature/compat-debt-a` `133ccbbb694`, tcpip v3 `4dd7592ed77`, M5b + M4 driver install `cf3bb09b103`, Tramo C `dbae2da89c3`, Tramo B `494fd76bd72`, refreshed main `2b538f6a38f`. |
| **1044** | Build 2 | 2026-10-29 | integration-1029, tip `7b0e8e3e4b0` | upstream `53c08dd284` `ca043473163` (151); `leeway/integration-1029` merges `feature/leeway-modern-item-states` `ee843849aba`, `feature/expand-lzx` `1250087e121`, `feature/expand-f-switch` `588ff536396`, `feature/explorer-view-zoom` `d09649a64fd`, `feature/audio-hda-inf` `c80df11bb8e`, `feature/audio-engine-e1` `50267509d5d`, `feature/drvinst-m3` `65c2f83bda8`, `feature/drvinst-m5` `00b0501ea2b`, `feature/driver-ux-phase1` `7dce0838db6`, `feature/driver-ux-phase2` `7e4d8a64e87`; `55b1524563c` user32_vista gestures, `7b0e8e3e4b0` newdev SEH library. |
| **1043** | Build 1 | 2026-10-28 | upstream sync `1a08d44cedd` | First integration: 208 LiberNT `main-nt10` commits merged into `leeway/main`. |
| **1042** (starting baseline) | base | 2026-10-27 | fork `26648bfa2c1` | `leeway/main` created from LiberNT `main-nt10` `4d1717400d8` with the fork README and FORK.md (`02f67fb22e7`, `26648bfa2c1`). Starting baseline 1042. |
