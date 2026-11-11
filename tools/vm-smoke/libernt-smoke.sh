#!/bin/sh
# libernt-smoke.sh - automated visual smoke test for a LiberNT CI image.
#
#   sh tools/vm-smoke/libernt-smoke.sh <image> [name]
#
# <image> is one of:
#   - path to a raw LiberNT.img
#   - path to the CI artifact zip (LiberNT-img-*.zip)
#   - a GitHub Actions run id (the clang-amd64 LiberNT-img artifact is downloaded with gh; gh unpacks it,
#     so only the .img sha256 is recorded in the output folder)
#
# It boots the image in QEMU (UEFI/OVMF + WHPX, per-VM qcow2 overlay, the image is never written),
# waits for the desktop, then opens Explorer (This PC), cmd, Notepad, Control Panel and Settings
# one at a time. For each window it saves a full screenshot and a 6x zoom of the caption buttons
# (minimize / maximize / close) and prints PASS or FAIL. The exit code is the number of FAILs
# (0 = all windows passed; 99 = could not run at all).
#
# Output: $OUT_ROOT/<sha256 of the image>/ : NN-<window>.png, NN-<window>-caption6x.png,
#   00-desktop.png, result.txt, com1.log (kernel debug log). Re-running an image replaces the folder.
#
# Paths (all can be overridden by environment variables; the defaults are the LiberNT lab machine):
#      QEMU       directory with qemu-system-x86_64.exe and qemu-img   (default /c/msys64/ucrt64/bin)
#      MAGICK     ImageMagick binary                                   (default $QEMU/magick); run-vm.sh also reads QEMU_SHARE (edk2 dir)
#      PY         python3 (standard library only)                      (default /c/msys64/usr/bin/python3)
#      RUNVM      run-vm.sh: boots NAME RAM [qemu args], honours LABEL, IMG, PAYLOAD, ACCEL, PORT
#                                                                       (default: the copy next to this script)
#      MONPY      mon.py: sends one HMP command to the VM monitor      (default: the copy next to this script)
#      VMDIR      directory holding RUNNING.txt (the VM cap register)  (default /e/LiberNT/vm)
#      OVERLAY_DIR  per-VM directories made by run-vm.sh               (default /c/LiberNT/vm/overlays)
#      OUT_ROOT   output root                                          (default E:/LiberNT/results/smoke)
#      IMGDIR     where CI images are fetched (IMG_DIR also works)     (default C:/LiberNT/images)
#      REPO       owner/name for run ids and base lookup               (default LeewayOpenSoftware/LiberNT-LDW)
#      GH         gh binary                                            (default C:/LiberNT/tools/gh/bin/gh.exe)
#      BASE       force the base label: MAIN or "NOV1-BASE (no caption fix)"  (default: looked up with gh)
#      MAIN_FIX   commit that carries the caption fix                  (default b0b3ceb5d4)
# Other env:
#      RAM        guest RAM in MB          (default 3072)
#      ACCEL      whpx | tcg               (default whpx)
#      BOOT_TIMEOUT  seconds to wait for the desktop (default 240)
#      KEEP_VM=1  leave the VM running when done
#      VMDIR      directory holding run-vm.sh and mon.py (default: /e/LiberNT/vm)
#
# The guest is driven only through the QEMU monitor (sendkey / screendump), never the host input.
# The VM is registered in E:/LiberNT/vm/RUNNING.txt while it runs and always removed on exit.

set -u
# Run from a private copy of this script and its helpers: sh reads a script incrementally, so editing the file while
# a run is in progress used to corrupt that run. SMOKE_SNAP marks the copy.
if [ -z "${SMOKE_SNAP:-}" ]; then
  _src=$(cd "$(dirname "$0")" && pwd)
  _snap=$(mktemp -d "${TMPDIR:-/tmp}/libernt-smoke.XXXXXX") || { echo "SMOKE ERROR: cannot create a snapshot dir" >&2; exit 99; }
  _m=$(cygpath -m "$_snap" 2>/dev/null) && _snap="/$(echo "${_m%%:*}" | tr A-Z a-z)${_m#*:}"   # /c/Users/... form: the msys2 python resolves it, but neither /tmp nor C:/...
  cp "$_src"/libernt-smoke.sh "$_src"/glyph.py "$_src"/qmp-pointer.py "$_src"/qmp-key.py "$_snap"/ || { echo "SMOKE ERROR: cannot copy the script" >&2; exit 99; }
  SMOKE_SNAP=$_snap; SMOKE_SRC=$_src; export SMOKE_SNAP SMOKE_SRC
  sh "$_snap/libernt-smoke.sh" "$@"; _rc=$?; rm -rf "$_snap"; exit $_rc
fi
IMGARG=${1:?usage: libernt-smoke.sh <image.img|artifact.zip|run-id> [name]}
OUT_ROOT=${OUT_ROOT:-E:/LiberNT/results/smoke}
IMG_DIR=${IMGDIR:-${IMG_DIR:-C:/LiberNT/images}}
REPO=${REPO:-LeewayOpenSoftware/LiberNT-LDW}
GH=${GH:-C:/LiberNT/tools/gh/bin/gh.exe}
RAM=${RAM:-3072}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-240}
VMDIR=${VMDIR:-/e/LiberNT/vm}          # where RUNNING.txt lives (the VM-cap register)
RUNNING=${RUNNING:-$VMDIR/RUNNING.txt}
Q=${QEMU:-/c/msys64/ucrt64/bin}
PY=${PY:-/c/msys64/usr/bin/python3}
MAGICK=${MAGICK:-$Q/magick}
RUNVM=${RUNVM:-$SMOKE_SRC/run-vm.sh}      # vendored copies next to this script; override to use another
MONPY=${MONPY:-$SMOKE_SRC/mon.py}
OVERLAY_DIR=${OVERLAY_DIR:-/c/LiberNT/vm/overlays}; export OVERLAY_DIR   # run-vm.sh and mon.py read it too
MAIN_FIX=${MAIN_FIX:-b0b3ceb5d4}
export ACCEL=${ACCEL:-whpx}
FAILS=0; TB=48
MODE=${MODE:-launcher}      # launcher: launcher.cmd payload drives the apps (no typing; skips OOBE)
                            # keys: Win+R typing; also reports OOBE window and dead keyboard
