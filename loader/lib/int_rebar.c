#include "../include/int_rebar.h"
#include "../include/int_mem.h"
#include <efiprot.h>

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

#define MAX_DEVS 64
#define ACPI_HDR_SIZE 36

typedef struct {
    EFI_PCI_IO_PROTOCOL* Pci;
    UINT8   Bus, Dev, Fn;
    UINT32  Id, Class, Hdr;
} PciDev;

typedef struct {
    EFI_PCI_IO_PROTOCOL* Pci;
    UINT64  Mcfg;               // ECAM base of segment 0 (0 = no MCFG)
    UINT8   StartBus;
    UINT8   Bus, Dev, Fn;
    BOOLEAN UsedMmio;           // some read went through ECAM because PciIo refused it
    EFI_STATUS PciSt;           // status of the first refused PciIo read
} RdCtx;

// Field by field: an initializer list on a struct with padding makes the compiler call memset,
// which this freestanding build does not link.
static VOID CtxInit(RdCtx* X, const PciDev* D, UINT64 Mcfg, UINT8 StartBus)
{
    X->Pci = D->Pci; X->Mcfg = Mcfg; X->StartBus = StartBus;
    X->Bus = D->Bus; X->Dev = D->Dev; X->Fn = D->Fn;
    X->UsedMmio = FALSE; X->PciSt = EFI_SUCCESS;
}

// Dword at a config offset. PciIo first; if it refuses (Apple's PciIo may stop at 0xFF) and an MCFG
// is known, a read-only ECAM access. Reads only.
static EFI_STATUS CfgRd(VOID* C, UINT32 Off, UINT32* V)
{
    RdCtx* X = (RdCtx*)C;
    EFI_STATUS St = X->Pci->Pci.Read(X->Pci, EfiPciIoWidthUint32, Off, 1, V);
    if (!EFI_ERROR(St)) return St;
    if (!X->UsedMmio) X->PciSt = St;
    if (X->Mcfg == 0 || X->Bus < X->StartBus || Off >= 0x1000) return St;
    UINT64 A = X->Mcfg + ((UINT64)(X->Bus - X->StartBus) << 20) + ((UINT64)X->Dev << 15) +
               ((UINT64)X->Fn << 12) + Off;
    *V = *(volatile UINT32*)(UINTN)A;
    X->UsedMmio = TRUE;
    return EFI_SUCCESS;
}

static PciDev gDevs[MAX_DEVS];

static VOID RepBdf(_INT_Rep* R, const PciDev* D)
{
    HX(D->Bus, 2); S(":"); HX(D->Dev, 2); S("."); HX(D->Fn, 1);
}

static BOOLEAN GuidIsAcpi20(const EFI_GUID* g)
{
    static const UINT8 want[16] = { 0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
                                    0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 };
    const UINT8* x = (const UINT8*)g;
    for (UINTN i = 0; i < 16; i++) if (x[i] != want[i]) return FALSE;
    return TRUE;
}

static BOOLEAN Sig4(const UINT8* p, const char* s)
{
    return p[0] == (UINT8)s[0] && p[1] == (UINT8)s[1] && p[2] == (UINT8)s[2] && p[3] == (UINT8)s[3];
}

