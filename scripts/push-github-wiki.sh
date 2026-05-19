#!/usr/bin/env bash
# Push wiki/*.md to the GitHub wiki git backend (repo.wiki.git).
# Requires: gh auth login, wiki enabled on the repository.
# If clone fails with "Repository not found", initialize once via:
#   gh workflow run publish-wiki.yml
# or create any page on the repo Wiki tab, then re-run this script.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIKI_SRC="${ROOT}/wiki"
REPO="${GITHUB_REPOSITORY:-shomanjk/esp32_balboa_spa}"
OWNER="${REPO%%/*}"
NAME="${REPO##*/}"
TMP="${TMPDIR:-/tmp}/${NAME}.wiki"

if [[ ! -d "${WIKI_SRC}" ]] || ! compgen -G "${WIKI_SRC}/*.md" >/dev/null; then
  echo "No markdown files in ${WIKI_SRC}" >&2
  exit 1
fi

TOKEN="$(gh auth token)"
REMOTE="https://${OWNER}:${TOKEN}@github.com/${OWNER}/${NAME}.wiki.git"

rm -rf "${TMP}"
if git clone "${REMOTE}" "${TMP}" 2>/dev/null; then
  :
else
  echo "Wiki git repo not found (not bootstrapped yet)." >&2
  echo "See wiki/BOOTSTRAP.md — create Home once on:" >&2
  echo "  https://github.com/${OWNER}/${NAME}/wiki/_new?wiki%5Btitle%5D=Home" >&2
  echo "Then run: gh workflow run publish-wiki.yml --ref $(git -C "${ROOT}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo ESP32)" >&2
  exit 1
fi

for f in "${WIKI_SRC}"/*.md; do
  case "$(basename "$f")" in
    BOOTSTRAP.md) continue ;;
  esac
  cp "$f" "${TMP}/"
done
rm -f "${TMP}/BOOTSTRAP.md"
cd "${TMP}"
git add -A
if git diff --staged --quiet; then
  echo "Wiki already up to date."
  exit 0
fi
git commit -m "Sync wiki from $(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo local)"
git push origin HEAD

echo "Wiki updated: https://github.com/${OWNER}/${NAME}/wiki"