FIRST_DELAY=${FIRST_DELAY:-150}; GAP=${GAP:-240}   # seconds before the first app / after an app with state checks (launcher mode)
SHORT_GAP=${SHORT_GAP:-120}                        # after apps that are only opened and closed

die() { echo "SMOKE ERROR: $*" >&2; exit 99; }
for t in "$Q/qemu-system-x86_64.exe" "$MAGICK" "$PY" "$RUNVM" "$MONPY"; do
  [ -e "$t" ] || die "missing $t"
done

# ---- 1. resolve the image ---------------------------------------------------------------------
case "$IMGARG" in
  *[!0-9]*) SRC=$IMGARG ;;
  *) # a run id
     RUN=$IMGARG; D=$IMG_DIR/$RUN
     if ! ls "$D"/*.img >/dev/null 2>&1; then
       mkdir -p "$D" || die "cannot create $D"
       NAME=$("$GH" api "repos/$REPO/actions/runs/$RUN/artifacts" --paginate \
              -q '.artifacts[]|select(.name|startswith("LiberNT-img-clang-amd64-"))|.name' | head -1)
       [ -n "$NAME" ] || die "run $RUN has no LiberNT-img-clang-amd64 artifact"
       "$GH" run download "$RUN" -R "$REPO" -n "$NAME" -D "$D" || die "download of $NAME failed"
     fi
     SRC=$D ;;
esac
if [ -d "$SRC" ]; then
  for z in "$SRC"/*.zip; do [ -e "$z" ] && (cd "$SRC" && unzip -oq "$z" && rm -f "$z"); done
  SRC=$(ls "$SRC"/*.img 2>/dev/null | head -1)
elif [ "${SRC%.zip}" != "$SRC" ]; then
  D=$(dirname "$SRC"); (cd "$D" && unzip -oq "$(basename "$SRC")") || die "unzip failed"
  SRC=$(ls "$D"/*.img | head -1)
fi
[ -f "$SRC" ] || die "no image found for '$IMGARG'"
IMG=$SRC; export IMG
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
OUT=$OUT_ROOT/$SHA
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
if [ -z "${ONLY:-}" ] && [ "${PERWIN:-1}" = 1 ]; then
  # One fresh VM boot per window: a window that fails to close (or opens late) can no longer spoil the
  # next window's result. Each window's folder is $OUT/<window>/; the summary is $OUT/result.txt.
  rm -rf "$OUT"; mkdir -p "$OUT" || die "cannot create $OUT"
  { echo "image   $IMG"; echo "sha256  $SHA"; echo "start   $(date -u +%Y-%m-%dT%H:%M:%SZ)"; } | tee "$OUT/result.txt"
  total=0
  for wn in explorer cmd notepad control settings terminal; do
    ONLY=$wn sh "$SELF" "$IMG" > "$OUT/run-$wn.log" 2>&1; rc=$?
    [ -f "$OUT/$wn/result.txt" ] && grep -Ev '^(image|sha256|vm|start|end|result|output) ' "$OUT/$wn/result.txt" | tee -a "$OUT/result.txt"
    [ $rc -eq 99 ] && { echo "$wn    FAIL  could not run (see run-$wn.log)" | tee -a "$OUT/result.txt"; rc=1; }
    total=$((total+rc))
  done
  echo "end     $(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$OUT/result.txt"
  if [ $total -eq 0 ]; then echo "result  PASS (6 windows, one boot each)" | tee -a "$OUT/result.txt"
  else echo "result  $total FAIL line(s) over 6 windows (one boot each)" | tee -a "$OUT/result.txt"; fi
  echo "output  $OUT"
  exit $total
fi
if [ -n "${ONLY:-}" ]; then OUT=$OUT_ROOT/$SHA/$ONLY; VM=${2:-smoke-$(echo "$SHA" | cut -c1-8)-$ONLY}; FIRST_DELAY=${FIRST_DELAY_ONLY:-130}
else VM=${2:-smoke-$(echo "$SHA" | cut -c1-8)}; fi
rm -rf "$OUT"; mkdir -p "$OUT" || die "cannot create $OUT"
want() { [ -z "${ONLY:-}" ] || [ "$ONLY" = "$1" ]; }
VDIR=$OVERLAY_DIR/$VM
RES=$OUT/result.txt
: > "$RES"
log() { echo "$*" | tee -a "$RES"; }
log "image   $IMG"
log "sha256  $SHA"
log "vm      $VM  ram=${RAM}MB accel=$ACCEL"
log "start   $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# ---- 2. VM lifecycle --------------------------------------------------------------------------
# the monitor takes one connection at a time and resets a rapid reconnect: retry a failed call
mon() { for _t in 1 2 3 4; do o=$("$PY" "$MONPY" "$VM" "$@" 2>/dev/null) && { echo "$o"; return 0; }; sleep 0.5; done; return 1; }
cleanup() {
  if [ "${KEEP_VM:-0}" != 1 ]; then
    mon quit >/dev/null 2>&1
    sleep 2
    [ -f "$VDIR/qemu.pid" ] && taskkill //F //PID "$(cat "$VDIR/qemu.pid" 2>/dev/null)" >/dev/null 2>&1
    [ -f "$RUNNING" ] && { grep -v "^$VM |" "$RUNNING" > "$RUNNING.$$" 2>/dev/null; cat "$RUNNING.$$" > "$RUNNING"; rm -f "$RUNNING.$$"; }
  fi
  [ -f "$VDIR/com1.log" ] && cp "$VDIR/com1.log" "$OUT/com1.log" 2>/dev/null
}
trap cleanup EXIT INT TERM

# the cap is 3 VMs at once: refuse to be the fourth
N=$(cut -d"|" -f1 "$RUNNING" 2>/dev/null | sed "s/ *$//" | sort -u | grep -c .); N=${N:-0}   # distinct VM names: a VM that re-registers itself is one VM
[ "$N" -lt 3 ] || die "VM cap reached ($N running, see $RUNNING)"
rm -rf "$VDIR"                       # always a fresh guest
if [ "$MODE" = launcher ]; then
  # A launcher.cmd at the root of a non-system disk skips OOBE and runs an automated session. It opens
  # one app per GAP seconds; the host notices each new window by diffing the screen, then closes it.
  mkdir -p "$VDIR/payload"
  # launcher.cmd itself exits at once (its console would otherwise stay on screen and pollute the
  # window diffs). One minimised helper per app sleeps until its slot and then starts the app.
  { printf '@echo off\r\n'
    t=$FIRST_DELAY
    for a in 'explorer shell:MyComputerFolder' 'cmd /k' 'notepad' 'control' 'ms-settings:' 'terminal'; do
      case "$a" in ms-settings:) lb=settings;; *) lb=${a%% *};; esac; want "$lb" || continue
      printf 'start "" /min cmd /c ping -n %s 127.0.0.1 ^>nul ^& start "" %s\r\n' "$t" "$a"; case "$a" in control|ms-settings:) t=$((t+SHORT_GAP));; *) t=$((t+GAP));; esac; done; } > "$VDIR/payload/launcher.cmd"
  export PAYLOAD=$(cygpath -m "$VDIR/payload")
fi
# ptr <move|click|dclick> X Y : absolute pointer action in screen pixels (never exactly 0,0)
ptr() { "$PY" "$QMPPY" "$QPORT" "$SW" "$SH" "$@"; }
# applaunch (the service that runs launcher.cmd at logon) only acts when SMBIOS carries this OEM string
QPORT=$((5900 + $(printf %s "$VM" | cksum | cut -d' ' -f1) % 100))
QMPPY=$SMOKE_SNAP/qmp-pointer.py
qk() { "$PY" "$SMOKE_SNAP/qmp-key.py" "$QPORT" "$@"; }   # ordered key chord (modifier down first), see qmp-key.py
# FLOOR RULE: the QEMU window title carries the image's BASE so a Nov-1 based image is never mistaken for a regression.
#   MAIN <sha>                          image built from leeway/main or a branch off b0b3ceb5d4
#   NOV1-BASE (no caption fix) <sha>    image built from a branch off the Nov-1 tag
# BASE="MAIN" or BASE="NOV1-BASE (no caption fix)" forces it. Otherwise the CI run id is taken from the image path
# (.../images/<run id>/LiberNT.img), its head commit is looked up with gh and compared with b0b3ceb5d4 (the
# caption fix): ancestor = MAIN, not = NOV1-BASE. If that cannot be found the label says BASE-UNKNOWN.
REPO_SLUG=$REPO
if [ -z "${BASE:-}" ]; then
  rid=$(echo "$IMGARG $IMG" | grep -Eo '/[0-9]{9,}/' | head -1 | tr -d /); hsha=
  [ -n "$rid" ] && hsha=$("$GH" run view "$rid" -R "$REPO_SLUG" --json headSha -q .headSha 2>/dev/null)
  if [ -n "$hsha" ]; then
    st=$("$GH" api "repos/$REPO_SLUG/compare/$MAIN_FIX...$hsha" -q .status 2>/dev/null)
    case "$st" in ahead|identical) BASE=MAIN;; diverged|behind) BASE="NOV1-BASE (no caption fix)";; *) BASE=BASE-UNKNOWN;; esac
  else BASE=BASE-UNKNOWN; fi
fi
SHA8=${SHA%${SHA#????????}}; LABEL="$BASE ${hsha:-$SHA8} $VM"
export LABEL
EXTRA="-qmp tcp:127.0.0.1:$QPORT,server,nowait"   # pointer is driven through QMP (HMP mouse_move pins the pointer)
[ "$MODE" = launcher ] && EXTRA="$EXTRA -smbios type=11,value=libernt.applaunch"
log "label   $LABEL"
eval "sh \"$RUNVM\" \"$VM\" \"$RAM\" $EXTRA" > "$OUT/qemu.out" 2>&1 &
echo "$VM | $LABEL (libernt-smoke.sh, image sha $SHA8) | $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RUNNING"
i=0; until [ -f "$VDIR/monitor.port" ] && mon "info status" 2>/dev/null | grep -q running; do
  i=$((i+1)); [ $i -le 30 ] || die "VM did not start (see $OUT/qemu.out)"; sleep 1
done

# shot <file.png>  -> screendump converted to png; prints WxH via $SW/$SH
shot() {
  mon "screendump $1.ppm" >/dev/null 2>&1
  [ -s "$1.ppm" ] || return 1
  "$MAGICK" "$1.ppm" "$1" && rm -f "$1.ppm"
}
# nonblank <png>: 0 when the picture has real content (not a flat colour)
nonblank() { s=$("$MAGICK" "$1" -format '%[fx:standard_deviation>0.02?1:0]' info: 2>/dev/null); [ "$s" = 1 ]; }
# diffpx <a.png> <b.png> [geometry-crop]: count of pixels that differ (fuzz 8%)
diffpx() {
  if [ -n "${3:-}" ]; then
    "$MAGICK" compare -metric AE -fuzz 8% "$1" -crop "$3" +repage "$2" -crop "$3" +repage null: 2>&1 | grep -o '^[0-9.e+]*' | head -1 | sed 's/[.].*//'
  else
    "$MAGICK" compare -metric AE -fuzz 8% "$1" "$2" null: 2>&1 | grep -o '^[0-9.e+]*' | head -1 | sed 's/[.].*//'
  fi
}

