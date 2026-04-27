# Cross-compile IcdUsb WinUSB replacement DLLs for Windows (32-bit) from Linux
# Requires: sudo apt install gcc-mingw-w64-i686

CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall
LDFLAGS = -shared -static-libgcc -lwinusb -lsetupapi -lcfgmgr32 -ladvapi32 -lkernel32 -Wl,--enable-stdcall-fixup
SRC = icdusb_winusb.c

all: ICDUSB.dll ICDUSB2.dll ICDUSB3.dll

ICDUSB.dll: $(SRC) icdusb.def
	$(CC) $(CFLAGS) -o $@ $< icdusb.def $(LDFLAGS)

ICDUSB2.dll: $(SRC) icdusb2.def
	$(CC) $(CFLAGS) -o $@ $< icdusb2.def $(LDFLAGS)

ICDUSB3.dll: $(SRC) icdusb3.def
	$(CC) $(CFLAGS) -o $@ $< icdusb3.def $(LDFLAGS)

clean:
	rm -f ICDUSB.dll ICDUSB2.dll ICDUSB3.dll

debug: CFLAGS += -DDEBUG
debug: clean all

.PHONY: all clean debug
