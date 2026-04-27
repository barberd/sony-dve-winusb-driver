# Sony DVE WinUSB Driver

Copyright Don Barber, 2026.

Drop-in replacement for Sony's `ICDUSB.dll`, `IcdUsb2.dll`, and `IcdUsb3.dll` that uses WinUSB instead of Sony's kernel drivers. Allows Sony Digital Voice Editor (DVE) 3 to communicate with ICD-series recorders on modern Windows (10/11).

## How it works

Sony's software stack loads DLLs dynamically:
```
DVEdit.exe → PXVoice.dll → IcdNStor3.dll → IcdComm4.dll
  → LoadLibrary("IcdUsb3.dll") → GetProcAddress("SonyUsbOpen", ...)
```

The original DLLs talk to Sony's kernel drivers (`ICDUSB3.sys`, etc.) via `DeviceIoControl`. These kernel drivers are unsigned and won't load on modern Windows. Our replacement DLLs implement the same 18 `SonyUsb*` exports using WinUSB, which is built into Windows.

## Supported devices

| PID | Series | DLL chain |
|-----|--------|-----------|
| 0x0048 | ICD-BP | icdcomm → ICDUSB |
| 0x007F | ICD-MS | icdcomm → ICDUSB |
| 0x00BF | ICD-S | icdcomm → ICDUSB |
| 0x015F | ICD-BM | icdcomm → ICDUSB |
| 0x0103, 0x0116 | ICD-ST | IcdComm2/3 → IcdUsb2 |
| 0x016D | ICD-SX | IcdComm2/3 → IcdUsb2 |
| 0x0387 | ICD-PX | IcdComm4 → IcdUsb3 |
| 0x03F9 | ICD-AX | IcdComm4 → IcdUsb3 |

All three DLLs are built from the same source with different export ordinals. Any Sony VID_054C recorder with WinUSB bound via Zadig should work.

## Applicable models

These DLLs replace the USB driver layer for Sony ICD-series recorders that use proprietary USB drivers:

- ICD-BP Series (PID 0x0048)
- ICD-MS Series (PID 0x007F)
- ICD-S Series (PID 0x00BF)
- ICD-BM Series (PID 0x015F)
- ICD-ST Series (PID 0x0103, 0x0116)
- ICD-SX Series (PID 0x016D)
- ICD-PX Series (PID 0x0387) — tested with ICD-PX720
- ICD-AX Series (PID 0x03F9)

Other series listed as DVE-compatible (ICD-P, ICD-MX) use mass storage or serial connections and don't need this driver replacement.

## Building (on Linux)

```
sudo apt install gcc-mingw-w64-i686
make
```

Produces `ICDUSB.dll`, `IcdUsb2.dll`, and `IcdUsb3.dll`.

For debug builds with DebugView output:
```
make debug
```

## Installation

1. Download the [latest release](https://github.com/barberd/sony-dve-winusb-driver/releases) zip and extract it.
2. Install [Sony Digital Voice Editor 3.2](https://www.sony-asia.com/electronics/support/digital-voice-recorders-icd-series/icd-px720/downloads/Y0015358). A warning will pop up that it cannot install the driver — just close it. **Do not reboot after install.**
3. Delete `C:\Windows\System32\drivers\PxHlpa64.sys` (Roxio driver that causes BSODs on modern Windows).
4. Copy `ICDUSB.dll`, `IcdUsb2.dll`, and `IcdUsb3.dll` to `C:\Windows\SysWOW64\`, replacing the originals.
5. Install [Zadig](https://zadig.akeo.ie) and assign the **WinUSB** driver to your Sony recorder.

Or run `install.bat` as Administrator to do steps 3–4 automatically.

Install order matters: DVE's installer registers `ICDUSB3.inf` which binds the device to `ICDUSB3.sys`. Zadig must be run *after* DVE install to override with WinUSB.

## Notes

- The CD burning functionality of DVE won't work. Use alternative software for that.
- **DVE 3.3 does not install cleanly on Windows 10/11.** The installer and its bundled driver installer (PXDriver.exe) both contain OS version checks (`OSVersion.dll`) that reject anything newer than Windows 7. It may be possible to work around these, but DVE 3.2 is known to work and is recommended.

## Debugging

Build with `make debug`, then monitor with [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) (run as Admin, enable Capture Global Win32). Messages are prefixed with `IcdUsb-WinUSB:`.

## GenAI Usage

Developed with assistance from [Kiro](https://kiro.dev).

## License

GPLv3
