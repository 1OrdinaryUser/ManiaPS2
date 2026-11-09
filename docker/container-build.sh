#!/bin/sh
# Runs inside the build container after container-prepare.sh: compiles the engine and the game for the PS2 EE and links p2_engine.elf.
cd /work/build/prototypes/p2_engine || exit 1

echo "== IOP modules"
sh ./make_irx.sh || { echo "Could not convert the IOP modules"; exit 1; }

echo "== Compiling and linking (about 5 minutes)"
sh ./all.sh
echo

# Every engine unit writes obj/EXIT_<unit>; anything but 0 is a failed compile.
BAD=0
for f in obj/EXIT_*; do
  [ "$(cat "$f")" = "0" ] && continue
  BAD=1
  name=${f#obj/EXIT_}
  echo "---- FAILED: $name"
  [ -f "obj/$name.log" ] && head -30 "obj/$name.log"
done
[ "$BAD" = "0" ] || { echo "Build failed."; exit 1; }
[ -f p2_engine.elf ] || { echo "Build failed: no p2_engine.elf was produced. Link log:"; cat link.log 2>/dev/null | tail -30; exit 1; }

echo "== Result"
ls -l p2_engine.elf
mips64r5900el-ps2-elf-size p2_engine.elf 2>/dev/null || true
echo "BUILD OK"
