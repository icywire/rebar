#include "../include/int_rebar.h"

#define S(x)  _INT_RepStr(R, (const CHAR8*)(x))
#define NL()  _INT_RepNl(R)
#define HX(v, d) _INT_RepHex(R, (v), (d))
#define DC(v) _INT_RepDec(R, (v))

// ---------------------------------------------------------------------------------------------
// Resizable BAR capability (PCIe extended capability 0x15)
// ---------------------------------------------------------------------------------------------

UINT8 _INT_RebarMaxExp(UINT32 CapReg)
{
    // capability register: bit n (4..23) = size 2^(n+16) bytes (bit 4 = 1 MB)
    for (UINT32 n = 23; n >= 4; n--)
        if (CapReg & (1u << n)) return (UINT8)(n + 16);
    return 0;
}

UINT8 _INT_RebarCurExp(UINT32 CtlReg)
{
    return (UINT8)(20 + ((CtlReg >> 8) & 0x1F));
}

EFI_STATUS _INT_RebarScan(_INT_CfgRd Rd, VOID* Ctx, _INT_RebarInfo* I)
{
    UINT8* p = (UINT8*)I;
    for (UINTN i = 0; i < sizeof(*I); i++) p[i] = 0;

    UINT32 Off = 0x100;
    UINT32 V = 0;
    if (EFI_ERROR(Rd(Ctx, Off, &V)))
        return EFI_UNSUPPORTED;
    I->ExtRead = TRUE;
    I->ExtHdr = V;

    // no extended capabilities (0 / all ones) is a valid answer
    for (UINTN guard = 0; guard < 64; guard++) {
        if (V == 0 || V == 0xFFFFFFFF) break;
        UINT16 Id = (UINT16)(V & 0xFFFF);
        if (I->NCaps < REBAR_MAX_CAPS) {
            I->CapId[I->NCaps] = Id;
            I->CapOff[I->NCaps] = (UINT16)Off;
            I->NCaps++;
        }
        if (Id == 0x0015 && !I->HasRebar) {
            UINT32 Ctl0 = 0;
            if (EFI_ERROR(Rd(Ctx, Off + 8, &Ctl0))) break;
            UINTN N = (Ctl0 >> 5) & 7;                  // number of BAR entries (first control register)
            if (N > REBAR_MAX_BARS) N = REBAR_MAX_BARS;
            I->HasRebar = TRUE;
            I->RebarOff = (UINT16)Off;
            for (UINTN k = 0; k < N; k++) {
                UINT32 Cap = 0, Ctl = 0;
                if (EFI_ERROR(Rd(Ctx, (UINT32)(Off + 4 + 8 * k), &Cap)) ||
                    EFI_ERROR(Rd(Ctx, (UINT32)(Off + 8 + 8 * k), &Ctl))) break;
                I->BarIdx[k] = (UINT8)(Ctl & 7);
                I->BarCap[k] = Cap;
                I->BarCtl[k] = Ctl;
                I->NBars = k + 1;
            }
        }
        UINT32 Next = (V >> 20) & 0xFFF;
        if (Next < 0x100 || (Next & 3) || Next <= Off) break;   // end, misaligned or looping
        Off = Next;
        if (EFI_ERROR(Rd(Ctx, Off, &V))) break;
    }
    return EFI_SUCCESS;
}

static VOID RepSize(_INT_Rep* R, UINT8 Exp)
{
    if (Exp >= 30) { DC((UINT64)1 << (Exp - 30)); S("G"); }
    else           { DC((UINT64)1 << (Exp - 20)); S("M"); }
}

VOID _INT_RebarReport(const _INT_RebarInfo* I, _INT_Rep* R)
{
    if (!I->ExtRead) {
        S("    extended config space: NOT readable (offset 0x100)"); NL();
        return;
    }
    S("    extended config space: readable, first header "); HX(I->ExtHdr, 8); NL();
    S("    extended capabilities:");
    if (I->NCaps == 0) S(" none");
    for (UINTN i = 0; i < I->NCaps; i++) {
        S(" "); HX(I->CapId[i], 4); S("@"); HX(I->CapOff[i], 3);
    }
    NL();
    if (!I->HasRebar) {
        S("    Resizable BAR capability (0015): NOT present"); NL();
        return;
    }
    S("    Resizable BAR capability (0015) at "); HX(I->RebarOff, 3); S(", entries "); DC(I->NBars); NL();
    for (UINTN k = 0; k < I->NBars; k++) {
        UINT8 Mx = _INT_RebarMaxExp(I->BarCap[k]);
        S("      BAR"); DC(I->BarIdx[k]); S(": cap "); HX(I->BarCap[k], 8); S(" ctl "); HX(I->BarCtl[k], 8);
        S(", current "); RepSize(R, _INT_RebarCurExp(I->BarCtl[k]));
        S(", max "); if (Mx) RepSize(R, Mx); else S("?");
        S(", sizes:");
        for (UINT32 n = 4; n <= 23; n++)
            if (I->BarCap[k] & (1u << n)) { S(" "); RepSize(R, (UINT8)(n + 16)); }
        NL();
    }
}

