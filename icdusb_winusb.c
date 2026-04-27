/*
 * WinUSB replacement for Sony ICD-series recorder DLLs
 *
 * Implements the 18-function SonyUsb API using WinUSB instead of Sony's
 * unsigned kernel drivers. Built as three DLLs (ICDUSB.dll, IcdUsb2.dll,
 * IcdUsb3.dll) from this single source with different .def files.
 *
 * Requires WinUSB driver installed via Zadig for the Sony IC Recorder.
 *
 * Copyright (C) 2026 Don Barber — GPLv3
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define SONY_VID        0x054c
#define WVALUE_MAGIC    0xabab
#define EP_BULK_IN      0x81
#define EP_BULK_OUT     0x02

static HANDLE g_device = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_winusb = NULL;

#define MAX_DEVICES 20
static char g_device_paths[MAX_DEVICES][512];
static unsigned int g_device_count = 0;

#ifdef DEBUG
static void dbg(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}
#else
#define dbg(...) ((void)0)
#endif

/*
 * WinUSB device setup class GUID — Zadig/libwdi registers all WinUSB
 * devices under this class ("Universal Serial Bus devices" in Device Manager).
 */
static const GUID WINUSB_CLASS_GUID = {0x88BAE032, 0x5A81, 0x49F0,
    {0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6}};

/* Standard USB device interface GUID (GUID_DEVINTERFACE_USB_DEVICE) */
static GUID USB_DEVICE_IFACE = {0xA5DCBF10, 0x6530, 0x11D2,
    {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

/*
 * Get the device interface path using CM_Get_Device_Interface_List
 * with GUID_DEVINTERFACE_USB_DEVICE. No registry lookup needed.
 */
static char *get_device_path(HDEVINFO devinfo, SP_DEVINFO_DATA *devdata) {
    char instance_id[256];
    if (!SetupDiGetDeviceInstanceIdA(devinfo, devdata, instance_id,
                                      sizeof(instance_id), NULL))
        return NULL;
    dbg("IcdUsb-WinUSB: instance=%s\n", instance_id);

    /* Log driver service for diagnostics */
    char svc[128] = {0};
    SetupDiGetDeviceRegistryPropertyA(devinfo, devdata, SPDRP_SERVICE,
                                       NULL, (BYTE *)svc, sizeof(svc), NULL);
    dbg("IcdUsb-WinUSB: current driver service=%s\n", svc);

    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeA(&len, &USB_DEVICE_IFACE,
            instance_id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    dbg("IcdUsb-WinUSB: CM_Get_Device_Interface_List_Size cr=%lu len=%lu\n",
        (unsigned long)cr, (unsigned long)len);
    if (cr != CR_SUCCESS || len <= 1)
        return NULL;

    char *buf = malloc(len);
    if (!buf) return NULL;

    cr = CM_Get_Device_Interface_ListA(&USB_DEVICE_IFACE, instance_id, buf, len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr == CR_SUCCESS && buf[0] != '\0') {
        dbg("IcdUsb-WinUSB: interface=%s\n", buf);
        return buf;
    }
    dbg("IcdUsb-WinUSB: CM_Get_Device_Interface_List cr=%lu buf[0]=0x%02x\n",
        (unsigned long)cr, (unsigned char)buf[0]);
    free(buf);
    return NULL;
}

/* Find and open a Sony WinUSB device */
static BOOL winusb_open(void) {
    HDEVINFO devinfo;
    SP_DEVINFO_DATA devdata = {sizeof(SP_DEVINFO_DATA)};

    devinfo = SetupDiGetClassDevsA(&WINUSB_CLASS_GUID, NULL, NULL, DIGCF_PRESENT);
    if (devinfo == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devinfo, i, &devdata); i++) {
        char hwid[256] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(devinfo, &devdata, SPDRP_HARDWAREID,
                                                NULL, (BYTE *)hwid, sizeof(hwid), NULL))
            continue;
        if (!strstr(hwid, "VID_054C"))
            continue;

        char *path = get_device_path(devinfo, &devdata);
        if (!path) continue;

        g_device = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, NULL);
        if (g_device != INVALID_HANDLE_VALUE && WinUsb_Initialize(g_device, &g_winusb)) {
            dbg("IcdUsb-WinUSB: opened %s\n", path);
            found = TRUE;
        } else {
            dbg("IcdUsb-WinUSB: open failed err=%lu\n", GetLastError());
            if (g_device != INVALID_HANDLE_VALUE) CloseHandle(g_device);
            g_device = INVALID_HANDLE_VALUE;
        }
        free(path);
        if (found) break;
    }
    SetupDiDestroyDeviceInfoList(devinfo);
    return found;
}

