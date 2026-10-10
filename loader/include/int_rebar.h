#ifndef int_rebar_h
#define int_rebar_h
#include "int_vbt.h"

// ReBAR diagnostic (read-only): what would it take to give the Radeon a Resizable BAR?
//   - PCIe extended capability 0x15 (Resizable BAR) of the Radeon: supported / current sizes
//   - the memory / prefetchable windows of the bridges between the root port and the Radeon
//   - M64B / M64L (the root bridge 64-bit window) in the firmware NVS, found through SaSsdt
// Nothing is written to PCI or ACPI; the report goes to \rebar_diag.txt on the ESP.

// ---- pure helpers (no EFI services, host-testable; lib/int_rebarcaps.c) ----

// Dword read at a PCI config offset (0..0xFFC, extended space above 0xFF).
typedef EFI_STATUS (*_INT_CfgRd)(VOID* Ctx, UINT32 Off, UINT32* V);

#define REBAR_MAX_CAPS 24
#define REBAR_MAX_BARS 6

typedef struct {
    BOOLEAN ExtRead;                        // dword at 0x100 could be read
    UINT32  ExtHdr;                         // that dword
    UINTN   NCaps;                          // extended capabilities seen
    UINT16  CapId[REBAR_MAX_CAPS];
    UINT16  CapOff[REBAR_MAX_CAPS];
    BOOLEAN HasRebar;
    UINT16  RebarOff;
    UINTN   NBars;                          // Resizable BAR entries
    UINT8   BarIdx[REBAR_MAX_BARS];
    UINT32  BarCap[REBAR_MAX_BARS];         // capability register: bits 4..23 = 1 MB .. 512 GB
    UINT32  BarCtl[REBAR_MAX_BARS];         // control register: bits 8..12 = current size (2^(20+n))
} _INT_RebarInfo;

EFI_STATUS _INT_RebarScan(_INT_CfgRd Rd, VOID* Ctx, _INT_RebarInfo* I);
UINT8      _INT_RebarMaxExp(UINT32 CapReg);     // log2 of the largest supported size (0 = none)
UINT8      _INT_RebarCurExp(UINT32 CtlReg);     // log2 of the current size
VOID       _INT_RebarReport(const _INT_RebarInfo* I, _INT_Rep* R);

typedef struct {
    UINT8   Pri, Sec, Sub;
    UINT32  MemBase, MemLimit;              // non-prefetchable window, limit inclusive
    BOOLEAN MemEmpty;
    BOOLEAN PrefBits64;                     // prefetchable window registers are 64-bit capable
    UINT64  PrefBase, PrefLimit;            // limit inclusive
    BOOLEAN PrefEmpty;
} _INT_BridgeWin;

EFI_STATUS _INT_BridgeDecode(_INT_CfgRd Rd, VOID* Ctx, _INT_BridgeWin* W);
VOID       _INT_BridgeReport(const _INT_BridgeWin* W, _INT_Rep* R);

typedef struct {
    BOOLEAN Found;                          // M64B and M64L located in a Field of an OperationRegion
    UINT64  RegionAddr;
    UINT32  RegionLen;
    UINT32  OffM64B, OffM64L;               // byte offsets in the region
    BOOLEAN HasM32;
    UINT32  OffM32B, OffM32L;
    CHAR8   Region[5];
} _INT_NvsInfo;

BOOLEAN _INT_NvsParse(const UINT8* T, UINT32 Len, _INT_NvsInfo* Out);

// ---- EFI glue (lib/int_rebar.c) ----
typedef struct {
    BOOLEAN GpuFound;
    BOOLEAN ExtRead;
    BOOLEAN HasRebar;
    UINT8   MaxExp, CurExp;                 // log2 sizes of the Radeon BAR0 entry (0 = unknown)
    BOOLEAN NvsOk;
    UINT64  M64B, M64L;
} _INT_RebarSummary;

// Collects the report in R and writes it to FileName on the ESP. Returns the file write status.
EFI_STATUS _INT_RebarDiag(EFI_BOOT_SERVICES* BS, EFI_SYSTEM_TABLE* ST, EFI_HANDLE ImageHandle,
                          CHAR16* FileName, _INT_Rep* R, _INT_RebarSummary* Sum);

#endif
