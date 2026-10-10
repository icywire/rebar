// Host test for lib/int_rebarcaps.c: Resizable BAR capability walk, bridge window decode, NVS M64B/M64L finder.
// Optional argv[1] = ssdt5.dat (SaSsdt of a MacBookPro16,x dump): expects region SANV at 0x7AF3DC98, M64B +218, M64L +226.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/int_rebar.h"

UINT32 _INT_Rd32(const UINT8* p){UINT32 v;memcpy(&v,p,4);return v;}
UINT64 _INT_Rd64(const UINT8* p){UINT64 v;memcpy(&v,p,8);return v;}
static char out[16384]; static size_t on;
static void put(const char* s){ size_t n=strlen(s); if(on+n<sizeof out){memcpy(out+on,s,n);on+=n;out[on]=0;} }
VOID _INT_RepStr(_INT_Rep* r,const CHAR8* s){(void)r;put((const char*)s);}
VOID _INT_RepNl(_INT_Rep* r){(void)r;put("\n");}
VOID _INT_RepHex(_INT_Rep* r,UINT64 v,UINTN d){(void)r;char b[32];snprintf(b,sizeof b,"%0*llX",(int)d,(unsigned long long)v);put(b);}
VOID _INT_RepDec(_INT_Rep* r,UINT64 v){(void)r;char b[32];snprintf(b,sizeof b,"%llu",(unsigned long long)v);put(b);}

static int fails;
#define CHECK(c,msg) do{ if(!(c)){printf("FAIL: %s (line %d)\n",msg,__LINE__);fails++;} }while(0)
#define HAS(s) (strstr(out,s)!=NULL)

typedef struct { UINT32 cfg[1024]; int ext_ok; } Cfg;
static EFI_STATUS rd(VOID* c, UINT32 off, UINT32* v){
    Cfg* x=c; if(off>=4096||(off&3)) return EFI_DEVICE_ERROR;
    if(off>=0x100 && !x->ext_ok) return EFI_UNSUPPORTED;
    *v=x->cfg[off/4]; return EFI_SUCCESS;
}
static UINT32 hdr(UINT32 id,UINT32 ver,UINT32 next){return id|(ver<<16)|(next<<20);}