static void winusb_close(void) {
    if (g_winusb) { WinUsb_Free(g_winusb); g_winusb = NULL; }
    if (g_device != INVALID_HANDLE_VALUE) { CloseHandle(g_device); g_device = INVALID_HANDLE_VALUE; }
}

/*
 * Exported functions — match original IcdUsb3.dll signatures exactly.
 * All use __stdcall (default for DLL exports on x86).
 */

/*
 * GUID-to-PID mapping. The device interface GUIDs are registered by Sony's
 * kernel drivers. When the caller passes a GUID to SonyUsbEnumerateDevicesGuid,
 * we look up which PIDs that GUID corresponds to and only return matching devices.
 */
struct guid_pid_entry {
    unsigned long d1;
    unsigned short d2, d3;
    unsigned char d4[8];
    unsigned short pids[4]; /* 0-terminated */
};

static const struct guid_pid_entry guid_pid_table[] = {
    /* {489A9278-5BEA-42D2-845D-CE4EC18DC69D} → ICDUSB3.sys → ICD-PX/AX series */
    { 0x489A9278, 0x5BEA, 0x42D2, {0x84,0x5D,0xCE,0x4E,0xC1,0x8D,0xC6,0x9D},
      {0x0387, 0x03F9, 0} },
    /* {5AE62052-2B2B-4A39-A4F2-6A6A4C5C3EF0} → IcdUSB2.sys → ICD-ST series */
    { 0x5AE62052, 0x2B2B, 0x4A39, {0xA4,0xF2,0x6A,0x6A,0x4C,0x5C,0x3E,0xF0},
      {0x0103, 0x0116, 0} },
    /* {5AE62052-2B2B-4A39-A4F2-6A6A4C5C3EF1} → ICDSX.sys → ICD-SX series */
    { 0x5AE62052, 0x2B2B, 0x4A39, {0xA4,0xF2,0x6A,0x6A,0x4C,0x5C,0x3E,0xF1},
      {0x016D, 0} },
};

static const unsigned short *lookup_pids_for_guid(unsigned long p1, unsigned long p2,
                                                   unsigned long p3, unsigned long p4) {
    /* The 4 DWORDs are the raw GUID bytes read as little-endian DWORDs:
     * p1 = Data1, p2 = Data3<<16 | Data2, p3 = Data4[0..3], p4 = Data4[4..7] */
    for (int i = 0; i < sizeof(guid_pid_table)/sizeof(guid_pid_table[0]); i++) {
        const struct guid_pid_entry *e = &guid_pid_table[i];
        unsigned long e2 = (unsigned long)e->d2 | ((unsigned long)e->d3 << 16);
        unsigned long e3, e4;
        memcpy(&e3, &e->d4[0], 4);
        memcpy(&e4, &e->d4[4], 4);
        if (e->d1 == p1 && e2 == p2 && e3 == p3 && e4 == p4)
            return e->pids;
    }
    return NULL;
}

static int pid_matches(const char *hwid, const unsigned short *pids) {
    if (!pids) return 1; /* no filter — match all Sony devices */
    for (int i = 0; pids[i]; i++) {
        char needle[16];
        snprintf(needle, sizeof(needle), "PID_%04X", pids[i]);
        if (strstr(hwid, needle)) return 1;
    }
    return 0;
}

static unsigned int enumerate_devices(const unsigned short *pid_filter) {
    HDEVINFO devinfo = SetupDiGetClassDevsA(&WINUSB_CLASS_GUID, NULL, NULL, DIGCF_PRESENT);
    if (devinfo == INVALID_HANDLE_VALUE) {
        dbg("IcdUsb-WinUSB: SetupDiGetClassDevs failed err=%lu\n", GetLastError());
        return 0;
    }
    SP_DEVINFO_DATA devdata = {sizeof(SP_DEVINFO_DATA)};
    g_device_count = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devinfo, i, &devdata) && g_device_count < MAX_DEVICES; i++) {
        char hwid[256] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(devinfo, &devdata, SPDRP_HARDWAREID,
                                                NULL, (BYTE *)hwid, sizeof(hwid), NULL))
            continue;
        if (!strstr(hwid, "VID_054C"))
            continue;
        if (!pid_matches(hwid, pid_filter))
            continue;
        char *path = get_device_path(devinfo, &devdata);
        if (path) {
            lstrcpynA(g_device_paths[g_device_count], path, sizeof(g_device_paths[0]));
            g_device_count++;
            free(path);
        } else {
            dbg("IcdUsb-WinUSB: VID/PID match but no device interface\n");
        }
    }
    SetupDiDestroyDeviceInfoList(devinfo);
    dbg("IcdUsb-WinUSB: found %u device(s)\n", g_device_count);
    return g_device_count;
}

