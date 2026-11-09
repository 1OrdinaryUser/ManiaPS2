#!/bin/sh
# Compile RSDK engine core for the PS2 EE. Objects go to ./obj, per-file logs to ./obj/*.log
# Usage: build.sh [files...]   (default: all engine sources)
export PS2DEV=/usr/local/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2SDK/bin

R=../../files/Sonic-Mania-Decompilation/dependencies/RSDKv5
E=$R/RSDKv5
D=$R/dependencies/all
G=../../files/Sonic-Mania-Decompilation/SonicMania

CXX=mips64r5900el-ps2-elf-g++
CC=mips64r5900el-ps2-elf-gcc

REV=${REV:-2}
# Scene entity slots (engine and game MUST agree, see build_game.sh). Default 0x400; the stock value is 0x800.
# Scene entity slots: the stock 0x800. Measured over all 29 Mania Mode acts (zones_all.txt): up to 2017 entities pass the filter and the highest
# slot ID is 2371, so anything below the stock value drops objects (1024 dropped 100..993 per act in 17 acts).
PS2_ENT=${PS2_ENT:-0x800}
# Render width in pixels (stock 424 = 16:9, letterboxed by the HAL; 320 = 4:3 fills the TV but crops the UI edges).
PS2_PIXWIDTH=${PS2_PIXWIDTH:-424}
# Replay buffer size (int32s each). The Game and the engine MUST agree: the engine remaps GameConfig.bin slots by this value.
PS2_REPLAY_INTS=${PS2_REPLAY_INTS:-0x10000}
DEFS="${PS2_CLASS_TOUR:+-DPS2_CLASS_TOUR=1} ${PS2_TOUR_ONLY:+-DPS2_TOUR_ONLY=\"$PS2_TOUR_ONLY\"} -DPS2_REPLAY_INTS=$PS2_REPLAY_INTS -DPS2_SCENEENTITY_COUNT=$PS2_ENT -DRETRO_REVISION=$REV -DRETRO_USE_MOD_LOADER=0 -DRETRO_STANDALONE=0 -DRSDK_AUTOBUILD=0 -DRETRO_DISABLE_LOG=0 -DSCREEN_XMAX=432"
INCS="-Ishim -I$E -I$D -I$D/tinyxml2 -I$D/iniparser -I$D/miniz -I$G -I$G/Objects -I$PS2SDK/ee/include -I$PS2SDK/common/include -I$PS2DEV/gsKit/include"
COMMON="-O2 -G0 -D_EE -fpermissive -w $DEFS $INCS"

mkdir -p obj
rm -f obj/*.log obj/EXIT_*

SRCS="main Core/RetroEngine Core/Math Core/Reader Core/Link Dev/Debug Storage/Storage Storage/Text Graphics/Drawing Graphics/Scene3D Graphics/Animation Graphics/Sprite Graphics/Palette Graphics/Video Audio/Audio Input/Input Scene/Scene Scene/Collision Scene/Object Scene/Objects/DefaultObject Scene/Objects/DevOutput User/Core/UserAchievements User/Core/UserCore User/Core/UserLeaderboards User/Core/UserPresence User/Core/UserStats User/Core/UserStorage"
[ -n "$1" ] && SRCS="$*"

for s in $SRCS; do
  n=$(echo $s | tr '/' '_')
  src=$E/RSDK/$s.cpp; [ "$s" = main ] && src=$E/main.cpp
  # Experiment: the software rasteriser is the hot unit, build it more aggressively (set DRAW_OPT="" to disable).
  # Hot unit: Graphics/Drawing (sprite/rect rasteriser): -O3 took the menu frame from 16.0 to 11.1 ms.
  # PS2_CLASS_TOUR=1 builds the developer class-draw audit (Scene/Object.cpp); header-visible, so it is applied to every unit.
  # Measured and rejected: the same flags on Scene/Scene (tile layers) gave no gain (13.75 -> 14.15 ms), so it stays at -O2.
  XOPT=""
  case "$s" in
    Graphics/Drawing) XOPT="${DRAW_OPT--O3 -funroll-loops}" ; ;;
    # (Audio/Audio at -O3 -funroll-loops was measured on 2026-10-17: mix cost stayed 3.6-4.1 ms per chunk, no change, so it stays at -O2.)
  esac
  ( $CXX -std=gnu++11 $COMMON $XOPT -c $src -o obj/$n.o > obj/$n.log 2>&1; echo $? > obj/EXIT_$n ) &
  # limit parallelism to 4
  while [ $(jobs -r 2>/dev/null | wc -l) -ge 4 ]; do sleep 1; done
done
wait

( $CC -std=gnu11 $COMMON -c $E/RSDK/PS2/ps2_hal.c -o obj/ps2_hal.o > obj/ps2_hal.log 2>&1; echo $? > obj/EXIT_ps2_hal )
for f in tinyxml2/tinyxml2 iniparser/iniparser iniparser/dictionary; do
  n=dep_$(echo $f | tr '/' '_')
  $CXX -std=gnu++11 $COMMON -c $D/$f.cpp -o obj/$n.o > obj/$n.log 2>&1; echo $? > obj/EXIT_$n
done
$CC -std=gnu11 $COMMON -c $D/miniz/miniz.c -o obj/dep_miniz.o > obj/dep_miniz.log 2>&1; echo $? > obj/EXIT_dep_miniz

for f in obj/EXIT_*; do echo "$f=$(cat $f)"; done
echo DONE
