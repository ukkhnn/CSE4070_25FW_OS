#!/bin/sh
# Run inside the test image with /input mounted read-only and /results writable.
set -eu
project=${1:?expected prj2, prj3, or prj4}
case "$project" in
  prj2) suite=userprog ;;
  prj3) suite=threads ;;
  prj4) suite=vm ;;
  *) echo "Unknown project: $project" >&2; exit 2 ;;
esac
{ i686-linux-gnu-gcc --version; qemu-system-i386 --version; } > /results/toolchain.txt
cp -a "/input/$project" /work/project
cd /work/project/src
# Discard only copied build artifacts; the input checkout stays read-only.
rm -rf "$suite/build"
make -C utils clean >/results/utils.log 2>&1
python3 - <<'PY_MANIFEST' > /results/source.sha256
from pathlib import Path
import hashlib
root = Path("/work/project")
for path in sorted(root.rglob("*")):
    if path.is_file():
        print(hashlib.sha256(path.read_bytes()).hexdigest(), path.relative_to(root))
PY_MANIFEST
if ! make -C utils setitimer-helper >>/results/utils.log 2>&1; then
  tail -30 /results/utils.log; exit 1
fi
export PATH="/work/project/src/utils:$PATH"
# Cross-compile on either ARM64 or x86-64 Linux; user-space utilities stay native.
set -- CC='i686-linux-gnu-gcc -fno-pie -no-pie' LD=i686-linux-gnu-ld OBJCOPY=i686-linux-gnu-objcopy
if ! make -C "$suite" -j4 "$@" >/results/build.log 2>&1; then
  tail -30 /results/build.log; exit 1
fi
status=0
make -C "$suite/build" -j"${TEST_JOBS:-2}" "$@" check >/results/check.log 2>&1 || status=$?
cp "$suite/build/results" /results/results 2>/dev/null || true
find "$suite/build/tests" -type f \( -name '*.output' -o -name '*.errors' -o -name '*.result' \) -exec cp --parents '{}' /results/ \;
cat /results/results 2>/dev/null || tail -30 /results/check.log
exit "$status"
