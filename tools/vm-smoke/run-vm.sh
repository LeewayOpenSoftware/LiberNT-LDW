# run-vm.sh NAME [RAM_MB] [extra qemu args...]
# Boots the shared LiberNT.img through a per-VM qcow2 overlay (the base image is never written).
# Output: C:/LiberNT/vm/overlays/NAME/{overlay.qcow2,com1.log,com2.log,qemu.pid,monitor port in monitor.port}
# Env: QEMUDIR, QEMU_SHARE, OVERLAY_DIR (host paths, defaults are the msys2 ucrt64 layout), LABEL (floor rule: "MAIN <sha>" or "NOV1-BASE (no caption fix) <sha>", shown in the QEMU window title), IMG (base image, required), ACCEL (whpx|tcg, default whpx), PORT (monitor tcp port, default 4450+hash)
set -e
NAME=${1:?name}; RAM=${2:-3072}; shift; if [ $# -gt 0 ]; then shift; fi
Q=${QEMUDIR:-${QEMU:-/c/msys64/ucrt64/bin}}         # dir with qemu-system-x86_64 and qemu-img
SHARE=${QEMU_SHARE:-/c/msys64/ucrt64/share/qemu}   # dir with the edk2 OVMF firmware files
VMROOT=${OVERLAY_DIR:-/c/LiberNT/vm/overlays}      # per-VM dirs (overlay, monitor.port, logs); same variable the smoke script uses
IMG=${IMG:?set IMG to the LiberNT.img to boot}
ACCEL=${ACCEL:-whpx}
[ -n "$LABEL" ] || echo "WARNING: LABEL not set; floor rule requires LABEL=\"MAIN <sha>\" or LABEL=\"NOV1-BASE (no caption fix) <sha>\"" >&2
# per-VM dirs live under $VMROOT (OVERLAY_DIR)
V=$VMROOT/$NAME
mkdir -p $V
PORT=${PORT:-$((4450 + $(printf %s "$NAME" | cksum | cut -d' ' -f1) % 100))}
echo $PORT > $V/monitor.port
[ -f $V/overlay.qcow2 ] || $Q/qemu-img.exe create -q -f qcow2 -F raw -b "$IMG" "$(cygpath -w $V/overlay.qcow2)"
W=$(cygpath -w $V)
# FW=uefi (default): edk2 OVMF code + a per-VM vars copy; FW=bios: SeaBIOS
FWARGS=
if [ "${FW:-uefi}" = uefi ]; then
  [ -f $V/vars.fd ] || cp $SHARE/edk2-i386-vars.fd $V/vars.fd
  FWARGS="-drive if=pflash,format=raw,readonly=on,file=$(cygpath -m $SHARE)/edk2-x86_64-code.fd -drive if=pflash,format=raw,file=$W\\vars.fd"
fi
# PAYLOAD=<host dir> adds it read-only as a FAT disk (vvfat, keep it < 500 MB); the guest sees it as a second drive
[ -n "$PAYLOAD" ] && FWARGS="$FWARGS -drive file=fat:$PAYLOAD,format=raw,if=ide,index=1,snapshot=on"
exec $Q/qemu-system-x86_64.exe -name "${LABEL:+$LABEL }$NAME" \
  -accel $ACCEL -M pc -cpu max -smp 2 -m $RAM \
  -drive file="$W\\overlay.qcow2",format=qcow2,if=ide,index=0 \
  -vga std -display sdl -usb -device usb-tablet \
  -serial file:"$W\\com1.log" -serial file:"$W\\com2.log" \
  -monitor tcp:127.0.0.1:$PORT,server,nowait \
  -pidfile "$W\\qemu.pid" $FWARGS \
  "$@"
