# Host harness for the boot applier (pending.c)

`host_applier.c` + `s1host_shim.h` compile the REAL `../../pending.c` for the host behind a small NT-API shim
(`S1_HOST_APPLIER`) over a fake volume root. `test-swap-host.py` builds a fake volume with a kernel/HAL/bootvid
trio, a dev-signed boot-class manifest and a swap plan, runs the applier in several scenarios and checks the
result by content hash (clean swap, dirty volume, second boot, torn file, missing file, mixed generation).

The shim counts every operation that can modify the volume (write/delete/create opens, writes, renames,
deletes, and the `logs\boot.log` append). The applier asks the dirty question through `ShimQueryVolumeDirty`;
if anything was mutated before that, the harness prints `FAIL` and `host_applier.exe` exits with code 3
(B1 review M4). `S1_FAKE_DIRTY=1` makes the fake volume dirty.

Build (mingw or clang, from this directory):

    gcc -municode -DUNICODE -D_UNICODE -I . -I ../.. -o host_applier.exe host_applier.c ../../s1core.c -ladvapi32 -lbcrypt

Run (needs Python with `cryptography` and the development signing key; the key is NOT in the repository,
`LN_DEV_SIGNING_DIR` points at the folder with `Leeway-LiberNT-Update-DEV.pfx` and `pfx-password.txt`):

    python3 test-swap-host.py host_applier.exe %TEMP%\swaptest

The applier verifies the manifest against the embedded development public key, so the manifest must be signed
with that key; this is why the test is run by a lab/owner host and not by the public CI. Making it a CI job
needs a decision about a test-only unsigned-manifest mode (it would put a host-only branch in pending.c).
