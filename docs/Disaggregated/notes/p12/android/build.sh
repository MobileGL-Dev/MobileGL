#!/usr/bin/env bash
# build.sh <sha>: the window-2 builder (10-build.sh) on a detached tree of the P12 package tree.
#
# 10-build.sh would create the tree with `git -C ~/w7/pipe worktree add` and copy submodules from
# pipe, and falls back to `git lfs pull` for pointer fixtures. The P12 rules forbid touching pipe
# and pulling LFS, so the tree is prepared HERE, from the package tree (same object store): the
# detached worktree, its submodules and glslang External copied from the package tree's copies
# (the p12 branch moves no gitlink), and the hydrated fixtures copied + assume-unchanged. Then
# 10-build.sh runs with W2_PIPE=<package tree>, finds everything in place and only builds.
set -euo pipefail
PKG=/home/swung/w7/p12-onscreen
SHA=$(git -C "$PKG" rev-parse --verify "$1^{commit}")
SHA8=${SHA:0:8}
STAMP=${2:-p12w1-$SHA8}
TREE=$HOME/w7/p7w7-tree-$SHA8
if [ ! -e "$TREE/.git" ]; then
    git -C "$PKG" worktree add --detach "$TREE" "$SHA"
fi
[ "$(git -C "$TREE" rev-parse HEAD)" = "$SHA" ] || { echo "tree $TREE not at $SHA"; exit 1; }
for path in $(git -C "$TREE" submodule status | awk '{print $2}'); do
    want=$(git -C "$TREE" ls-tree HEAD "$path" | awk '{print $3}')
    base=$(git -C "$PKG" ls-tree HEAD "$path" | awk '{print $3}')
    [ "$want" = "$base" ] || { echo "gitlink $path differs from the package tree's ($want vs $base)"; exit 1; }
    if [ -z "$(ls -A "$TREE/$path" 2>/dev/null)" ]; then
        rm -rf "$TREE/$path"
        cp -a "$PKG/$path" "$TREE/$path"
        rm -f "$TREE/$path/.git"
        echo "submodule $path copied from the package tree ($want)"
    fi
done
[ -d "$TREE/3rdparty/glslang/External/spirv-tools" ] || cp -a "$PKG/3rdparty/glslang/External" "$TREE/3rdparty/glslang/"
git -C "$TREE" ls-files -z tools/trace_replay/fixtures | while IFS= read -r -d "" f; do
    if head -c 40 "$TREE/$f" 2>/dev/null | grep -q git-lfs && ! head -c 40 "$PKG/$f" | grep -q git-lfs; then
        cp "$PKG/$f" "$TREE/$f"
    fi
done
git -C "$TREE" ls-files -z tools/trace_replay/fixtures | xargs -0 -r git -C "$TREE" update-index --assume-unchanged
if head -c 40 "$TREE/tools/trace_replay/fixtures/openra.tgz" | grep -q git-lfs; then
    echo "openra.tgz is still an LFS pointer - refusing (no git lfs pull)"; exit 1
fi
echo "tree ready: $TREE @ $SHA8, stamp $STAMP"
W2_PIPE=$PKG bash /home/swung/w7/notes/p7/window2/10-build.sh "$SHA" "$STAMP"
