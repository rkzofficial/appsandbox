/*++

Module Name:

    vdd.h

Abstract:

    AppSandbox Virtual Display Driver - single virtual monitor with direct
    HvSocket frame transport to the host (no agent middleman).

    The monitor's mode (resolution + refresh rate) is configurable: the driver
    reads HKLM\SOFTWARE\AppSandbox\Display at device add (Width, Height,
    RefreshHz, ModeList) and defaults to 1920x1080 @ 60 Hz when the key is
    absent. The host writes that key through the guest agent and restarts the
    device, so a mode change never needs a driver rebuild. Framebuffer
    readback is sent to the host over the transport with the frame's real
    width/height/stride in every header, so the host adapts to whatever mode
    the guest is running.

Environment:

    Windows User-Mode Driver Framework 2

--*/

#pragma once

#include <winsock2.h>   /* must precede windows.h */
#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>

#include "Trace.h"
#include "../transport/asb_transport.h"   /* AsbConn — host connection (AF_HYPERV on PC, ivshmem on Mac) */

/* ============================================================================
 *  Display constants
 * ============================================================================ */
#define VDD_DEFAULT_WIDTH    1920       /* used when the registry key is absent/invalid */
#define VDD_DEFAULT_HEIGHT   1080
#define VDD_DEFAULT_REFRESH  60
#define VDD_BPP              4          /* BGRA 8-bit */
#define VDD_MAX_DIRTY_RECTS  64

/* Sanity limits for a configured mode. The host viewer refuses frames larger
   than 7680x4320; refresh is bounded so hSync/pixelRate stay sane for IddCx. */
#define VDD_MIN_WIDTH        640
#define VDD_MIN_HEIGHT       480
#define VDD_MAX_WIDTH        7680
#define VDD_MAX_HEIGHT       4320
#define VDD_MIN_REFRESH      24
#define VDD_MAX_REFRESH      500
#define VDD_MAX_MODES        96         /* configured mode + built-in table (9 x 7 = 63) with headroom */

/* Registry configuration (written by the guest agent on the host's behalf):
 *   HKLM\SOFTWARE\AppSandbox\Display
 *     Width      REG_DWORD   active width in pixels
 *     Height     REG_DWORD   active height in pixels
 *     RefreshHz  REG_DWORD   vertical refresh in Hz
 *     ModeList   REG_DWORD   0 = advertise ONLY the configured mode (host has
 *                            full control; the guest cannot pick another mode)
 *                            1 = also advertise the built-in mode table so the
 *                            guest's Display Settings offers other choices
 *                            (the configured mode stays the preferred one). */
#define VDD_REG_KEY          L"SOFTWARE\\AppSandbox\\Display"
#define VDD_REG_WIDTH        L"Width"
#define VDD_REG_HEIGHT       L"Height"
#define VDD_REG_REFRESH      L"RefreshHz"
#define VDD_REG_MODELIST     L"ModeList"

typedef struct _VDD_MODE {
    UINT width;
    UINT height;
    UINT refresh;               /* Hz (integer) */
} VDD_MODE;

/* ============================================================================
 *  Wire protocol: ASFR frame header (sent over HvSocket to host)
 *  Must match FrameHeader in vm_display_idd.c
 * ============================================================================ */
#define VDD_FRAME_MAGIC     0x52465341  /* "ASFR" little-endian */

#pragma pack(push, 1)
typedef struct _VDD_WIRE_FRAME_HEADER {
    UINT32  magic;              /* VDD_FRAME_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT64  frame_seq;
    UINT32  dirty_rect_count;   /* 0 = full frame */
} VDD_WIRE_FRAME_HEADER;

/* Wire protocol: ASCR cursor update (sent over HvSocket to host)
 * Must match CursorHeader in vm_display_idd.c */
#define VDD_CURSOR_MAGIC    0x52435341  /* "ASCR" little-endian */

