#!/bin/sh
# Runs inside the build container. /work is the host's work directory; /work/stage holds line-ending-normalised copies of this release's
# patches, src and ps2-build folders (build.ps1 puts them there).
#
# 1. Fetch the two upstream decompilations (once; kept pristine in /work/cache).
# 2. Make a fresh working copy in /work/build, apply our patches, add our new source files, and set up the build scripts.
set -e

MANIA_URL=https://github.com/RSDKModding/Sonic-Mania-Decompilation.git
MANIA_COMMIT=9dc699428420d752af9767bdb13f585ee0881bc0   # Sonic Mania Decompilation, v1.1.1 + 15 commits
RSDK_COMMIT=bd5939658fa9987bd2d4384799cacd57aa432eec     # RSDKv5-Decompilation v1.1.1 (the commit the line above pins as a submodule)

STAGE=/work/stage
CACHE=/work/cache/Sonic-Mania-Decompilation
BUILD=/work/build

git config --global --add safe.directory '*'
git config --global advice.detachedHead false

echo "== Upstream sources"
if [ ! -d "$CACHE/.git" ]; then
  echo "Cloning $MANIA_URL (first run only)"
  mkdir -p /work/cache
  git clone "$MANIA_URL" "$CACHE"
else
  echo "Using the cached clone in work/cache"
fi
cd "$CACHE"
git checkout --quiet --force "$MANIA_COMMIT" || { echo "Cannot check out $MANIA_COMMIT (no network?)"; exit 1; }
git submodule sync --quiet --recursive
git submodule update --init --recursive --force
GOT=$(git rev-parse HEAD)
[ "$GOT" = "$MANIA_COMMIT" ] || { echo "Wrong Sonic Mania Decompilation commit: $GOT"; exit 1; }
GOT=$(git -C dependencies/RSDKv5 rev-parse HEAD)
[ "$GOT" = "$RSDK_COMMIT" ] || { echo "Wrong RSDKv5-Decompilation commit: $GOT"; exit 1; }
echo "Sonic Mania Decompilation $MANIA_COMMIT"
echo "RSDKv5-Decompilation      $RSDK_COMMIT"

echo "== Working copy"
rm -rf "$BUILD"
mkdir -p "$BUILD/files/Sonic-Mania-Decompilation" "$BUILD/prototypes/p2_engine"
# a copy without git metadata: the build does not need it, and git apply then works as a plain patch tool
tar -C "$CACHE" --exclude=.git -cf - . | tar -C "$BUILD/files/Sonic-Mania-Decompilation" -xf -

echo "== Applying our patches"
cd "$BUILD/files/Sonic-Mania-Decompilation"
git apply --whitespace=nowarn "$STAGE/patches/sonic-mania-decompilation.patch"
echo "applied sonic-mania-decompilation.patch"
cd dependencies/RSDKv5
git apply --whitespace=nowarn "$STAGE/patches/rsdkv5.patch"
echo "applied rsdkv5.patch"
cp -r "$STAGE/src/rsdkv5/." .
echo "added the PS2 backend source files"

echo "== Build scripts"
cp -r "$STAGE/ps2-build/." "$BUILD/prototypes/p2_engine/"
echo "prepared $BUILD"
