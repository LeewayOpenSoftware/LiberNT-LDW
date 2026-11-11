# D0 DPI/mode probe

`dpiprobe` records a baseline; it does not change win32k/user32/GDI behavior and does not decide pass/fail from the current implementation. Each run emits `D0_PROBE_BEGIN` and `D0_PROBE_END` and writes the same output to stdout and the requested file.

## Usage

```text
dpiprobe --variant=unaware|system|permonitor|permonitorv2 \
         --phase=cold|live --log=PATH [--wait-ms=N] \
         [--test-width=W --test-height=H [--apply [--restore]]]
```

- Select awareness before creating the probe window. `system` means system DPI aware, `permonitor` PMv1, and `permonitorv2` PMv2.
- `--phase` labels the evidence; it does not itself change DPI. Use `cold` after sign-in/reboot at a fixed setting and `live` for a watch run.
- `--wait-ms=N` keeps the message loop alive and records a final snapshot. Use this while another process changes scale or mode.
- `--test-width/--test-height` select a CDS_TEST candidate. Without `--apply`, nothing is applied.
- `--apply` performs CDS_TEST first and, only if successful, CDS_UPDATEREGISTRY. It is rejected unless `--restore` is also present; `--restore` reapplies the original current mode. Use only in a disposable VM with a known alternate mode and a pre-apply snapshot for rollback.
- The log directory must already exist. Quote the command line if the log path contains spaces.

Every initial snapshot contains the three DPI registry values, awareness context, system/window/monitor DPI, SHCore monitor DPI, device caps, global and explicit-DPI metrics, stock/non-client fonts, current and registry DEVMODEs, and the indexed mode list. Final snapshots repeat those live values and rerun the indexed enumeration so an apply/restore cannot hide a changed mode list.

The window procedure logs `WM_DISPLAYCHANGE`, `WM_DPICHANGED` including the suggested rectangle, and `WM_SETTINGCHANGE`. Absence of a message is evidence, not a probe failure. Unsupported APIs are logged as `UNSUPPORTED`; invalid command lines, missing process-context support, or window creation failure return nonzero.

Lab matrix and Windows expectation table: `shared/libernt/display/D0-DPI-BASELINE.md` and `D0-WINDOWS-EXPECTATIONS.md`.