typedef struct _VDD_WIRE_CURSOR_HEADER {
    UINT32  magic;              /* VDD_CURSOR_MAGIC */
    INT32   x;                  /* cursor position X (can be negative) */
    INT32   y;                  /* cursor position Y (can be negative) */
    UINT32  visible;            /* 0 = hidden, 1 = visible */
    UINT32  shape_updated;      /* 0 = position only, 1 = shape data follows */
    UINT32  shape_id;           /* monotonic shape identifier */
    UINT32  width;              /* cursor width in pixels */
    UINT32  height;             /* cursor height in pixels */
    UINT32  pitch;              /* row pitch in bytes */
    UINT32  xhot;               /* hotspot X */
    UINT32  yhot;               /* hotspot Y */
    UINT32  cursor_type;        /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32  shape_data_size;    /* bytes of shape data following header (0 if !shape_updated) */
} VDD_WIRE_CURSOR_HEADER;
#pragma pack(pop)

/* ============================================================================
 *  Built-in mode table (advertised when ModeList=1, in addition to the
 *  configured mode). Any resolution x any refresh rate below.
 * ============================================================================ */
static const VDD_MODE g_BuiltinResolutions[] = {
    { 1280,  720, 0 },
    { 1600,  900, 0 },
    { 1920, 1080, 0 },
    { 1920, 1200, 0 },
    { 2560, 1080, 0 },
    { 2560, 1440, 0 },
    { 2560, 1600, 0 },
    { 3440, 1440, 0 },
    { 3840, 2160, 0 },
};
static const UINT g_BuiltinRefreshRates[] = { 60, 75, 100, 120, 144, 165, 240 };

/* ============================================================================
 *  EDID - 128-byte block
 *
 *  Manufacturer: "ASB" (AppSandBox)
 *  Descriptor:   1920x1080 @ 60 Hz, 8-bit (descriptive only: with an EDID
 *                monitor description IddCx asks the driver for the mode list
 *                via ParseMonitorDescription, which returns the configured
 *                modes, so the DTD does not constrain the active mode).
 *  Checksum byte 127 is a placeholder - patched at runtime.
 * ============================================================================ */
static const BYTE VDD_EDID[] = {
    /* Header (bytes 0-7) */
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,

    /* Manufacturer "ASB" = 0x06 0x62  (5-bit packed: A=1,S=19,B=2) */
    /* Product code 0x0001, serial 0x00000001 */
    0x06, 0x62, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,

    /* Week 1, year 2024 (offset from 1990 = 34 = 0x22) */
    0x01, 0x22,

    /* EDID version 1.4 */
    0x01, 0x04,

    /* Digital input, 8-bit color depth, DisplayPort */
    0xA5,

    /* 53cm x 30cm (approx 24" diagonal for 1920x1080) */
    0x35, 0x1E,

    /* Gamma 2.2 (value = (gamma*100)-100 = 120 = 0x78) */
    0x78,

    /* Supported features: RGB color, preferred timing in DTD1 */
    0x22,

    /* Chromaticity coordinates (standard sRGB-ish values) */
    0xFC, 0x81, 0xA4, 0x55, 0x4D, 0x9D, 0x25, 0x12, 0x50, 0x54,

    /* Established timings */
    0x00, 0x00, 0x00,

    /* Standard timings (unused, filled with 0x01 0x01) */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,

    /* Detailed Timing Descriptor #1: 1920x1080 @ 60Hz */
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
    0x58, 0x2C, 0x45, 0x00, 0x12, 0x2C, 0x21, 0x00,
    0x00, 0x1E,

    /* Descriptor #2: Monitor name "AppSandboxVDD" */
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'A', 'p', 'p', 'S', 'a', 'n', 'd', 'b', 'o', 'x', 'V', 'D', 'D',

    /* Descriptor #3: Monitor range limits: V 24-240 Hz, H 15-255 kHz,
       max pixel clock 2550 MHz (wide enough for 4K @ 240 Hz) */
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x18, 0xF0, 0x0F, 0xFF, 0xFF, 0x00, 0x0A,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,

    /* Descriptor #4: Dummy (zero) */
    0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,

    /* Extension block count */
    0x00,

    /* Checksum placeholder - computed at runtime */
    0x00
};

/* ============================================================================
 *  Swap chain processor state (heap-allocated per AssignSwapChain)
 * ============================================================================ */