# ---- 3. wait for the desktop ------------------------------------------------------------------
# The desktop is up when the picture is non-flat and unchanged between two shots 4 s apart.
T0=$(date +%s); prev=; ok=0
while [ $(( $(date +%s) - T0 )) -lt "$BOOT_TIMEOUT" ]; do
  sleep 4
  shot "$OUT/.cur.png" || continue
  if nonblank "$OUT/.cur.png" && [ -n "$prev" ] && [ "$(diffpx "$prev" "$OUT/.cur.png")" -lt 200 ]; then ok=$((ok+1)); else ok=0; fi
  [ $ok -ge 3 ] && break
  cp "$OUT/.cur.png" "$OUT/.prev.png"; prev=$OUT/.prev.png
done
if [ $ok -lt 3 ]; then
  shot "$OUT/00-desktop.png"
  log "BOOT    FAIL  no stable desktop within ${BOOT_TIMEOUT}s"
  log "result  1 FAIL"; exit 1
fi
sleep 10                              # let the shell finish its start-up work
cp "$OUT/.cur.png" "$OUT/00-desktop.png"; shot "$OUT/00-desktop.png"
BASE=$OUT/00-desktop.png
GEO=$("$MAGICK" identify -format '%w %h' "$BASE"); SW=${GEO% *}; SH=${GEO#* }
log "BOOT    PASS  desktop up in $(( $(date +%s) - T0 ))s, screen ${SW}x${SH}"

# typeslow <text>: one key per monitor call with a pause, so a busy guest does not drop keys
# (lowercase letters, digits, space, '-', '.', ':' only)
typeslow() {
  t=$1
  while [ -n "$t" ]; do
    ch=${t%"${t#?}"}; t=${t#?}
    case "$ch" in
      " ") k=spc ;; "-") k=minus ;; ".") k=dot ;; ":") k=shift-semicolon ;; *) k=$ch ;;
    esac
    mon "sendkey $k" >/dev/null; sleep 0.25
  done
}
# ---- 3b. first-boot checks: OOBE window and keyboard liveness ----------------------------------
if [ "$MODE" = keys ]; then
# A clean boot must show a bare desktop. A "LiberNT Setup" (OOBE) window leaves a button on the
# taskbar, left of the tray; anything drawn there at boot is reported.
TBY=$((SH-TB+6)); TBH=$((TB-12))
tbs=$("$MAGICK" "$BASE" -crop 700x${TBH}+70+${TBY} +repage -format '%[fx:standard_deviation>0.02?1:0]' info: 2>/dev/null)
if [ "$tbs" = 1 ]; then log "OOBE    FAIL  a window is open at first boot (taskbar button present; LiberNT Setup/OOBE?), see 00-desktop.png"; FAILS=$((FAILS+1))
else log "OOBE    PASS  bare desktop, no window open at first boot"; fi
# Keyboard: open Run (Win+R) and type two bursts a few seconds apart; the field must change both times.
RD=360x220+0+$((SH-TB-220))
mon "sendkey esc" >/dev/null; sleep 1; shot "$OUT/.k0.png"
qk meta_l r >/dev/null; sleep 3; shot "$OUT/.kA.png"
typeslow "abcdef"; sleep 2; shot "$OUT/.kB.png"
sleep 5; typeslow "ghijkl"; sleep 2; shot "$OUT/.kC.png"
cp "$OUT/.kC.png" "$OUT/00-keyboard.png"
dA=$(diffpx "$OUT/.k0.png" "$OUT/.kA.png" $RD); dB=$(diffpx "$OUT/.kA.png" "$OUT/.kB.png" $RD); dC=$(diffpx "$OUT/.kB.png" "$OUT/.kC.png" $RD)
mon "sendkey esc" >/dev/null; sleep 1
if [ "${dA:-0}" -gt 500 ] && [ "${dB:-0}" -gt 30 ] && [ "${dC:-0}" -gt 30 ]; then KB=alive; log "KEYBOARD PASS  Win+R opened Run and both typing bursts reached it (px: $dA/$dB/$dC)"
else KB=dead; log "KEYBOARD FAIL  guest keyboard not working (Run opened px=$dA, burst1 px=$dB, burst2 px=$dC; need >500/>30/>30), see 00-keyboard.png"; FAILS=$((FAILS+1)); fi
rm -f "$OUT"/.k0.png "$OUT"/.kA.png "$OUT"/.kB.png "$OUT"/.kC.png
else
  KB=alive
  log "OOBE    SKIPPED  launcher.cmd payload bypasses first-boot setup (MODE=keys checks it)"
  log "KEYBOARD SKIPPED  no typing in launcher mode (MODE=keys checks it)"
