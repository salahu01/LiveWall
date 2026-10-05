#!/usr/bin/env bash
# Build the static export and publish it to the gh-pages branch.
set -euo pipefail

cd "$(dirname "$0")/.."
remote=$(git remote get-url origin)
rev=$(git rev-parse --short HEAD)

npm run build
touch out/.nojekyll

cd out
rm -rf .git
git init -q -b gh-pages
git add -A
git commit -qm "deploy: static export of main $rev"
git push -qf "$remote" gh-pages
rm -rf .git

echo "Deployed $rev to gh-pages → https://salahu01.github.io/livewall/"