typedef struct _VDD_SWAP_PROC {
    IDDCX_SWAPCHAIN     hSwapChain;
    HANDLE              hAvailableEvent;    /* from pInArgs->hNextSurfaceAvailable */
    HANDLE              hTerminateEvent;
    HANDLE              hThread;

    /* Non-owning pointers to cached D3D11 device (owned by VDD_DEVICE_CONTEXT).
       The device is cached across swap chain transitions to avoid destroying it
       during IddCx teardown to avoid re-creation overhead. */
    ID3D11Device*       pDevice;
    ID3D11DeviceContext* pDeviceContext;

    /* Staging texture for CPU readback (owned, per-swap-chain). Created from the
       first acquired surface's size and recreated whenever the surface size
       changes (a guest mode switch hands us a new swap chain of a new size). */
    ID3D11Texture2D*    pStagingTex;
    UINT                width;              /* current frame size (pixels) */
    UINT                height;
    UINT                stride;             /* bytes per row on the wire (width * 4) */

    /* Host connection via asb_transport — AF_HYPERV HvSocket on a Windows host, ivshmem stream
       slot on a macOS host. One emit path, two backends. */
    HANDLE              hNetworkThread;     /* listener/accept thread */
    AsbConn * volatile  hClientConn;        /* active client, owned by swap chain thread */
    AsbConn * volatile  hPendingConn;       /* new client from network thread, picked up by swap chain */
    volatile BOOL       bStopNetwork;
    UINT64              frameSeq;

    /* Hardware cursor */
    IDDCX_MONITOR       hMonitor;           /* needed for QueryHardwareCursor */
    HANDLE              hCursorEvent;       /* signaled when cursor data changes */
    PBYTE               pCursorShapeBuffer; /* buffer for cursor bitmap */
    UINT                cursorBufSize;      /* size of pCursorShapeBuffer (2x for MASKED_COLOR) */
    DWORD               lastShapeId;        /* last received shape ID */
} VDD_SWAP_PROC;

/* ============================================================================
 *  Device context (heap-allocated, stored via IndirectDeviceContextWrapper)
 * ============================================================================ */
typedef struct _VDD_DEVICE_CONTEXT {
    WDFDEVICE           wdfDevice;
    IDDCX_ADAPTER       hAdapter;
    IDDCX_MONITOR       hMonitor;
    VDD_SWAP_PROC*      pSwapProc;          /* Active swap chain processor */

    /* Cached D3D11 device — persists across swap chain transitions.
       Kept alive here so that WdfObjectDelete's IddCx teardown never triggers
       destruction of the D3D device (which crashes WUDFHost.exe). */
    ID3D11Device*       pCachedDevice;
    ID3D11DeviceContext* pCachedCtx;
    LUID                cachedDeviceLuid;

    /* Monitor mode list: the configured mode (index preferredMode) and, when
       ModeList=1, the built-in table. Read once at device add. */
    VDD_MODE            modes[VDD_MAX_MODES];
    UINT                modeCount;
    UINT                preferredMode;

    /* Recovery: if no AssignSwapChain arrives within 5s of Unassign,
       depart and re-arrive the monitor to force DWM re-engagement.
       Uses a WDF timer so the callback runs on a proper WDF thread
       (IddCx APIs crash when called from raw CreateThread threads). */
    WDFTIMER            hRecoveryTimer;
} VDD_DEVICE_CONTEXT;

/* ============================================================================
 *  WDF Context Wrappers
 * ============================================================================ */

struct IndirectDeviceContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

struct MonitorContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

struct RecoveryTimerContext
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(RecoveryTimerContext);

/* ============================================================================
 *  IddCx Callback declarations
 * ============================================================================ */

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD           VddDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY            VddDeviceD0Entry;
EVT_WDF_DRIVER_UNLOAD              VddDriverUnload;

EVT_IDD_CX_ADAPTER_INIT_FINISHED                   VddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES                    VddAdapterCommitModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2                   VddAdapterCommitModes2;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION               VddParseMonitorDescription;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2              VddParseMonitorDescription2;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES   VddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES              VddMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2             VddMonitorQueryTargetModes2;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN                VddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN              VddMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO               VddAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA        VddMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP                  VddMonitorSetGammaRamp;
