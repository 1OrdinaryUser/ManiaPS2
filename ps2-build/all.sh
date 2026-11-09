#!/bin/sh
# Full rebuild: engine core, game logic, link. REV=2 (default, Mania Plus) or REV=3.
rm -f obj/*.o p2_engine.elf
sh ./build.sh
sh ./build_game.sh
