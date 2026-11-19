# Cross-build akstream for the AK3918.
#
# Toolchain and SDK come from outside this repo; see
# reference/anyka3918-gc1084-camera-main/docs/cross-compiling.md for the pinned
# versions, and set ANYKA_ROOT to wherever they were installed.
#
#   make            -> akstream (ARMv5TEJ soft-float, uClibc)
#   make install    -> uploads to the camera with tools/rtsp.py-style FTP
#
# The libraries linked here are NOT the SDK dump's own copies: they are the
# md5-identical ones from rootfs_v1basic, which match the camera's
# /mnt/Factory/custom/lib byte for byte (libmpi_venc 19bb0f80..., libplat_vi
# f924ed82..., libakstreamenc 3dae5c20...). Linking a neighbouring build of the
# SDK is how ABI bugs get shipped to a board you cannot debug interactively.

ANYKA_ROOT ?= $(HOME)/opt/anyka_v380ipcam_experiments
TOOLCHAIN  ?= $(HOME)/opt/arm-anykav200-crosstool
CROSS      := $(TOOLCHAIN)/usr/bin/arm-anykav200-linux-uclibcgnueabi-
CC         := $(CROSS)gcc

SDK_INC    := $(ANYKA_ROOT)/akv300-extract/libplat/include \
              $(ANYKA_ROOT)/akv300-extract/libmpi/include
# rootfs_v1basic holds the libraries this camera generation actually runs.
SDK_LIB    := $(ANYKA_ROOT)/rootfs_v1basic/rootfs/usr/lib
CAM_LIB    := /mnt/Factory/custom/lib

CFLAGS     := -O2 -Wall -Wextra -std=gnu99 $(addprefix -I,$(SDK_INC))
# -lplat_thread and -ldl are deliberate: the SDK libraries call ak_thread_* and
# dladdr without recording those dependencies in their .so files, so the link
# only succeeds if the caller adds them (learned from link errors, not headers).
# -lakstreamenc likewise: libmpi_venc is only a thin wrapper, the encoder itself
# (VideoStream_Enc_*) lives in libakstreamenc, which is also in the stock rtsp's
# NEEDED list.
# --allow-shlib-undefined: the vendor .so files carry versioned libc references
# (@GLIBC_2.4) left over from wherever they were first built. The device's
# uClibc does not do symbol versioning and resolves them at run time, so the
# build host must not insist on resolving them itself.
LDFLAGS    := -L$(SDK_LIB) -Wl,-rpath-link,$(SDK_LIB) -Wl,--allow-shlib-undefined
LIBS       := -lmpi_venc -lakstreamenc -lplat_vi -lplat_vpss -lplat_common \
              -lplat_thread -lakuio -lakispsdk -ldl -lpthread
# -lplat_drv: the light dependent resistor and the IR-cut filter (ak_drv_ir_*).
# The server needs it from the moment it switches day/night itself instead of
# leaving the ISP in whatever mode the conf file booted into.
LIBS       += -lplat_drv
# -lmpi_osd for the stream server only: that is what burns the clock into the
# picture. Per-target rather than in LIBS because these are shared libraries and
# an unused -l still lands in the binary's NEEDED list, which would leave the
# probes requiring a library they never call - and on this board a missing .so
# means the program does not start at all. libmpi_osd.so is not in the vendor's
# /mnt/Factory/custom/lib; it comes from the same rootfs_v1basic tree as the
# other libraries here and is deployed to /mnt/camrtsp/lib.
OSD_LIBS   := -lmpi_osd

CAM_HOST  ?= 192.168.10.46

all: akstream probe akrtsp

akstream: akstream.c vi_abi.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)
	@echo -n 'target: '
	@$(TOOLCHAIN)/usr/bin/arm-anykav200-linux-uclibcgnueabi-readelf -h $@ | grep -E 'Machine|Flags' | tr '\n' ' '
	@echo

probe: akvi_probe.c vi_abi.h
	$(CC) $(CFLAGS) -o akvi_probe $< $(LDFLAGS) $(LIBS)

# The light sensor / IR-cut driver lives in libplat_drv, which the stream server
# does not need until it switches day/night itself - so this tool links it
# explicitly rather than growing the server's link line by accident.
irprobe: akir_probe.c
	$(CC) $(CFLAGS) -o akir_probe $< $(LDFLAGS) -lplat_drv -lplat_common -lplat_thread -ldl -lpthread
	$(CROSS)strip -o akir_probe.stripped akir_probe
	@ls -l akir_probe.stripped

akrtsp: akrtsp.c vi_abi.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS) $(OSD_LIBS)

check: akstream akrtsp
	$(TOOLCHAIN)/usr/bin/arm-anykav200-linux-uclibcgnueabi-readelf -d akrtsp | grep NEEDED
	$(CROSS)strip -o akstream.stripped akstream
	$(CROSS)strip -o akrtsp.stripped akrtsp
	ls -l akstream akstream.stripped akrtsp akrtsp.stripped

install: akstream akrtsp
	@python3 -c "import ftplib;f=ftplib.FTP('$(CAM_HOST)');f.login('root','anyka');f.storbinary('STOR /tmp/akstream',open('akstream.stripped','rb'));f.storbinary('STOR /tmp/akrtsp',open('akrtsp.stripped','rb'));f.quit();print('uploaded /tmp/akstream and /tmp/akrtsp on $(CAM_HOST)')"

clean:
	rm -f akstream akstream.stripped akvi_probe akrtsp akrtsp.stripped akir_probe akir_probe.stripped

.PHONY: all check install clean
