#!/bin/sh
# The audio IOP modules are loaded at run time from the ELF's directory (see ps2_audio_init): copy them next to the ELF's run directory.
# The USB IOP modules (iomanX, fileXio, usbd, usbhdfsd) and the audio modules (libsd, audsrv) are linked INTO the ELF instead, because a console booting from USB has no host: to load
# them from. irx_gen/*.c are bin2c arrays made by tools/make_usb_image.sh; they are compiled here once.
export PS2DEV=/usr/local/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2SDK/bin
mkdir -p run obj
for m in audsrv libsd; do
  [ -f run/$m.irx ] || cp $PS2SDK/iop/irx/$m.irx run/$m.irx
done
rm -f obj/audsrv_irx.o obj/libsd_irx.o
for m in iomanX fileXio usbd usbhdfsd libsd audsrv; do
  if [ -f irx_gen/${m}_irx.c ] && [ ! -f obj/irx_$m.o ]; then
    mips64r5900el-ps2-elf-gcc -O2 -G0 -D_EE -c irx_gen/${m}_irx.c -o obj/irx_$m.o || echo "FAILED to compile irx_$m"
  fi
done
