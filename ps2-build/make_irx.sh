#!/bin/sh
# Turns the PS2 SDK's IOP modules into C arrays (bin2c) so they are linked into the ELF. A console that boots from USB has no host: to load them from.
# Run inside the build container from the p2_engine build directory.
set -e
S=/usr/local/ps2dev/ps2sdk
OUT=$PWD/irx_gen
mkdir -p $OUT
for m in iomanX fileXio usbd usbhdfsd libsd audsrv; do
  $S/bin/bin2c $S/iop/irx/$m.irx $OUT/${m}_irx.c ${m}_irx
  echo "generated $OUT/${m}_irx.c"
done