// ---------------------------------------------------------------------------------------------
// Bridge windows (type 1 header)
// ---------------------------------------------------------------------------------------------

EFI_STATUS _INT_BridgeDecode(_INT_CfgRd Rd, VOID* Ctx, _INT_BridgeWin* W)
{
    UINT32 Bus = 0, Mem = 0, Pref = 0, PrefHi = 0, PrefLo = 0;
    UINT8* p = (UINT8*)W;
    for (UINTN i = 0; i < sizeof(*W); i++) p[i] = 0;
    if (EFI_ERROR(Rd(Ctx, 0x18, &Bus)) || EFI_ERROR(Rd(Ctx, 0x20, &Mem)) ||
        EFI_ERROR(Rd(Ctx, 0x24, &Pref)) || EFI_ERROR(Rd(Ctx, 0x28, &PrefHi)) ||
        EFI_ERROR(Rd(Ctx, 0x2C, &PrefLo)))
        return EFI_DEVICE_ERROR;

    W->Pri = (UINT8)Bus; W->Sec = (UINT8)(Bus >> 8); W->Sub = (UINT8)(Bus >> 16);
    W->MemBase  = (Mem & 0xFFF0u) << 16;
    W->MemLimit = (((Mem >> 16) & 0xFFF0u) << 16) | 0xFFFFFu;
    W->MemEmpty = (W->MemBase > W->MemLimit);

    W->PrefBits64 = ((Pref & 0xF) == 1) && (((Pref >> 16) & 0xF) == 1);
    W->PrefBase  = (UINT64)(Pref & 0xFFF0u) << 16;
    W->PrefLimit = ((UINT64)((Pref >> 16) & 0xFFF0u) << 16) | 0xFFFFFu;
    if (W->PrefBits64) {
        W->PrefBase  |= (UINT64)PrefHi << 32;
        W->PrefLimit |= (UINT64)PrefLo << 32;
    }
    W->PrefEmpty = (W->PrefBase > W->PrefLimit);
    return EFI_SUCCESS;
}

VOID _INT_BridgeReport(const _INT_BridgeWin* W, _INT_Rep* R)
{
    S("    buses pri/sec/sub "); HX(W->Pri, 2); S("/"); HX(W->Sec, 2); S("/"); HX(W->Sub, 2); NL();
    S("    memory window       "); if (W->MemEmpty) S("empty"); else { HX(W->MemBase, 8); S(" - "); HX(W->MemLimit, 8); } NL();
    S("    prefetchable window "); if (W->PrefEmpty) S("empty"); else { HX(W->PrefBase, 16); S(" - "); HX(W->PrefLimit, 16); }
    S(W->PrefBits64 ? "  (64-bit capable)" : "  (32-bit only)"); NL();
}

// ---------------------------------------------------------------------------------------------
// M64B / M64L in the firmware NVS: the Field list of the OperationRegion in SaSsdt
// ---------------------------------------------------------------------------------------------

// AML PkgLength at P (End = limit). *Val = the encoded value, *Used = bytes of the encoding.
static BOOLEAN PkgLen(const UINT8* P, const UINT8* End, UINT32* Val, UINTN* Used)
{
    if (P >= End) return FALSE;
    UINTN Cnt = P[0] >> 6;
    if (P + 1 + Cnt > End) return FALSE;
    if (Cnt == 0) *Val = P[0] & 0x3F;
    else {
        UINT32 V = P[0] & 0x0F;
        for (UINTN i = 0; i < Cnt; i++) V |= (UINT32)P[1 + i] << (4 + 8 * i);
        *Val = V;
    }
    *Used = 1 + Cnt;
    return TRUE;
}

static BOOLEAN NameEq(const UINT8* P, const char* N)
{
    return P[0] == (UINT8)N[0] && P[1] == (UINT8)N[1] && P[2] == (UINT8)N[2] && P[3] == (UINT8)N[3];
}

