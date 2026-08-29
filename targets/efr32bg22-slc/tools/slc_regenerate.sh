#!/usr/bin/env bash
#
# Regenerate autogen/ for the EFR32BG22 target from opendisplay-bg22.slcp.
#
# WHY THIS EXISTS. autogen/ is in this repo's .gitignore (a decision that predates
# the import), but the build needs four files that live nowhere else:
# autogen/linkerfile.ld, autogen/gatt_db.c, autogen/sl_bluetooth.c and
# autogen/sl_event_handler.c. Without this script a fresh clone cannot build the
# target at all, and the alternative -- committing generated files -- reverses the
# .gitignore rule.
#
# WHAT IT DELIBERATELY DOES NOT DO: it does not write cmake_gcc/. `slc generate`
# emits only the sources the .slcp lists, so regenerating cmake_gcc/ in place would
# DELETE the hand-added display, panel, QR, colour and compression sources, and
# would rewrite the application paths as absolute machine paths. So generation goes
# to a temporary directory and only autogen/ is copied back. The generated
# cmake_gcc/opendisplay-bg22.cmake is left there for inspection -- diffing it against
# the checked-in one is how you find out whether the hand-maintained file has drifted.
#
# Requires: the pinned Simplicity SDK 2025.12.2 (see README.md § "What must be
# installed"), slc-cli, and the bundled Java 21 -- which is NOT on PATH by default.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$(cd "$HERE/.." && pwd)"

# Java 21 ships with slt but is not on PATH; without it slc reports "command not
# found", which reads like a missing runtime rather than a missing PATH entry.
# Globbed rather than pinned to one archive/platform string: slt's java21/slc-cli
# point versions drift (and the platform directory name itself differs, e.g.
# java21_linux_x86_64 vs macosx.x86_64_*), so a hardcoded path goes stale on the
# next slt update or a different OS without ever being wrong about the pin itself.
JAVA_DIR="${OD_SLC_JAVA_DIR:-}"
if [[ -z "$JAVA_DIR" ]]; then
  for d in "$HOME"/.silabs/slt/installs/archive/java21-v*/*/jre/bin; do
    [[ -x "$d/java" ]] && { JAVA_DIR="$d"; break; }
  done
fi
if [[ -n "$JAVA_DIR" && -x "$JAVA_DIR/java" ]]; then
  export PATH="$JAVA_DIR:$PATH"
fi

SLC="${SLC:-}"
if [[ -z "$SLC" ]]; then
  for f in "$HOME"/.silabs/slt/installs/archive/slc-cli-v*/slc_cli/slc; do
    [[ -x "$f" ]] && { SLC="$f"; break; }
  done
fi
if [[ -z "$SLC" || ! -x "$SLC" ]]; then
  SLC="$(command -v slc || true)"
fi
if [[ -z "$SLC" || ! -x "$SLC" ]]; then
  echo "error: slc-cli not found. Set SLC=/path/to/slc." >&2
  exit 1
fi

# Same discovery order the CMake uses, so the two cannot disagree about which SDK.
SDK="${SIMPLICITY_SDK_DIR:-}"
if [[ -z "$SDK" ]] && command -v slt >/dev/null 2>&1; then
  SDK="$(slt where simplicity-sdk 2>/dev/null | tr -d '\r\n' || true)"
fi
if [[ -z "$SDK" || ! -f "$SDK/bluetooth_le_host/src/sl_bt_stack_init.c" ]]; then
  echo "error: Simplicity SDK 2025.12.2 not found. This target does not vendor it." >&2
  echo "       slt install simplicity-sdk   (pin 2025.12.2), or set SIMPLICITY_SDK_DIR." >&2
  exit 1
fi

OUT="${OD_SLC_OUT:-$(mktemp -d)}"
echo "==> slc:  $SLC"
echo "==> sdk:  $SDK"
echo "==> temp: $OUT"

# -nocp: do NOT copy the SDK sources in. That is the whole point -- a copy is the
# 57 MB tree this repo refused to import.
"$SLC" generate -p "$TARGET_DIR/opendisplay-bg22.slcp" \
  -s "$SDK" -d "$OUT" -nocp \
  --with EFR32BG22C222F352GM40 \
  --generator-timeout 300

if [[ ! -f "$OUT/autogen/linkerfile.ld" || ! -f "$OUT/autogen/gatt_db.c" ]]; then
  echo "error: generation produced no linkerfile.ld / gatt_db.c -- refusing to copy." >&2
  exit 1
fi

mkdir -p "$TARGET_DIR/autogen"
cp -a "$OUT/autogen/." "$TARGET_DIR/autogen/"

# THREE FILES IN autogen/ ARE NOT GENERATED, despite the banner they carry, and the
# generator reverts both edits. They are the only files under autogen/ that this repo
# commits (see the .gitignore negations), so restore them from git afterwards. If this
# is skipped, the resulting firmware links over the bootloader and moves a GATT handle.
PROTECTED=(linkerfile.ld gatt_db.c gatt_db.h)
for f in "${PROTECTED[@]}"; do
  if git -C "$TARGET_DIR" ls-files --error-unmatch "autogen/$f" >/dev/null 2>&1; then
    git -C "$TARGET_DIR" checkout -- "autogen/$f"
    echo "==> restored hand-edited autogen/$f (generator output discarded)"
  else
    echo "WARNING: autogen/$f is not tracked -- the hand-edited version may be lost." >&2
  fi
done

echo "==> wrote $TARGET_DIR/autogen/"
echo "==> generated cmake left for inspection: $OUT/cmake_gcc/opendisplay-bg22.cmake"
echo "==> generated (rejected) linker/GATT left at: $OUT/autogen/"
