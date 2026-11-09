# Build environment for the PS2 port: the ps2dev toolchain (PS2 SDK, gsKit, audsrv, EE/IOP compilers), pinned by digest so a build today and a build in a
# year use the same compiler and libraries. If the registry ever drops this digest, replace it with a current ps2dev/ps2dev tag and re-test.
FROM ps2dev/ps2dev@sha256:5476fdfbf34d35eb644fde0a6f6533fb554a4a8171ce53dad7cd1df6077dfb05

# make: the final link uses the PS2 SDK makefiles. git: fetches the upstream decompilation sources at their pinned commits.
RUN apk add --no-cache make git
