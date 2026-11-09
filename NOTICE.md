# Notice and credits

This is an unofficial fan port. It is not made, endorsed or supported by SEGA, Headcannon, Evening Star or the authors of the projects below, and none of them is responsible for it.

## The game and engine (not ours)

- **Sonic Mania** and **Sonic Mania Plus** are the work of **SEGA**, **Headcannon** and **Evening Star** (Christian Whitehead, Simon Thomley, Hunter Bridges and their teams).
  All game code, art, music, sound and levels belong to them. "Sonic the Hedgehog" and "Sonic Mania" are trademarks of SEGA.
- **RSDKv5**, the engine, is by **Evening Star** (Christian Whitehead).
- Please support the official release: https://store.steampowered.com/app/584400/Sonic_Mania/

## Community projects this port builds on (downloaded at build time, not included)

- **RSDKv5 Decompilation** by Rubberduckycooly and RMGRich (chuliRMG): https://github.com/Rubberduckycooly/RSDKv5-Decompilation (license v2.1, in `licenses/`).
- **Sonic Mania Decompilation** by Rubberduckycooly, RMGRich and contributors: https://github.com/RSDKModding/Sonic-Mania-Decompilation (license v1, in `licenses/`).
  Its own notice applies: do not use this project to pirate Sonic Mania.

## Libraries that come with those projects

tinyxml2 (zlib license), stb_vorbis by Sean Barrett (public domain / MIT), miniz (MIT), iniparser (MIT). Their licenses are in the upstream repositories that the build downloads.

## PlayStation 2 toolchain (pulled by Docker at build time, not included)

The **ps2dev** project: PS2 SDK (AFL 3.0 and component licenses), gsKit, audsrv and the IOP modules that get linked into the program (iomanX, fileXio, usbd, usbhdfsd, libsd). See https://github.com/ps2dev.
The emulator **PCSX2** (GPL), used for testing, is not included.

## This port

The PS2 layer (display via the Graphics Synthesizer, pad input, audio output through audsrv, memory card and USB save/boot support, software-rendering optimisations) and the
adaptations to the game's menus for 4:3 screens are the port's own work, released as described in `LICENSE.md`.