fi

# ---- 3c. pointer probe -------------------------------------------------------------------------
# Move the pointer onto the "This PC" desktop icon and click it: the icon must get a selection highlight.
# The pointer is driven through QMP input-send-event: HMP mouse_move/mouse_button pin it at the bottom-right
# corner (every click then toggles Show Desktop), so a pointer that does not move via QMP is a real FAIL.
if want explorer; then PLOG=log; else PLOG=:; fi; POINTER_OK=0
THISPC_X=${THISPC_X:-44}; THISPC_Y=${THISPC_Y:-105}
IR=70x70+10+$((THISPC_Y-30))
# Up to three attempts: the guest tablet now and then ignores an action (seen as a click with no effect), so a miss is
# retried after a wake-up move before it is reported as a FAIL.
for attempt in 1 2 3; do
  shot "$OUT/.pa.png"; sleep 3; shot "$OUT/.pb.png"; noise=$(diffpx "$OUT/.pa.png" "$OUT/.pb.png" $IR)   # idle noise of the region
  ptr move $((THISPC_X + 120)) $((THISPC_Y + 160)); sleep 1
  ptr click $THISPC_X $THISPC_Y; sleep 2
  shot "$OUT/00-pointer-probe.png"
  pd=$(diffpx "$OUT/.pb.png" "$OUT/00-pointer-probe.png" $IR); rm -f "$OUT/.pa.png" "$OUT/.pb.png"
  [ "${pd:-0}" -gt $(( ${noise:-0} + 300 )) ] && break
  [ $attempt -lt 3 ] && { log "pointer-probe attempt $attempt  INFO  no selection (changed px ${pd:-0}, noise ${noise:-0}); retrying"; mon "sendkey esc" >/dev/null; sleep 1; }
done
if [ "${pd:-0}" -gt $(( ${noise:-0} + 300 )) ]; then POINTER_OK=1; $PLOG "POINTER PASS  QMP pointer click selected the This PC icon (changed px $pd, idle noise ${noise:-0})"
else $PLOG "POINTER FAIL  the QMP pointer click did not select the This PC icon (changed px ${pd:-0}, idle noise ${noise:-0}, need noise+300); caption-button hover/click tests skipped, window states are driven by keyboard instead, see 00-pointer-probe.png"; want explorer && FAILS=$((FAILS+1)); fi
mon "sendkey esc" >/dev/null; sleep 1

