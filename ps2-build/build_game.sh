#!/bin/sh
# Compile the Sonic Mania game logic (Game.c + Objects/All.c) for the EE, then link the ELF.
export PS2DEV=/usr/local/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2SDK/bin

R=../../files/Sonic-Mania-Decompilation/dependencies/RSDKv5
G=../../files/Sonic-Mania-Decompilation/SonicMania
CC=mips64r5900el-ps2-elf-gcc

PS2_ENT=${PS2_ENT:-0x800}   # must equal the value used by build.sh
PS2_REPLAY_INTS=${PS2_REPLAY_INTS:-0x10000}   # replay buffer size in int32s (stock 0x40000 = 1MB each; this = 256KB). Must equal build.sh.
GDEFS="-DPS2_PORT=1 ${PS2_KEEP_EXIT:+-DPS2_KEEP_EXIT=1} ${PS2_DEBUGMODE:+-DPS2_DEBUGMODE=1} ${PS2_WARP:+-DPS2_WARP_HOOK=1} -DPS2_REPLAY_INTS=$PS2_REPLAY_INTS -DPS2_SCENEENTITY_COUNT=$PS2_ENT -DGAME_INCLUDE_EDITOR=1 -DGAME_VERSION=6 -DMANIA_PREPLUS=0 -DMANIA_FIRST_RELEASE=0 -DRETRO_REVISION=${REV:-2} -DRETRO_USE_MOD_LOADER=0 -DRETRO_MOD_LOADER_VER=2 -DSCREEN_XMAX=432"
GFLAGS="-O2 -G0 -D_EE -w $GDEFS -I$G -I$G/Objects -I$PS2SDK/ee/include -I$PS2SDK/common/include"

mkdir -p obj
sh ./build_irx.sh
# Game objects are skipped when already built (they do not depend on the engine); all.sh removes them.
if [ ! -f obj/game_Game.o ]; then
  $CC -std=gnu11 $GFLAGS -c $G/Game.c -o obj/game_Game.o > obj/game_Game.log 2>&1; echo $? > obj/EXIT_game_Game &
else echo 0 > obj/EXIT_game_Game; fi
if [ ! -f obj/game_All.o ]; then
  $CC -std=gnu11 $GFLAGS -c $G/Objects/All.c -o obj/game_All.o > obj/game_All.log 2>&1; echo $? > obj/EXIT_game_All &
else echo 0 > obj/EXIT_game_All; fi
wait
echo "Game=$(cat obj/EXIT_game_Game) All=$(cat obj/EXIT_game_All)"

if [ "$(cat obj/EXIT_game_Game)" = 0 ] && [ "$(cat obj/EXIT_game_All)" = 0 ]; then
  make -f Makefile.link clean > link.log 2>&1
  make -f Makefile.link >> link.log 2>&1
  echo "LINK=$?"
  ls -l p2_engine.elf 2>&1
  size p2_engine.elf 2>&1 || mips64r5900el-ps2-elf-size p2_engine.elf 2>&1
fi
echo DONE
