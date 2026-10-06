#!/usr/bin/env bash
set -euxo pipefail

git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"

patch_branch() {
  branch="$1"
  git fetch --no-tags origin "$branch"
  git checkout -B work FETCH_HEAD

  python3 - <<'PY'
from pathlib import Path

path = Path('.github/workflows/jjs-libreelec-12.2.1-test.yml')
text = path.read_text(encoding='utf-8')
marker = '          # LibreELEC 12.2.1 builds host tools with -march=native.\n'
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

  test "$(grep -c 'JJS_SOURCE_MIRROR_FIX_V1' .github/workflows/jjs-libreelec-12.2.1-test.yml)" -eq 2
  git diff --check
  git diff -- .github/workflows/jjs-libreelec-12.2.1-test.yml
  git add .github/workflows/jjs-libreelec-12.2.1-test.yml
  git commit -m "Use stable mirrors for LibreELEC 12.2.1 sources"
  git push origin HEAD:"$branch"
}

patch_branch build/21.3-jjs-009-libreelec
patch_branch build/21.3-jjs-009-rpi4

for branch in build/21.3-jjs-009-libreelec build/21.3-jjs-009-rpi4; do
  curl --fail-with-body --silent --show-error \
    -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer ${GH_TOKEN}" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    -d "{\"ref\":\"${branch}\"}" \
    "https://api.github.com/repos/${GITHUB_REPOSITORY}/actions/workflows/jjs-libreelec-12.2.1-test.yml/dispatches"
done

git push origin --delete infra/jjs009-libreelec-source-mirrors