# ---- 4. the windows ---------------------------------------------------------------------------
# Every window is located by diffing the screen before and after it opens (the taskbar is ignored),
# so it does not matter whether Win+Up maximised it. The caption strip is the top-right 160x32 px
# of that window's rectangle, zoomed 6x. The window is then closed with a real click on its close
# button (Alt+F4, then "exit" for cmd, are fallbacks); the close result is reported next to PASS/FAIL.
CW=160; CH=32; TB=48                  # caption strip size, taskbar height to ignore
runcmd() {                            # clear stray dialogs, Win+R, type the command, Enter
  mon "sendkey esc" >/dev/null; sleep 1; mon "sendkey esc" >/dev/null; sleep 1
  qk meta_l r >/dev/null; sleep 3
  typeslow "$1"; sleep 1
  mon "sendkey ret" >/dev/null
}
# winbox <before.png> <after.png> -> "WxH+X+Y" of the biggest changed area above the taskbar (empty if none).
# Small unrelated changes (a window losing focus, the clock) form separate blobs and are ignored.
winbox() {
  "$MAGICK" "$1" "$2" -compose difference -composite -crop ${SW}x$((SH-TB))+0+0 +repage \
    -separate -evaluate-sequence max -threshold 10% -morphology Dilate Square:4 -define connected-components:verbose=true \
    -connected-components 8 null: 2>/dev/null | awk '/gray\(255\)/ && $4+0 > m { m=$4+0; b=$2 } END { if (m > 20000) print b }'
}
# click <x> <y>: absolute pointer click through the monitor (usb-tablet scale is 0..32767)
click() { ptr click "$1" "$2"; }
# ---- caption button checks (hover, minimise/restore, maximise/restore) -------------------------
# Buttons are laid out min | max | close, flush with the window's top-right corner. Their size depends on
# the window's caption style (measured on the Nov-1/main images; override with CAP_<LABEL>="close_w btn_w btn_h top_off"):
#   close_w = width of the close button, btn_w = width of min and max, btn_h = height, top_off = px below the window top.
capgeom() { TRI=
  case "$1" in
    explorer) g=${CAP_EXPLORER:-"43 26 18 3"} ;;
    cmd)      g=${CAP_CMD:-"43 26 18 1"} ;;
    notepad)  g=${CAP_NOTEPAD:-"43 26 18 1"} ;;
    terminal) g=${CAP_TERMINAL:-"26 26 22 6"}; TRI=23 ;;   # Terminal draws 24x22 buttons on a 26 px pitch, 23 px in from the box edge
    *)        g="43 26 18 3" ;;
  esac
  set -- $g; CLW=$1; BTW=$2; BTH=$3; BTY=$4
  RI=${CAP_RIGHT_INSET:-${TRI:-20}}   # the diff box includes the window shadow on the right
}
setbox() { box=$1; [ -n "$box" ] || return 1
  w=${box%%x*}; r=${box#*x}; h=${r%%+*}; r=${r#*+}; x=${r%%+*}; y=${r#*+}; }
curbox() { shot "$OUT/.m.png"; winbox "$pre" "$OUT/.m.png"; }
ismax() { [ "$w" -ge $((SW-30)) ] && [ "$h" -ge $((SH-TB-30)) ]; }
# geometry of button k (1=close 2=max 3=min): left edge, width, centre x; common centre y
btn_wd() { [ "$1" = 1 ] && echo $CLW || echo $BTW; }
btn_l()  { case $1 in 1) echo $((x+w-RI-CLW));; 2) echo $((x+w-RI-CLW-BTW));; 3) echo $((x+w-RI-CLW-2*BTW));; esac; }
btn_x()  { echo $(( $(btn_l $1) + $(btn_wd $1) / 2 )); }
btn_cy() { echo $(( y + BTY + BTH / 2 )); }
point() { ptr move "$1" "$2"; }
cap_tests() {
  # KEYBOARD-driven window-state checks. Mouse clicks on the caption buttons cannot be tested on these
  # images: the guest pointer is pinned at the bottom-right corner, so every absolute click toggles
  # "Show Desktop" (ledger: input / mouse, Kelly). The pointer probe at boot reports that as its own FAIL line.
  lb=$1; box0=$box
  # the pointer probe clicked the desktop: click an empty spot of the title bar so the window has the focus
  # (the guest tablet now and then ignores the first action after a pause, so move once to wake it first)
  [ "$POINTER_OK" = 1 ] && { ptr move $((x + w / 3)) $((y + BTY + BTH / 2 + 20)); sleep 1; ptr click $((x + w / 2)) $((y + BTY + BTH / 2)); sleep 1; }
  # start from the normal state
  if ismax; then qk meta_l down >/dev/null; sleep 3; setbox "$(curbox)" || setbox "$box0"; fi
  qk meta_l up >/dev/null; sleep 3; shot "$OUT/$n-$lb-maximized.png"
  if ! { setbox "$(winbox "$pre" "$OUT/$n-$lb-maximized.png")" && ismax; } && [ "$POINTER_OK" = 1 ]; then
    # one retry: refocus the window by clicking its title bar, then Win+Up again
    setbox "$box0"; ptr click $((x + w / 2)) $((y + BTY + BTH / 2)); sleep 1; qk meta_l up >/dev/null; sleep 3; shot "$OUT/$n-$lb-maximized.png"
  fi
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-maximized.png")" && ismax; then log "$lb.max      PASS  Win+Up maximised it (${w}x${h}), see $n-$lb-maximized.png"
  else log "$lb.max      FAIL  Win+Up did not maximise it (box ${box:-none}), see $n-$lb-maximized.png"; FAILS=$((FAILS+1)); fi
  qk meta_l down >/dev/null; sleep 3; shot "$OUT/$n-$lb-restored.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-restored.png")" && ! ismax; then log "$lb.restore  PASS  Win+Down restored it to ${w}x${h}, see $n-$lb-restored.png"; box0=$box
  else log "$lb.restore  FAIL  Win+Down did not restore the normal size (box ${box:-none}), see $n-$lb-restored.png"; FAILS=$((FAILS+1)); setbox "$box0"; fi
  qk meta_l down >/dev/null; sleep 3; shot "$OUT/$n-$lb-minimized.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-minimized.png")"; then
    log "$lb.min      FAIL  Win+Down again did not minimise it (still ${w}x${h}), see $n-$lb-minimized.png"; FAILS=$((FAILS+1)); setbox "$box0"
  else
    log "$lb.min      PASS  Win+Down again minimised it (window left the screen), see $n-$lb-minimized.png"
    qk alt tab >/dev/null; sleep 3
    ow=${box0%%x*}; if setbox "$(curbox)" && [ "$(( w > ow ? w - ow : ow - w ))" -lt 80 ]; then log "$lb.unmin    PASS  Alt+Tab brought it back (${w}x${h})"; box0=$box
    else log "$lb.unmin    FAIL  Alt+Tab did not bring the minimised window back (box ${box:-none})"; FAILS=$((FAILS+1)); setbox "$box0"; fi
  fi
}
# dclick <x> <y>: pointer to the spot, then two quick clicks
dclick() { ptr dclick "$1" "$2"; }
# guest_check: stop at once with a clear FAIL when the guest is no longer running
guest_check() { mon "info status" 2>/dev/null | grep -q running && return 0
  log "GUEST   FAIL  the VM is no longer running (powered off or crashed); remaining windows not tested"; log "result  FAIL"; exit 1; }