EFI_STATUS _INT_RebarDiag(EFI_BOOT_SERVICES* BS, EFI_SYSTEM_TABLE* ST, EFI_HANDLE ImageHandle,
                          CHAR16* FileName, _INT_Rep* R, _INT_RebarSummary* Sum)
{
    EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
    UINT8* Sa = NULL;
    UINT8* Mcfg = NULL;
    UINT64 EcamBase = 0;
    UINT8  EcamStart = 0, EcamEnd = 0;

    for (UINTN i = 0; i < sizeof(*Sum); i++) ((UINT8*)Sum)[i] = 0;

    S("t2gmux ReBAR diagnostic (read-only, nothing is written to PCI or ACPI)"); NL();

    // ---- ACPI: XSDT -> SaSsdt (NVS window M64B/M64L) and MCFG (ECAM) ----
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (!GuidIsAcpi20(&ST->ConfigurationTable[i].VendorGuid)) continue;
        const UINT8* Rsdp = (const UINT8*)ST->ConfigurationTable[i].VendorTable;
        if (!Rsdp || Rsdp[15] < 2) break;
        const UINT8* Xsdt = (const UINT8*)(UINTN)_INT_Rd64(Rsdp + 24);
        if (!Xsdt || !Sig4(Xsdt, "XSDT")) break;
        UINTN N = (_INT_Rd32(Xsdt + 4) - ACPI_HDR_SIZE) / 8;
        for (UINTN k = 0; k < N; k++) {
            UINT8* T = (UINT8*)(UINTN)_INT_Rd64(Xsdt + ACPI_HDR_SIZE + k * 8);
            if (!T) continue;
            if (Sig4(T, "MCFG")) Mcfg = T;
            if (Sig4(T, "SSDT") && Sig4(T + 16, "SaSs") && T[20] == 'd' && T[21] == 't') Sa = T;
        }
        break;
    }
    if (Mcfg && _INT_Rd32(Mcfg + 4) >= 44 + 16 && _INT_Rd16(Mcfg + 44 + 8) == 0) {
        EcamBase = _INT_Rd64(Mcfg + 44);
        EcamStart = Mcfg[44 + 10];
        EcamEnd = Mcfg[44 + 11];
        S("MCFG: ECAM base "); HX(EcamBase, 8); S(", buses "); HX(EcamStart, 2); S("-"); HX(EcamEnd, 2); NL();
    } else {
        S("MCFG: none usable (no ECAM fallback)"); NL();
    }

    // ---- PCI devices ----
    EFI_HANDLE* Buf = NULL;
    UINTN Cnt = 0, Nd = 0;
    EFI_STATUS Status = BS->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &Cnt, &Buf);
    if (EFI_ERROR(Status)) {
        S("PciIo handles: error "); HX(Status, 8); NL();
    } else {
        for (UINTN i = 0; i < Cnt && Nd < MAX_DEVS; i++) {
            EFI_PCI_IO_PROTOCOL* Pci = NULL;
            UINTN Seg = 0, Bus = 0, Dev = 0, Fn = 0;
            UINT32 Id = 0, Cl = 0, Hd = 0;
            if (EFI_ERROR(BS->OpenProtocol(Buf[i], &pci_guid, (VOID**)&Pci, ImageHandle, NULL,
                                           EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
            if (EFI_ERROR(Pci->GetLocation(Pci, &Seg, &Bus, &Dev, &Fn)) || Seg != 0) continue;
            if (EFI_ERROR(Pci->Pci.Read(Pci, EfiPciIoWidthUint32, 0x00, 1, &Id)) || (Id & 0xFFFF) == 0xFFFF) continue;
            Pci->Pci.Read(Pci, EfiPciIoWidthUint32, 0x08, 1, &Cl);
            Pci->Pci.Read(Pci, EfiPciIoWidthUint32, 0x0C, 1, &Hd);
            gDevs[Nd].Pci = Pci; gDevs[Nd].Bus = (UINT8)Bus; gDevs[Nd].Dev = (UINT8)Dev; gDevs[Nd].Fn = (UINT8)Fn;
            gDevs[Nd].Id = Id; gDevs[Nd].Class = Cl; gDevs[Nd].Hdr = Hd;
            Nd++;
        }
        BS->FreePool(Buf);
    }
    S("PCI devices seen: "); DC(Nd); NL();

    // ---- every AMD display device ----
    UINT8 GpuBus[4]; UINTN Ng = 0;
    for (UINTN i = 0; i < Nd; i++) {
        PciDev* D = &gDevs[i];
        if ((D->Id & 0xFFFF) != 0x1002 || (D->Class >> 24) != 0x03) continue;
        RdCtx X;
        CtxInit(&X, D, EcamBase, EcamStart);
        _INT_RebarInfo Info;
        UINT32 Cmd = 0, Bar[6], Sub = 0;
        D->Pci->Pci.Read(D->Pci, EfiPciIoWidthUint32, 0x04, 1, &Cmd);
        D->Pci->Pci.Read(D->Pci, EfiPciIoWidthUint32, 0x2C, 1, &Sub);
        for (UINTN b = 0; b < 6; b++) { Bar[b] = 0; D->Pci->Pci.Read(D->Pci, EfiPciIoWidthUint32, (UINT32)(0x10 + 4 * b), 1, &Bar[b]); }

        NL(); S("[AMD display device] "); RepBdf(R, D); S("  "); HX(D->Id & 0xFFFF, 4); S(":"); HX(D->Id >> 16, 4);
        S("  subsys "); HX(Sub, 8); S("  class "); HX(D->Class >> 8, 6); NL();
        S("    command/status "); HX(Cmd, 8); NL();
        S("    BAR0-5 raw:"); for (UINTN b = 0; b < 6; b++) { S(" "); HX(Bar[b], 8); } NL();

        _INT_RebarScan(CfgRd, &X, &Info);
        S("    extended config via "); S(X.UsedMmio ? "MCFG/ECAM (PciIo refused: status " : "PciIo");
        if (X.UsedMmio) { HX(X.PciSt, 8); S(")"); }
        NL();
        _INT_RebarReport(&Info, R);

        if (Ng < 4) GpuBus[Ng++] = D->Bus;
        if (!Sum->GpuFound) {
            Sum->GpuFound = TRUE;
            Sum->ExtRead = Info.ExtRead;
            Sum->HasRebar = Info.HasRebar;
            if (Info.HasRebar && Info.NBars > 0) {
                Sum->MaxExp = _INT_RebarMaxExp(Info.BarCap[0]);
                Sum->CurExp = _INT_RebarCurExp(Info.BarCtl[0]);
            }
        }
    }
    if (Ng == 0) { NL(); S("[AMD display device] not found"); NL(); }

    // ---- bridges whose secondary..subordinate range contains an AMD display bus ----
    if (Ng) { NL(); S("[Bridges on the path]"); NL(); }
    for (UINTN i = 0; Ng && i < Nd; i++) {
        PciDev* D = &gDevs[i];
        if (((D->Hdr >> 16) & 0x7F) != 1) continue;
        RdCtx X;
        CtxInit(&X, D, EcamBase, EcamStart);
        _INT_BridgeWin W;
        if (EFI_ERROR(_INT_BridgeDecode(CfgRd, &X, &W))) continue;
        BOOLEAN On = FALSE;
        for (UINTN g = 0; g < Ng; g++) if (W.Sec <= GpuBus[g] && GpuBus[g] <= W.Sub) On = TRUE;
        if (!On) continue;
        S("  "); RepBdf(R, D); S("  "); HX(D->Id & 0xFFFF, 4); S(":"); HX(D->Id >> 16, 4); NL();
        _INT_BridgeReport(&W, R);
    }

    // ---- firmware NVS: is a 64-bit root window offered? ----
    NL(); S("[Root bridge 64-bit window in firmware NVS]"); NL();
    _INT_NvsInfo Nv;
    if (!Sa) {
        S("  SaSsdt not found in XSDT"); NL();
    } else if (!_INT_NvsParse(Sa, _INT_Rd32(Sa + 4), &Nv)) {
        S("  M64B / M64L not found in a Field of SaSsdt"); NL();
    } else if (Nv.RegionAddr == 0 || (UINT64)Nv.OffM64B + 8 > Nv.RegionLen || (UINT64)Nv.OffM64L + 8 > Nv.RegionLen) {
        S("  OperationRegion "); S((const char*)Nv.Region); S(" does not cover M64B/M64L"); NL();
    } else {
        const UINT8* Base = (const UINT8*)(UINTN)Nv.RegionAddr;
        Sum->NvsOk = TRUE;
        Sum->M64B = _INT_Rd64(Base + Nv.OffM64B);
        Sum->M64L = _INT_Rd64(Base + Nv.OffM64L);
        S("  OperationRegion "); S((const char*)Nv.Region); S(" at "); HX(Nv.RegionAddr, 8); S(" length "); HX(Nv.RegionLen, 4); NL();
        S("  M64B (+"); DC(Nv.OffM64B); S(") = "); HX(Sum->M64B, 16); NL();
        S("  M64L (+"); DC(Nv.OffM64L); S(") = "); HX(Sum->M64L, 16);
        S(Sum->M64L == 0 ? "   -> the firmware offers NO 64-bit window" : "   -> a 64-bit window is offered"); NL();
        if (Nv.HasM32 && (UINT64)Nv.OffM32B + 4 <= Nv.RegionLen && (UINT64)Nv.OffM32L + 4 <= Nv.RegionLen) {
            S("  M32B = "); HX(_INT_Rd32(Base + Nv.OffM32B), 8); S("  M32L = "); HX(_INT_Rd32(Base + Nv.OffM32L), 8); NL();
        }
    }

    NL(); S("end of report"); NL();
    return _INT_WriteEspFile(BS, ImageHandle, FileName, R->buf, R->len);
}