// AML integer (Zero / One / Ones / Byte / Word / DWord / QWord prefix). Returns bytes used or 0.
static UINTN AmlInt(const UINT8* P, const UINT8* End, UINT64* V)
{
    if (P >= End) return 0;
    switch (P[0]) {
    case 0x00: *V = 0; return 1;
    case 0x01: *V = 1; return 1;
    case 0xFF: *V = ~(UINT64)0; return 1;
    case 0x0A: if (P + 2 > End) return 0; *V = P[1]; return 2;
    case 0x0B: if (P + 3 > End) return 0; *V = P[1] | ((UINT64)P[2] << 8); return 3;
    case 0x0C: if (P + 5 > End) return 0; *V = _INT_Rd32(P + 1); return 5;
    case 0x0E: if (P + 9 > End) return 0; *V = _INT_Rd64(P + 1); return 9;
    }
    return 0;
}

BOOLEAN _INT_NvsParse(const UINT8* T, UINT32 Len, _INT_NvsInfo* Out)
{
    UINT8* o = (UINT8*)Out;
    for (UINTN i = 0; i < sizeof(*Out); i++) o[i] = 0;
    if (Len < 36 + 8) return FALSE;
    const UINT8* End = T + Len;

    for (const UINT8* P = T + 36; P + 8 < End; P++) {
        if (P[0] != 0x5B || P[1] != 0x81) continue;               // FieldOp
        UINT32 PL = 0; UINTN U = 0;
        if (!PkgLen(P + 2, End, &PL, &U)) continue;
        const UINT8* FEnd = P + 2 + PL;
        if (PL < U + 5 || FEnd > End) continue;
        const UINT8* F = P + 2 + U;
        const UINT8 Region[4] = { F[0], F[1], F[2], F[3] };
        F += 5;                                                   // region name + FieldFlags

        UINT64 Bit = 0;
        BOOLEAN B = FALSE, L = FALSE;
        Out->HasM32 = FALSE;
        UINT32 M32B = 0, M32L = 0; BOOLEAN B32 = FALSE, L32 = FALSE;
        BOOLEAN Ok = TRUE;
        while (F < FEnd && Ok) {
            UINT32 W = 0; UINTN WU = 0;
            if (F[0] == 0x00) {                                   // ReservedField
                if (!PkgLen(F + 1, FEnd, &W, &WU)) { Ok = FALSE; break; }
                Bit += W; F += 1 + WU;
            } else if (F[0] == 0x01) {                            // AccessField
                F += 3;
            } else if (F[0] == 0x03) {                            // ExtendedAccessField
                F += 4;
            } else if (F[0] == 0x02) {                            // ConnectField: not handled
                Ok = FALSE;
            } else {                                              // NamedField
                if (F + 5 > FEnd) { Ok = FALSE; break; }
                if (!PkgLen(F + 4, FEnd, &W, &WU)) { Ok = FALSE; break; }
                if ((Bit & 7) == 0) {
                    if (NameEq(F, "M64B")) { Out->OffM64B = (UINT32)(Bit / 8); B = TRUE; }
                    if (NameEq(F, "M64L")) { Out->OffM64L = (UINT32)(Bit / 8); L = TRUE; }
                    if (NameEq(F, "M32B")) { M32B = (UINT32)(Bit / 8); B32 = TRUE; }
                    if (NameEq(F, "M32L")) { M32L = (UINT32)(Bit / 8); L32 = TRUE; }
                }
                Bit += W; F += 4 + WU;
            }
        }
        if (!Ok || !B || !L) continue;

        // the OperationRegion of that name: 5B 80 <name> <space> <offset> <length>
        for (const UINT8* Q = T + 36; Q + 8 < End; Q++) {
            if (Q[0] != 0x5B || Q[1] != 0x80 || !NameEq(Q + 2, (const char*)Region)) continue;
            if (Q[6] != 0x00) continue;                           // SystemMemory only
            UINT64 A = 0, Ln = 0;
            UINTN a = AmlInt(Q + 7, End, &A);
            if (!a) continue;
            UINTN l = AmlInt(Q + 7 + a, End, &Ln);
            if (!l) continue;
            Out->RegionAddr = A;
            Out->RegionLen = (UINT32)Ln;
            Out->Found = TRUE;
            Out->HasM32 = (B32 && L32);
            Out->OffM32B = M32B; Out->OffM32L = M32L;
            for (UINTN i = 0; i < 4; i++) Out->Region[i] = (CHAR8)Region[i];
            Out->Region[4] = 0;
            return TRUE;
        }
    }
    return FALSE;
}