cap_click_tests() {
  # POINTER-driven checks (QMP absolute tablet): hover each caption button, then click maximise twice
  # (maximise, restore), minimise (+ restore from the taskbar button) and close. Run only when the pointer
  # probe passed. The window is in its normal state when this starts.
  lb=$1; setbox "$box0"; RI0=$RI; cy=$(btn_cy)
  # Hover is tried two ways (a third, jump-next-to-then-1px, gave the same result and was dropped to keep the number of tablet events low; the guest pointer stalled after ~8 QMP actions in one run), because one absolute jump may not produce the WM_NCMOUSEMOVE sequence that
  # hover tracking needs: (jump) one QMP move onto the button, (steps) ~10 small QMP moves 60 ms apart ending
  # on it, (near) jump next to the button, then a 1-pixel move onto it. A button passes if any way highlights it.
  res=; hv=PASS
  for spec in min:3 max:2 close:1; do
    b=${spec%:*}; k=${spec#*:}; bx=$(btn_l $k); bw=$(btn_wd $k); best=0; bm=none
    for meth in move steps; do
      ptr move $((SW/2)) $((SH-TB-6)); sleep 1; shot "$OUT/.idle.png"
      ptr $meth $(btn_x $k) $cy; sleep 1; shot "$OUT/$n-$lb-hover-$b-$meth.png"
      "$MAGICK" "$OUT/$n-$lb-hover-$b-$meth.png" -crop ${bw}x${BTH}+${bx}+$((y+BTY)) +repage -filter point -resize 600% "$OUT/$n-$lb-hover-$b-$meth-6x.png"
      d=$(lumdelta "$OUT/.idle.png" "$OUT/$n-$lb-hover-$b-$meth.png" $bw $bx $((y+BTY)))   # button face luminance change x1000
      res="$res $b/$meth=${d:-0}"
      [ "${d:-0}" -gt "$best" ] && { best=${d:-0}; bm=$meth; }
    done
    [ "$best" -gt 25 ] || hv=FAIL
  done
  if [ $hv = PASS ]; then log "$lb.hover    PASS  every button changes face when hovered by at least one approach (luminance delta x1000:$res; need >25), see $n-$lb-hover-*.png"
  else log "$lb.hover    FAIL  no approach (jump, small steps) changes the face of at least one button (luminance delta x1000:$res; need >25), see $n-$lb-hover-*.png"; FAILS=$((FAILS+1)); fi
  # maximise, then restore (the middle button twice)
  ptr click $(btn_x 2) $cy; sleep 3; shot "$OUT/$n-$lb-click-max.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-click-max.png")" && ismax; then
    RI=0; ptr click $(btn_x 2) $(btn_cy); sleep 3; shot "$OUT/$n-$lb-click-restore.png"; RI=$RI0
    if setbox "$(winbox "$pre" "$OUT/$n-$lb-click-restore.png")" && ! ismax; then log "$lb.click-max  PASS  clicking maximise then restore: ${w}x${h} -> maximised -> ${w}x${h}"; box0=$box
    else log "$lb.click-max  FAIL  maximised by click but the second click did not restore it (box ${box:-none}), see $n-$lb-click-restore.png"; FAILS=$((FAILS+1)); setbox "$box0"
      qk meta_l down >/dev/null; sleep 3; setbox "$(curbox)" && box0=$box; fi
  else log "$lb.click-max  FAIL  clicking the maximise button did not maximise the window (box ${box:-none}), see $n-$lb-click-max.png"; FAILS=$((FAILS+1)); setbox "$box0"; fi
  # minimise by click, restore from the taskbar
  cy=$(btn_cy); ptr click $(btn_x 3) $cy; sleep 3; shot "$OUT/$n-$lb-click-min.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-click-min.png")"; then
    log "$lb.click-min  FAIL  clicking minimise left the window on screen (${w}x${h}), see $n-$lb-click-min.png"; FAILS=$((FAILS+1)); setbox "$box0"
  else
    tb=$("$MAGICK" "$pre" "$OUT/$n-$lb-click-min.png" -compose difference -composite -crop $((SW*7/10))x$((TB-4))+0+$((SH-TB+2)) +repage \
          -separate -evaluate-sequence max -threshold 10% -trim -format '%@' info: 2>/dev/null | grep -Ev '^[01]x[01]+')
    setbox "$box0"
    if [ -n "$tb" ]; then
      # the diff detector measured a box that included the Start button (it clicked Start, not the app), so the
      # position is fixed instead: with one app open its taskbar button is the first one after Start, centre x=92.
      TBX=${TBX_APP:-92}
    else TBX=${TBX_APP:-92}; tb=fixed; fi
    if [ -n "$tb" ]; then
      ow=${box0%%x*}; how=taskbar
      ptr click $TBX $(( SH - TB / 2 )); sleep 3; shot "$OUT/$n-$lb-click-min-restored.png"
      ok=0; setbox "$(winbox "$pre" "$OUT/$n-$lb-click-min-restored.png")" && [ "$(( w > ow ? w - ow : ow - w ))" -lt 80 ] && ok=1
      if [ $ok = 0 ]; then mon "sendkey esc" >/dev/null; sleep 1; qk alt tab >/dev/null; sleep 3; how=alt-tab; setbox "$(curbox)" && [ "$(( w > ow ? w - ow : ow - w ))" -lt 80 ] && ok=1; fi
      if [ $ok = 1 ]; then log "$lb.click-min  PASS  minimise by click removed the window; it came back via $how (${w}x${h})"; box0=$box
        [ $how = taskbar ] || { log "$lb.taskbar-restore  FAIL  clicking the taskbar button did not bring the minimised window back (Alt+Tab did), see $n-$lb-click-min-restored.png"; FAILS=$((FAILS+1)); }
        [ "${TASKBAR_PROBE:-1}" = 1 ] && [ "$lb" = "${TASKBAR_PROBE_WIN:-explorer}" ] && taskbar_probe "$lb"
      else log "$lb.click-min  FAIL  minimised by click but neither its taskbar button nor Alt+Tab brought it back (box ${box:-none}), see $n-$lb-click-min-restored.png"; FAILS=$((FAILS+1)); setbox "$box0"; fi
    fi
  fi
  # close by click (CLOSE_BY=keys skips it so the window is closed with Alt+F4 below and that is what gets tested)
  [ "${CLOSE_BY:-click}" = keys ] && return 0
  cy=$(btn_cy); ptr click $(btn_x 1) $cy; sleep 3; shot "$OUT/.cur.png"
  if [ "$(diffpx "$pre" "$OUT/.cur.png" ${SW}x$((SH-TB))+0+0)" -lt 3000 ]; then log "$lb.click-close  PASS  clicking close removed the window"; CLICK_CLOSED=1
  else log "$lb.click-close  FAIL  clicking close left the window on screen"; FAILS=$((FAILS+1)); fi
}
# lumdelta <idle.png> <hover.png> <button width> <left x> <top y> : |mean luminance change| x1000 of the left 40% of
# the button face (the arrow starts at the centre of the button, so it is excluded). The idle shot is taken with
# the pointer parked away from the caption and the window active.
lumdelta() {
  cw=$(( $3 * 40 / 100 ))
  a=$("$MAGICK" "$1" -crop ${cw}x${BTH}+$4+$5 +repage -colorspace Gray -format '%[fx:mean*1000]' info: 2>/dev/null)
  b=$("$MAGICK" "$2" -crop ${cw}x${BTH}+$4+$5 +repage -colorspace Gray -format '%[fx:mean*1000]' info: 2>/dev/null)
  awk -v a="${a:-0}" -v b="${b:-0}" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%d", d }'
}
# taskbar_probe <label>: discriminators for "clicking the taskbar button does not restore a minimised window" (SM):
#  (c) click the button of the NON-minimised active window: does it minimise?
#  (b) does the button show hover / pressed under the QMP pointer? (luminance of the button rectangle, x1000)
#  (a) after the click: did a NEW window open (a window at a different position/size) instead of the old one?
#  (d) the same click with the PS/2 relative pointer (stepped path, E:/LiberNT/vm/click.py), on the minimised window.
# Informational lines only (INFO); the FAIL for the QMP restore is logged above.
taskbar_probe() {
  lb=$1; bx0=$TBX; by0=$((SH - TB / 2)); rx=$((bx0 - 22)); ry=$((SH - TB + 8)); rbox=44x30+$rx+$ry; ob=$box0
  # (c) active, normal window: one click on its taskbar button
  ptr click $((x + w / 3)) $((y + BTY + BTH / 2)); sleep 1   # make sure the window is the foreground window first (click its caption)
  ptr move $((SW / 2)) $((SH - TB - 40)); sleep 1; shot "$OUT/.tb-idle.png"
  ptr move $bx0 $by0; sleep 1; shot "$OUT/$n-$lb-tb-hover.png"
  ptr down; sleep 0.4; shot "$OUT/$n-$lb-tb-pressed.png"; ptr up; sleep 3; shot "$OUT/$n-$lb-tb-after-click.png"
  hv=$(lumdelta "$OUT/.tb-idle.png" "$OUT/$n-$lb-tb-hover.png" 44 $rx $ry); pr=$(lumdelta "$OUT/.tb-idle.png" "$OUT/$n-$lb-tb-pressed.png" 44 $rx $ry)
  log "$lb.taskbar-b  INFO  taskbar button luminance change vs idle: hover=$hv pressed=$pr (x1000; a button that reacts shows >25), see $n-$lb-tb-hover.png / $n-$lb-tb-pressed.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-tb-after-click.png")"; then
    nb=$box
    log "$lb.taskbar-c  INFO  clicking the taskbar button of the active, non-minimised window left it ON SCREEN (box $nb; real Windows minimises it)"
    log "$lb.taskbar-a  INFO  window visible after the click at box $nb vs box $ob before: $([ "$nb" = "$ob" ] && echo same window || echo 'DIFFERENT position or size - a new window may have opened'), see $n-$lb-tb-after-click.png"
  else
    log "$lb.taskbar-c  INFO  clicking the taskbar button of the active, non-minimised window MINIMISED it (as real Windows does), see $n-$lb-tb-after-click.png"
    qk alt tab >/dev/null; sleep 3; setbox "$(curbox)" || setbox "$ob"
  fi
  # (d) minimise again by the caption button, then click the taskbar button with the PS/2 relative pointer
  cy=$(btn_cy); setbox "$ob"; ptr click $(btn_x 3) $cy; sleep 3
  "$PY" "$VMDIR/click.py" "$VM" $bx0 $by0 >/dev/null 2>&1; sleep 3; shot "$OUT/$n-$lb-tb-ps2.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-tb-ps2.png")"; then log "$lb.taskbar-d  INFO  PS/2 relative pointer click on the taskbar button brought the minimised window BACK (${w}x${h}), see $n-$lb-tb-ps2.png"
  else log "$lb.taskbar-d  INFO  PS/2 relative pointer click on the taskbar button did NOT restore the minimised window, see $n-$lb-tb-ps2.png"; qk alt tab >/dev/null; sleep 3; setbox "$(curbox)" || setbox "$ob"; fi
  box0=$box
}
# window <n> <label> <run-command> <settle seconds>
window() {
  n=$1; lab=$2; cmd=$3; wait=$4; caps=${5:-}
  f=$OUT/$n-$lab.png; c=$OUT/$n-$lab-caption6x.png; pre=$OUT/.pre.png
  mon "sendkey esc" >/dev/null; sleep 1; shot "$pre"
  box=; try=0
  while [ -z "$box" ] && [ $try -lt 2 ]; do      # a dropped key gets one retry
    try=$((try+1))
    if [ "$MODE" = launcher ]; then          # wait for launcher.cmd to open the next app
      # rolling baseline: the shot just before the change is the "before" picture, so windows that
      # opened earlier (the launcher's own console) never count as the new window
      t=0; box=; cp "$pre" "$OUT/.roll.png"
      while [ -z "$box" ] && [ $t -lt $((GAP+90)) ]; do
        sleep 3; t=$((t+3)); shot "$OUT/.cur.png"; box=$(winbox "$OUT/.roll.png" "$OUT/.cur.png")
        [ -n "$box" ] || cp "$OUT/.cur.png" "$OUT/.roll.png"
      done
      [ -n "$box" ] && cp "$OUT/.roll.png" "$pre"
      sleep "$wait"
    elif [ "$KB" = dead ]; then
      if [ "$lab" = explorer ]; then mon "sendkey esc" >/dev/null; dclick ${THISPC_X:-44} ${THISPC_Y:-104}; sleep "$wait"   # keyboard dead: double-click the This PC icon
      else log "$lab    FAIL  not run: guest keyboard is dead (no way to launch it by mouse)"; FAILS=$((FAILS+1)); return; fi
    else runcmd "$cmd"; sleep "$wait"; fi
    shot "$f" || { guest_check; log "$lab    FAIL  screendump failed"; FAILS=$((FAILS+1)); return; }
    box=$(winbox "$pre" "$f")
    by=${box##*+}; [ -n "$box" ] && [ "$by" -gt $((SH/2)) ] && box=   # the Run dialog is still open: the typing was lost
  done
  if [ -z "$box" ]; then log "$lab    FAIL  no window appeared (screen did not change)"; FAILS=$((FAILS+1)); return; fi
  w=${box%%x*}; r=${box#*x}; h=${r%%+*}; r=${r#*+}; x=${r%%+*}; y=${r#*+}
  if [ "$w" -lt 200 ] || [ "$h" -lt 100 ]; then
    log "$lab    FAIL  changed area too small ($box)"; FAILS=$((FAILS+1)); return
  fi
  capgeom "$lab"
  # annotated picture: the three buttons the checks will use (red=min, green=max, blue=close)
  "$MAGICK" "$f" -fill none -stroke red -draw "rectangle $(btn_l 3),$((y+BTY)) $(( $(btn_l 3)+BTW )),$((y+BTY+BTH))" \
    -stroke lime -draw "rectangle $(btn_l 2),$((y+BTY)) $(( $(btn_l 2)+BTW )),$((y+BTY+BTH))" \
    -stroke blue -draw "rectangle $(btn_l 1),$((y+BTY)) $(( $(btn_l 1)+CLW )),$((y+BTY+BTH))" "$OUT/$n-$lab-buttons.png"
  cx=$(( x + w - RI - CW )); [ $cx -ge 0 ] || cx=0
  "$MAGICK" "$f" -crop ${CW}x${CH}+${cx}+${y} +repage -filter point -resize 600% "$c"
  cs=$("$MAGICK" "$c" -format '%[fx:standard_deviation>0.03?1:0]' info: 2>/dev/null)
  if [ "$cs" = 1 ]; then verdict=PASS; else verdict=FAIL; FAILS=$((FAILS+1)); fi
  # caption GLYPH check: min flat, max a square frame, close a symmetric X; the strip is thresholded to its bright
  # glyph pixels and judged by glyph.py (aspect ratio, left-right and top-bottom symmetry, fill). A broken glyph
  # (e.g. a close X squashed into an I shape) FAILs and its 6x crop is kept.
  "$MAGICK" "$f" -crop ${CW}x${CH}+${cx}+${y} +repage -colorspace Gray -threshold 78% -compress none "$OUT/.glyph.pgm" 2>/dev/null
  gl=$("$PY" "$SMOKE_SNAP/glyph.py" "$OUT/.glyph.pgm" 2>&1); grc=$?; rm -f "$OUT/.glyph.pgm"
  if [ $grc -eq 0 ]; then log "$lab.glyph    PASS  min, max and close glyphs have the right shape ($(echo "$gl" | sed 's/^glyph\.\([a-z]*\) *PASS *\([0-9x]*\) px.*w\/h=\([0-9.]*\).*/\1 \2/' | tr '\n' ' '))"
  else cp "$c" "$OUT/$n-$lab-glyph-FAIL-6x.png"; log "$lab.glyph    FAIL  broken caption glyph, see $n-$lab-glyph-FAIL-6x.png: $(echo "$gl" | grep FAIL | sed 's/  */ /g' | tr '\n' ';')"; FAILS=$((FAILS+1)); fi
  CLICK_CLOSED=0
  [ -n "$caps" ] && [ "$verdict" = PASS ] && cap_tests "$lab"
  [ -n "$caps" ] && [ "$verdict" = PASS ] && [ "$POINTER_OK" = 1 ] && cap_click_tests "$lab"
  # close with Alt+F4 (keyboard: the pointer is unusable). Esc afterwards cancels the shutdown dialog
  # that Alt+F4 opens if no window had the focus. Success = the screen is back to how it was before.
  closed=no
  [ "$CLICK_CLOSED" = 1 ] && closed=click
  for how in 1 2; do
    [ $closed = no ] || break
    qk alt f4 >/dev/null; sleep 3; mon "sendkey esc" >/dev/null; sleep 1; shot "$OUT/.cur.png"
    [ "$(diffpx "$pre" "$OUT/.cur.png" ${SW}x$((SH-TB))+0+0)" -lt 3000 ] && { closed=alt-f4; break; }
  done
  [ $closed = no ] && [ "$verdict" = PASS ] && { verdict=FAIL; FAILS=$((FAILS+1)); }
  log "$lab    $verdict  window ${w}x${h} at ${x},${y}; caption strip $([ "$cs" = 1 ] && echo drawn || echo blank); closed by: $closed"
}
want explorer && window 01 explorer 'explorer shell:mycomputerfolder' 8 caps
want cmd      && window 02 cmd       'cmd'                             5 caps
want notepad  && window 03 notepad   'notepad'                         5 caps
want control  && window 04 control   'control'                         8
want settings && window 05 settings  'ms-settings:'                    12
want terminal && window 06 terminal  'terminal'                         8 caps

# ---- 5. verdict -------------------------------------------------------------------------------
log "end     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
rm -f "$OUT"/.cur.png "$OUT"/.prev.png "$OUT"/.pre.png "$OUT"/.m.png "$OUT"/.idle.png "$OUT"/.roll.png
if [ $FAILS -eq 0 ]; then log "result  PASS"; else log "result  $FAILS FAIL"; fi
echo "output  $OUT"
exit $FAILS