int main(int argc,char**argv){
    _INT_Rep* R=NULL; _INT_RebarInfo I; static Cfg c;

    // ---- chain: AER@100 -> ReBAR@150 -> 0019@200 ----
    memset(&c,0,sizeof c); c.ext_ok=1;
    c.cfg[0x100/4]=hdr(0x0001,2,0x150);
    c.cfg[0x150/4]=hdr(0x0015,1,0x200);
    UINT32 cap=0; for(UINT32 n=12;n<=17;n++) cap|=1u<<n;              // 256 MB .. 8 GB
    c.cfg[0x154/4]=cap;
    c.cfg[0x158/4]=(1u<<5)|(8u<<8);                                    // 1 entry, BAR0, current 2^(20+8)=256 MB
    c.cfg[0x200/4]=hdr(0x0019,1,0);
    CHECK(_INT_RebarScan(rd,&c,&I)==EFI_SUCCESS,"scan ok");
    CHECK(I.ExtRead && I.NCaps==3,"3 extended caps");
    CHECK(I.HasRebar && I.RebarOff==0x150 && I.NBars==1 && I.BarIdx[0]==0,"ReBAR entry found");
    CHECK(_INT_RebarMaxExp(I.BarCap[0])==33,"max size 8 GB (2^33)");
    CHECK(_INT_RebarCurExp(I.BarCtl[0])==28,"current size 256 MB (2^28)");
    on=0; out[0]=0; _INT_RebarReport(&I,R);
    CHECK(HAS("Resizable BAR capability (0015) at 150") && HAS("current 256M") && HAS("max 8G") && HAS("256M 512M 1G 2G 4G 8G"),"report text");

    // ---- no ReBAR cap ----
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=hdr(0x0001,2,0);
    _INT_RebarScan(rd,&c,&I); CHECK(I.ExtRead && !I.HasRebar && I.NCaps==1,"no ReBAR cap");
    on=0; _INT_RebarReport(&I,R); CHECK(HAS("NOT present"),"report: not present");

    // ---- empty ext space (all ones: no extended config on this device) ----
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=0xFFFFFFFF;
    _INT_RebarScan(rd,&c,&I); CHECK(I.ExtRead && I.NCaps==0 && !I.HasRebar,"all ones = no caps");

    // ---- extended space not readable (PciIo limited to 256 bytes) ----
    memset(&c,0,sizeof c); c.ext_ok=0;
    CHECK(_INT_RebarScan(rd,&c,&I)==EFI_UNSUPPORTED && !I.ExtRead,"not readable");
    on=0; _INT_RebarReport(&I,R); CHECK(HAS("NOT readable"),"report: not readable");

    // ---- hostile chains must terminate ----
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=hdr(0x0001,1,0x100);          // points at itself
    _INT_RebarScan(rd,&c,&I); CHECK(I.NCaps==1,"self loop stops");
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=hdr(0x0001,1,0x103);          // misaligned
    _INT_RebarScan(rd,&c,&I); CHECK(I.NCaps==1,"misaligned next stops");
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=hdr(0x0001,1,0x200); c.cfg[0x200/4]=hdr(0x0002,1,0x100);   // A->B->A
    _INT_RebarScan(rd,&c,&I); CHECK(I.NCaps==2,"two-node loop stops");
    memset(&c,0,sizeof c); c.ext_ok=1; c.cfg[0x100/4]=hdr(0x0015,1,0); c.cfg[0x108/4]=7u<<5;   // claims 7 entries
    _INT_RebarScan(rd,&c,&I); CHECK(I.NBars<=REBAR_MAX_BARS,"entry count clamped");

    // ---- bridge windows ----
    _INT_BridgeWin W;
    memset(&c,0,sizeof c);
    c.cfg[0x18/4]=0x000A0100|0x01;                                     // pri 01 sec 01? pri=01 sec=01 sub=0A -> see checks
    c.cfg[0x18/4]=(0x0Au<<16)|(0x02u<<8)|0x01u;                        // pri 01 sec 02 sub 0A
    c.cfg[0x20/4]=0xC010B000u;                                         // B0000000 - C01FFFFF
    c.cfg[0x24/4]=0x0001FFF1u;                                         // 64-bit; base field FFF1 low16, limit field 0001 high16 -> empty
    c.cfg[0x24/4]=(0x0001u<<16)|0x0001u;                               // base 0, limit 0xFFFFF (+ upper)
    c.cfg[0x28/4]=0x20; c.cfg[0x2C/4]=0x23;
    CHECK(_INT_BridgeDecode(rd,&c,&W)==EFI_SUCCESS,"bridge decode");
    CHECK(W.Pri==1&&W.Sec==2&&W.Sub==0x0A,"bridge buses");
    CHECK(W.MemBase==0xB0000000u&&W.MemLimit==0xC01FFFFFu&&!W.MemEmpty,"mem window B0000000-C01FFFFF (as in pnpdump)");
    CHECK(W.PrefBits64&&W.PrefBase==0x2000000000ull&&W.PrefLimit==0x23000FFFFFull&&!W.PrefEmpty,"64-bit prefetchable window");
    c.cfg[0x24/4]=(0x0001u<<16)|0xFFF1u; c.cfg[0x28/4]=0; c.cfg[0x2C/4]=0;
    _INT_BridgeDecode(rd,&c,&W); CHECK(W.PrefEmpty,"inverted prefetchable window = empty");
    c.cfg[0x24/4]=(0x0000u<<16)|0x0000u; _INT_BridgeDecode(rd,&c,&W);
    CHECK(!W.PrefBits64 && W.PrefBase==0 && W.PrefLimit==0xFFFFF,"32-bit only window ignores the upper registers");
    on=0; _INT_BridgeReport(&W,R); CHECK(HAS("32-bit only"),"bridge report");

    // ---- NVS: synthetic table: OperationRegion(SANV, SystemMemory, 0x7AF3DC98, 0x1F4); Field(SANV){ pad 24 bits, M64B 64, M64L 64 } ----
    UINT8 t[256]; memset(t,0,sizeof t); size_t n=36;
    memcpy(t,"SSDT",4);
    UINT8 op[]={0x5B,0x80,'S','A','N','V',0x00,0x0C,0x98,0xDC,0xF3,0x7A,0x0B,0xF4,0x01};
    memcpy(t+n,op,sizeof op); n+=sizeof op;
    UINT8 fld[]={0x5B,0x81,0x18,'S','A','N','V',0x01,   // pkglen 0x18 incl. itself
                 0x00,0x18,                              // reserved 24 bits
                 'M','6','4','B',0x40,0x04,              // 64 bits: PkgLength 2-byte form (0x40 | 64&15, 64>>4)
                 'M','6','4','L',0x40,0x04,
                 'M','3','2','B',0x20};
    fld[2]=(UINT8)(sizeof fld-2); memcpy(t+n,fld,sizeof fld); n+=sizeof fld;
    _INT_NvsInfo N;
    CHECK(_INT_NvsParse(t,(UINT32)n,&N),"synthetic NVS parsed");
    CHECK(N.RegionAddr==0x7AF3DC98ull && N.RegionLen==0x1F4 && N.OffM64B==3 && N.OffM64L==11 && !strcmp((char*)N.Region,"SANV"),"synthetic NVS values");
    CHECK(!_INT_NvsParse(t,40,&N),"truncated table refused");
    UINT8 u[sizeof t]; memcpy(u,t,sizeof t); u[36+sizeof op+3]='X';     // region name in the Field no longer matches any OperationRegion
    CHECK(!_INT_NvsParse(u,(UINT32)n,&N),"field without matching OperationRegion -> not found");

    // ---- real SaSsdt ----
    if(argc>1){
        FILE* fp=fopen(argv[1],"rb"); if(!fp){puts("cannot open dat");return 2;}
        static UINT8 buf[131072]; size_t z=fread(buf,1,sizeof buf,fp); fclose(fp);
        CHECK(_INT_NvsParse(buf,(UINT32)z,&N),"real SaSsdt parsed");
        CHECK(N.RegionAddr==0x7AF3DC98ull && N.OffM64B==218 && N.OffM64L==226 && N.HasM32 && N.OffM32B==243,"real SaSsdt: SANV 0x7AF3DC98, M64B +218, M64L +226, M32B +243");
        printf("real SaSsdt: region %s at %llX len %X, M64B +%u M64L +%u\n",N.Region,(unsigned long long)N.RegionAddr,N.RegionLen,N.OffM64B,N.OffM64L);
    }
    if(fails){printf("%d FAILED\n",fails);return 1;}
    puts("ALL OK"); return 0;
}
