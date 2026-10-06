#!/usr/bin/env bash
set -euxo pipefail

make_blob() {
  branch="$1"
  label="$2"

  git reset --hard
  git clean -fd
  git fetch --no-tags origin "$branch"
  git checkout -B work FETCH_HEAD

  python3 - <<'PY'
from pathlib import Path

path = Path('.github/workflows/jjs-libreelec-12.2.1-test.yml')
text = path.read_text(encoding='utf-8')
marker = '          # LibreELEC 12.2.1 builds host tools with -march=native.'
tag = 'JJS_SOURCE_MIRROR_FIX_V1'

if tag in text:
    raise SystemExit('Mirror fix already present; refusing to duplicate it')

count = text.count(marker)
if count != 2:
    raise SystemExit(f'Expected exactly 2 insertion points, found {count}')

block = '''          # JJS_SOURCE_MIRROR_FIX_V1: GitHub runners currently cannot reliably
          # reach ftp.gnu.org or Savannah. Use mirrors carrying the identical
          # release archives; LibreELEC's pinned SHA256 values remain authoritative.
          find LibreELEC.tv/packages -type f -name package.mk -print0 | xargs -0 sed -i \\
            -e 's#https://ftp.gnu.org/pub/gnu/#https://mirrors.kernel.org/gnu/#g' \\
            -e 's#https://ftp.gnu.org/gnu/#https://mirrors.kernel.org/gnu/#g'

          sed -i \\
            's#http://download.savannah.nongnu.org/releases/attr/${PKG_NAME}-${PKG_VERSION}.tar.gz#https://mirror.fi.ossplanet.net/nongnu/attr/${PKG_NAME}-${PKG_VERSION}.tar.gz#' \\
            LibreELEC.tv/packages/devel/attr/package.mk

          sed -i \\
            's#https://download.savannah.gnu.org/releases/freetype/freetype-${PKG_VERSION}.tar.xz#https://downloads.sourceforge.net/project/freetype/freetype2/${PKG_VERSION}/freetype-${PKG_VERSION}.tar.xz#' \\
            LibreELEC.tv/packages/print/freetype/package.mk

'''

text = text.replace(marker, block + marker)
path.write_text(text, encoding='utf-8')
PY

  file=.github/workflows/jjs-libreelec-12.2.1-test.yml
  test "$(grep -c 'JJS_SOURCE_MIRROR_FIX_V1' "$file")" -eq 2
  git diff --check
  git diff -- "$file"

  python3 - "$file" > /tmp/blob-request.json <<'PY'
import base64
import json
from pathlib import Path
import sys

raw = Path(sys.argv[1]).read_bytes()
json.dump({'content': base64.b64encode(raw).decode('ascii'), 'encoding': 'base64'}, sys.stdout)
PY

  curl --fail-with-body --silent --show-error \
    -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer ${GH_TOKEN}" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    --data-binary @/tmp/blob-request.json \
    "https://api.github.com/repos/${GITHUB_REPOSITORY}/git/blobs" \
    > /tmp/blob-response.json

  sha="$(python3 -c 'import json; print(json.load(open("/tmp/blob-response.json"))["sha"])')"
  test -n "$sha"
  echo "PATCHED_BLOB_${label}=${sha}"
}

make_blob build/21.3-jjs-009-libreelec X86
make_blob build/21.3-jjs-009-rpi4 RPI4
