#!/usr/bin/env bash
# Reproduce the isolated PSP build for this import and the pinned GCC 15 SDK.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:?use baseline or native}
case "$mode" in
  baseline) tinygl=--enable-tinygl ;;
  native) tinygl=--disable-tinygl ;;
  *) exit 2 ;;
esac
: "${LAB:?set LAB to a private directory outside the checkout}"
lab=$(realpath -m "$LAB")
case "$lab/" in "$repo/"*) echo "Use a build directory outside the checkout" >&2; exit 2;; esac
if [[ -z ${PSPDEV:-} ]]; then
  if [[ -x "$HOME/.local/share/stark-psp-sdk/bin/psp-config" ]]; then
    PSPDEV="$HOME/.local/share/stark-psp-sdk"
  else
    PSPDEV=/tmp/pspdev
  fi
fi
export PSPDEV
export PATH="$PSPDEV/bin:$PATH"
PSPSDK=$(psp-config --pspsdk-path)
export PSPSDK
up=16159a2afe14d0ecdfdf9547906a5801c2083cb6
mkdir -p "$lab/build-$mode"
cd "$lab/build-$mode"
for path in backends/graphics/graphics.h backends/mixer/mixer.h; do
  if [[ -f "$repo/$path" ]]; then
    # -I. precedes the source directory. Never shadow a now-restored header.
    if [[ -e "$path" ]]; then echo "Use a fresh lab after restoring $path" >&2; exit 1; fi
    continue
  fi
  blob=$(git -C "$repo" rev-parse "$up:$path")
  mkdir -p "$(dirname "$path")"
  if [[ ! -f "$path" ]]; then
    if GIT_NO_LAZY_FETCH=1 git -C "$repo" cat-file -e "$blob" 2>/dev/null; then
      git -C "$repo" cat-file blob "$blob" > "$path.tmp"
    else
      curl -fLsS --retry 2 "https://raw.githubusercontent.com/scummvm/scummvm/$up/$path" -o "$path.tmp"
    fi
    test "$(git hash-object "$path.tmp")" = "$blob"
    mv "$path.tmp" "$path"
  fi
  test "$(git hash-object "$path")" = "$blob"
done
# Keep raw link evidence and regenerate the import order when dependencies change.
bash "$repo/configure" --host=psp --disable-all-engines --enable-engine=stark "$tinygl" 2>&1 | tee configure.out
grep -Eq '^ENABLE_STARK[[:space:]]*=[[:space:]]*STATIC_PLUGIN' config.mk
if [[ -f scummvm.elf ]]; then mv -f scummvm.elf previous-link.elf; fi
make -j"${JOBS:-4}" scummvm.elf 2>&1 | tee elf.log
cp mapfile.txt raw-link.map
python3 - "$PSPDEV" <<'PY'
from pathlib import Path
import re
import sys
script = Path(sys.argv[1], 'psp/lib/ldscripts/elf_mipsallegrexel_psp.x').read_text()
modules = sorted(set(re.findall(r'__stub_module_([\w]+)\s*$', Path('raw-link.map').read_text(), re.M)))
if not modules:
    raise SystemExit('SDK stub module names absent from map')
for section in ('.sceStub.text', '.rodata.sceNid'):
    wildcard = '*(' + section + ')'
    if script.count(wildcard) != 1:
        raise SystemExit('Unexpected SDK linker script; inspect before building')
    groups = [f'*:{module}_[0-9]*.o({section})' for module in modules]
    script = script.replace(wildcard, ' '.join(groups + [wildcard]))
Path('group-imports.ld').write_text(script)
Path('diagnostic.mk').write_text('LDFLAGS += -Wl,-T$(CURDIR)/group-imports.ld\n')
PY
mv -f scummvm.elf raw-link.elf
make -f Makefile -f diagnostic.mk pack_pbp 2>&1 | tee package.log
if grep -qi 'stubs out of order' package.log; then
  echo 'Import grouping failed. Keep logs and diagnose before boot.' >&2; exit 1
fi
psp-size scummvm.elf
sha256sum EBOOT.PBP scummvm.elf param.sfo group-imports.ld
