# License

This repository contains **no SEGA, Headcannon or Evening Star game data, and no complete copy of their code.** The only material derived from their code is
the patch files in `patches/`, which modify two community decompilations and are distributed under those decompilations' licenses.

## 1. Port source: `src/` and `patches/` (non-commercial)

The new source files in `src/` are backends for the RSDKv5 engine decompilation (they implement its platform interfaces for the PS2), and `patches/` modifies the
Sonic Mania Decompilation and the RSDKv5 Decompilation. They are therefore distributed under the same terms as those projects:

- `patches/rsdkv5.patch` and everything in `src/`: **RSDKv5(U) Decompilation Source Code License v2.1** (`licenses/RSDKv5-Decompilation-LICENSE.md`).
- `patches/sonic-mania-decompilation.patch`: **Sonic Mania Decompilation Source Code License v1** (`licenses/Sonic-Mania-Decompilation-LICENSE.md`).

In short (read the licenses, which are the authority): no commercial use of any kind, the original developers and the decompilation authors must be credited, no game assets
may be distributed, the license must accompany modified source, and pre-built executables handed to anyone must have DLC disabled by default. **This project distributes no executables.**

## 2. Build tooling and documentation (MIT)

`build.bat`, `build.ps1`, `docker/`, `ps2-build/`, `tools/`, `docs/`, `data-hashes.txt`, `.gitignore` and `.gitattributes` are copyright (c) 2026 the Sonic Mania PS2 port contributors and
are released under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

This does not change section 1: running the build produces a program that is a derivative of the decompilations and falls under their licenses.

## 3. Things this repository does not license

The game itself, its data (`Data.rsdk`), its trademarks, and everything the build downloads from other projects keep their own licenses and owners (see `NOTICE.md`).