__declspec(dllexport) unsigned int __stdcall
SonyUsbEnumerateDevices(void) {
    dbg("IcdUsb-WinUSB: SonyUsbEnumerateDevices\n");
    return enumerate_devices(NULL); /* no PID filter — match all Sony devices */
}

__declspec(dllexport) unsigned int __stdcall
SonyUsbEnumerateDevicesGuid(unsigned long p1, unsigned long p2,
                            unsigned long p3, unsigned long p4) {
    dbg("IcdUsb-WinUSB: SonyUsbEnumerateDevicesGuid({%08lx-%04x-...})\n", p1, (unsigned)(p2 & 0xffff));
    const unsigned short *pids = lookup_pids_for_guid(p1, p2, p3, p4);
    if (pids)
        dbg("IcdUsb-WinUSB: GUID matched, filtering by PID\n");
    else
        dbg("IcdUsb-WinUSB: GUID not in table, matching all Sony devices\n");
    return enumerate_devices(pids);
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbGetDeviceName(int index, LPSTR name) {
    dbg("IcdUsb-WinUSB: SonyUsbGetDeviceName(%d)\n", index);
    if (index < 0 || (unsigned int)index >= g_device_count) {
        SetLastError(0x57); /* ERROR_INVALID_PARAMETER, same as original */
        return 0;
    }
    if (name) lstrcpyA(name, g_device_paths[index]);
    dbg("IcdUsb-WinUSB: SonyUsbGetDeviceName -> %s\n", g_device_paths[index]);
    return 1;
}

__declspec(dllexport) BOOL __stdcall
SonyUsbOpen(LPCSTR path, uint32_t *handle) {
    dbg("IcdUsb-WinUSB: SonyUsbOpen(%s)\n", path ? path : "NULL");
    if (!winusb_open()) {
        SetLastError(0x57); /* ERROR_INVALID_PARAMETER, same as original */
        return FALSE;
    }
    if (handle) *handle = (uint32_t)(uintptr_t)g_device;
    return TRUE;
}

__declspec(dllexport) BOOL __stdcall
SonyUsbOpenOriginal(LPCSTR path, uint32_t *handle, HANDLE event) {
    dbg("IcdUsb-WinUSB: SonyUsbOpenOriginal(%s)\n", path ? path : "NULL");
    return SonyUsbOpen(path, handle);
}

__declspec(dllexport) BOOL __stdcall
SonyUsbOpenGuid(LPCSTR path, uint32_t *handle, uint32_t g1, uint32_t g2, uint32_t g3, uint32_t g4) {
    dbg("IcdUsb-WinUSB: SonyUsbOpenGuid(%s)\n", path ? path : "NULL");
    return SonyUsbOpen(path, handle);
}

__declspec(dllexport) BOOL __stdcall
SonyUsbOpenOriginalGuid(LPCSTR path, uint32_t *handle, HANDLE event,
                         uint32_t g1, uint32_t g2, uint32_t g3, uint32_t g4) {
    dbg("IcdUsb-WinUSB: SonyUsbOpenOriginalGuid(%s)\n", path ? path : "NULL");
    return SonyUsbOpen(path, handle);
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbClose(int handle) {
    dbg("IcdUsb-WinUSB: SonyUsbClose\n");
    winusb_close();
    return 1;
}

__declspec(dllexport) BOOL __stdcall
SonyUsbControlSend(HANDLE h, uint8_t bRequest, void *data, size_t len) {
    WINUSB_SETUP_PACKET setup = {0x41, bRequest, WVALUE_MAGIC, 0, (USHORT)len};
    ULONG transferred = 0;
    BOOL ok = WinUsb_ControlTransfer(g_winusb, setup, data, (ULONG)len, &transferred, NULL);
    dbg("IcdUsb-WinUSB: ControlSend req=0x%02x len=%u -> %s\n",
        bRequest, (unsigned)len, ok ? "OK" : "FAIL");
    return ok;
}

__declspec(dllexport) BOOL __stdcall
SonyUsbControlRecive(HANDLE h, uint8_t bRequest, void *data, size_t len, DWORD *bytesRead) {
    WINUSB_SETUP_PACKET setup = {0xc1, bRequest, WVALUE_MAGIC, 0, (USHORT)len};
    ULONG transferred = 0;
    BOOL ok = WinUsb_ControlTransfer(g_winusb, setup, data, (ULONG)len, &transferred, NULL);
    if (bytesRead) *bytesRead = transferred;
    dbg("IcdUsb-WinUSB: ControlRecive req=0x%02x len=%u -> %s (%lu bytes)\n",
        bRequest, (unsigned)len, ok ? "OK" : "FAIL", transferred);
    return ok;
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbDataSend(HANDLE h, void *data, unsigned int len) {
    ULONG transferred = 0;
    BOOL ok = WinUsb_WritePipe(g_winusb, EP_BULK_OUT, data, len, &transferred, NULL);
    dbg("IcdUsb-WinUSB: DataSend len=%u -> %s\n", len, ok ? "OK" : "FAIL");
    return ok ? 1 : 0;
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbDataRecive(HANDLE h, void *data, unsigned int len, int *bytesRead) {
    ULONG transferred = 0;
    BOOL ok = WinUsb_ReadPipe(g_winusb, EP_BULK_IN, data, len, &transferred, NULL);
    if (bytesRead) *bytesRead = (int)transferred;
    dbg("IcdUsb-WinUSB: DataRecive len=%u -> %s (%lu bytes)\n",
        len, ok ? "OK" : "FAIL", transferred);
    return ok ? 1 : 0;
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbDataSendFileIO(HANDLE h, const void *data, unsigned int len) {
    return SonyUsbDataSend(h, (void *)data, len);
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbDataReciveFileIO(HANDLE h, void *data, unsigned int len, int *bytesRead) {
    return SonyUsbDataRecive(h, data, len, bytesRead);
}

__declspec(dllexport) BOOL __stdcall
SonyUsbBulkReset(HANDLE h) {
    dbg("IcdUsb-WinUSB: BulkReset\n");
    BOOL ok1 = WinUsb_ResetPipe(g_winusb, EP_BULK_IN);
    BOOL ok2 = WinUsb_ResetPipe(g_winusb, EP_BULK_OUT);
    return ok1 && ok2;
}

/*
 * The original stores 4 DWORDs (16 bytes) as the timeout structure.
 * DAT_1000c748 holds: [control_send_timeout, control_recv_timeout,
 *                      bulk_send_timeout, bulk_recv_timeout]
 * We apply the bulk timeouts to WinUSB pipe policies.
 */
static DWORD g_timeouts[4] = {10000, 10000, 10000, 10000};

__declspec(dllexport) uint32_t __stdcall
SonyUsbSetTimeOut(void *timeout) {
    if (timeout) {
        memcpy(g_timeouts, timeout, 16);
        dbg("IcdUsb-WinUSB: SetTimeOut [%lu, %lu, %lu, %lu]\n",
            g_timeouts[0], g_timeouts[1], g_timeouts[2], g_timeouts[3]);
        if (g_winusb) {
            WinUsb_SetPipePolicy(g_winusb, EP_BULK_IN, PIPE_TRANSFER_TIMEOUT,
                                 sizeof(DWORD), &g_timeouts[3]);
            WinUsb_SetPipePolicy(g_winusb, EP_BULK_OUT, PIPE_TRANSFER_TIMEOUT,
                                 sizeof(DWORD), &g_timeouts[2]);
        }
    }
    return 1;
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbGetTimeOut(void *timeout) {
    if (timeout) memcpy(timeout, g_timeouts, 16);
    return 1;
}

__declspec(dllexport) uint32_t __stdcall
SonyUsbCheckMyDevice(int p1, int p2) {
    dbg("IcdUsb-WinUSB: CheckMyDevice(%d, %d)\n", p1, p2);
    return (g_winusb != NULL) ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        dbg("IcdUsb-WinUSB: loaded\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        winusb_close();
    }
    return TRUE;
}
