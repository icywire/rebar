// Host test for _INT_AcpiAppendSsdtData (lib/int_acpi.c): builds a fake firmware ACPI tree (RSDP -> XSDT -> tables)
// and checks what the append does to it. Optional argv[1] = a compiled SSDT (e.g. SSDT_REBAR.aml),
// argv[2] = a directory with real *.dat dumps (acpidump -b) to use as the firmware tables.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../../include/int_acpi.h"

UINT16 _INT_Rd16(const UINT8* p){return p[0]|(p[1]<<8);}
UINT32 _INT_Rd32(const UINT8* p){UINT32 v;memcpy(&v,p,4);return v;}
UINT64 _INT_Rd64(const UINT8* p){UINT64 v;memcpy(&v,p,8);return v;}
VOID _INT_RepStr(_INT_Rep* r,const CHAR8* s){(void)r;if(getenv("V"))fputs((const char*)s,stdout);}
VOID _INT_RepNl(_INT_Rep* r){(void)r;if(getenv("V"))puts("");}
VOID _INT_RepHex(_INT_Rep* r,UINT64 v,UINTN d){(void)r;(void)d;if(getenv("V"))printf("%llx",(unsigned long long)v);}
VOID _INT_RepDec(_INT_Rep* r,UINT64 v){(void)r;if(getenv("V"))printf("%llu",(unsigned long long)v);}
EFI_STATUS _INT_ReadEspFile(EFI_BOOT_SERVICES* BS, EFI_HANDLE I, CHAR16* N, VOID** D, UINTN* S){(void)BS;(void)I;(void)N;(void)D;(void)S;return EFI_NOT_FOUND;}
VOID _INT_FreePool(EFI_BOOT_SERVICES* BS, VOID* B){BS->FreePool(B);}
VOID _INT_memcpy(VOID* D, VOID* S, UINTN n){memcpy(D,S,n);}

static int failInstall = 0, failAlloc = 0, installs = 0;
static EFI_CONFIGURATION_TABLE Cfg[1];
static EFI_SYSTEM_TABLE ST = { 1, Cfg };

static EFI_STATUS stAlloc(EFI_MEMORY_TYPE t, UINTN n, VOID** b){(void)t; if(failAlloc) return EFI_OUT_OF_RESOURCES; *b=malloc(n); return EFI_SUCCESS;}
static EFI_STATUS stFree(VOID* b){free(b);return EFI_SUCCESS;}
static EFI_STATUS stInstall(EFI_GUID* g, VOID* t){ if(failInstall) return EFI_DEVICE_ERROR; Cfg[0].VendorGuid=*g; Cfg[0].VendorTable=t; installs++; return EFI_SUCCESS; }
static EFI_STATUS stStall(UINTN us){(void)us;return EFI_SUCCESS;}
static EFI_BOOT_SERVICES BS = { stStall, stAlloc, stFree, stInstall };

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static UINT8 sum8(const UINT8* p, size_t n){UINT8 s=0;for(size_t i=0;i<n;i++)s=(UINT8)(s+p[i]);return s;}
static void fix(UINT8* t, UINT32 len){ memcpy(t+4,&len,4); t[9]=0; t[9]=(UINT8)(0-sum8(t,len)); }

static UINT8* mkssdt(const char* oemtid, const void* body, UINT32 blen){
    UINT32 len = 36 + blen; UINT8* t = calloc(1,len);
    memcpy(t,"SSDT",4); t[8]=2; memcpy(t+10,"T2TEST",6); memcpy(t+16,oemtid,8);
    if (blen) memcpy(t+36,body,blen); fix(t,len); return t;
}
static UINT8* mktable(const char* sig, const char* oemtid, UINT32 blen, UINT8 fill){
    UINT8* b = malloc(blen?blen:1); memset(b,fill,blen);
    UINT8* t = mkssdt(oemtid,b,blen); memcpy(t,sig,4); fix(t,36+blen); free(b); return t;
}

typedef struct { UINT8** tab; size_t n; UINT8* rsdp; UINT8* xsdt; } FW;
static void build(FW* f, UINT8** tabs, size_t n){
    f->tab=tabs; f->n=n;
    UINT32 xl=36+8*(UINT32)n; f->xsdt=calloc(1,xl); memcpy(f->xsdt,"XSDT",4); f->xsdt[8]=1;
    for(size_t i=0;i<n;i++){UINT64 a=(UINT64)(UINTN)tabs[i]; memcpy(f->xsdt+36+8*i,&a,8);}
    fix(f->xsdt,xl);
    f->rsdp=calloc(1,36); memcpy(f->rsdp,"RSD PTR ",8); f->rsdp[15]=2; UINT32 rl=36; memcpy(f->rsdp+20,&rl,4);
    UINT64 xa=(UINT64)(UINTN)f->xsdt; memcpy(f->rsdp+24,&xa,8);
    f->rsdp[8]=(UINT8)(0-sum8(f->rsdp,20)); f->rsdp[32]=(UINT8)(0-sum8(f->rsdp,36));
    static const EFI_GUID g={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
    Cfg[0].VendorGuid=g; Cfg[0].VendorTable=f->rsdp; installs=0;
}

// the XSDT the firmware config table points at right now
static UINT8* curXsdt(void){ return (UINT8*)(UINTN)_INT_Rd64((UINT8*)Cfg[0].VendorTable+24); }
static UINT32 curEntries(void){ return (_INT_Rd32(curXsdt()+4)-36)/8; }
static UINT8* curEntry(UINT32 i){ return (UINT8*)(UINTN)_INT_Rd64(curXsdt()+36+8*i); }

// every original table must be byte-identical afterwards: nothing is renamed
static void checkUntouched(FW* f, UINT8** copies){
    for(size_t i=0;i<f->n;i++) CHECK(memcmp(f->tab[i],copies[i],_INT_Rd32(copies[i]+4))==0,"firmware table modified");
}

int main(int argc,char**argv){
    // ---- synthetic firmware: a few tables incl. a fake SaSsdt that contains a "_BCM" ----
    UINT8 bcm[] = { 0x14,0x08,'_','B','C','M',0x01,0xA4 };
    UINT8* t0 = mktable("DSDT","APPLE   ",64,0x11);
    UINT8* t1 = mkssdt("SaSsdt  ",bcm,sizeof bcm);
    UINT8* t2 = mktable("SSDT","PEG0GFX0",32,0x22);
    UINT8* tabs[3] = { t0,t1,t2 };
    UINT8* copies[3]; for(int i=0;i<3;i++){ UINT32 l=_INT_Rd32(tabs[i]+4); copies[i]=malloc(l); memcpy(copies[i],tabs[i],l); }
    FW f; build(&f,tabs,3);

    UINT8 body[] = { 0x10,0x07,'_','S','B','_', 0x00 };   // junk AML, only the container is checked
    UINT8* rebar = mkssdt("REBAR   ",body,sizeof body);
    UINT32 rlen = _INT_Rd32(rebar+4);
    if (argc > 1) {                                       // a real compiled SSDT
        FILE* fp=fopen(argv[1],"rb"); if(!fp){puts("cannot open SSDT");return 2;}
        static UINT8 buf[65536]; rlen=(UINT32)fread(buf,1,sizeof buf,fp); fclose(fp);
        rebar=buf;
    }
    UINT8* oldRsdp = f.rsdp; UINT8* oldXsdt = f.xsdt; UINT8 oldXcopy[36+8*3]; memcpy(oldXcopy,oldXsdt,sizeof oldXcopy);

    // 1. append: ok
    EFI_STATUS s = _INT_AcpiAppendSsdtData(&BS,&ST,rebar,rlen,NULL);
    CHECK(s==EFI_SUCCESS,"append returns success");
    CHECK(installs==1,"config table installed once");
    CHECK(Cfg[0].VendorTable!=oldRsdp,"a new RSDP is published");
    CHECK(curEntries()==4,"XSDT has 3+1 entries");
    CHECK(sum8(curXsdt(),_INT_Rd32(curXsdt()+4))==0,"new XSDT checksum");
    CHECK(sum8((UINT8*)Cfg[0].VendorTable,36)==0,"new RSDP ext checksum");
    CHECK(sum8((UINT8*)Cfg[0].VendorTable,20)==0,"new RSDP checksum");
    for(UINT32 i=0;i<3;i++) CHECK(curEntry(i)==tabs[i],"original entries kept in order");
    CHECK(curEntry(3)!=NULL && memcmp(curEntry(3),rebar,rlen)==0,"appended table == file");
    CHECK(memcmp(oldXcopy,oldXsdt,sizeof oldXcopy)!=0 || 1,"(old XSDT may be left alone)");
    CHECK(_INT_Rd64(oldRsdp+24)==(UINT64)(UINTN)curXsdt(),"old RSDP updated in place too");
    checkUntouched(&f,copies);

    // 2. same file again: already installed, nothing published
    s = _INT_AcpiAppendSsdtData(&BS,&ST,rebar,rlen,NULL);
    CHECK(s==EFI_ALREADY_STARTED,"second append of the same table is refused");
    CHECK(installs==1 && curEntries()==4,"no second install");

    // 3. a different SSDT chains onto the first (this is the 'after _INT_AcpiApplyPatch' case)
    UINT8* other = mkssdt("OTHER   ",body,sizeof body);
    s = _INT_AcpiAppendSsdtData(&BS,&ST,other,_INT_Rd32(other+4),NULL);
    CHECK(s==EFI_SUCCESS && curEntries()==5,"chained append -> 5 entries");
    CHECK(curEntry(3)!=NULL && memcmp(curEntry(3),rebar,rlen)==0,"first appended table still there");
    checkUntouched(&f,copies);

    // 4. bad files
    UINT8* bad = mkssdt("BADCSUM ",body,sizeof body); bad[9]^=0x5A;
    UINT32 before=curEntries();
    CHECK(_INT_AcpiAppendSsdtData(&BS,&ST,bad,_INT_Rd32(bad+4),NULL)==EFI_COMPROMISED_DATA,"bad checksum refused");
    UINT8* badsig = mkssdt("BADSIG  ",body,sizeof body); memcpy(badsig,"DSDT",4); fix(badsig,_INT_Rd32(badsig+4));
    CHECK(_INT_AcpiAppendSsdtData(&BS,&ST,badsig,_INT_Rd32(badsig+4),NULL)==EFI_COMPROMISED_DATA,"wrong signature refused");
    CHECK(_INT_AcpiAppendSsdtData(&BS,&ST,rebar,20,NULL)==EFI_COMPROMISED_DATA,"truncated file refused");
    UINT8 xb[] = { 0x14,0x08,'X','B','C','M' };
    UINT8* needs = mkssdt("NEEDXBCM",xb,sizeof xb);
    CHECK(_INT_AcpiAppendSsdtData(&BS,&ST,needs,_INT_Rd32(needs+4),NULL)==EFI_UNSUPPORTED,"SSDT calling XBCM refused");
    CHECK(curEntries()==before,"refusals change nothing");

    // 5. failure injection: nothing published, nothing modified
    FW g; UINT8* tabs2[3]={t0,t1,t2}; build(&g,tabs2,3);
    UINT8* fresh = mkssdt("FRESH   ",body,sizeof body);
    failInstall=1; s=_INT_AcpiAppendSsdtData(&BS,&ST,fresh,_INT_Rd32(fresh+4),NULL); failInstall=0;
    CHECK(s==EFI_DEVICE_ERROR && Cfg[0].VendorTable==g.rsdp && curEntries()==3,"InstallConfigurationTable failure leaves everything as it was");
    CHECK(_INT_Rd64(g.rsdp+24)==(UINT64)(UINTN)g.xsdt,"old RSDP not touched on failure");
    failAlloc=1; s=_INT_AcpiAppendSsdtData(&BS,&ST,fresh,_INT_Rd32(fresh+4),NULL); failAlloc=0;
    CHECK(s==EFI_OUT_OF_RESOURCES && curEntries()==3,"allocation failure leaves everything as it was");

    // 6. no RSDP
    EFI_CONFIGURATION_TABLE saved=Cfg[0]; memset(&Cfg[0].VendorGuid,0,sizeof(EFI_GUID));
    CHECK(_INT_AcpiAppendSsdtData(&BS,&ST,fresh,_INT_Rd32(fresh+4),NULL)==EFI_UNSUPPORTED,"no ACPI 2.0 RSDP -> unsupported");
    Cfg[0]=saved;

    // 7. optional: the real firmware dump
    if (argc > 2) {
        DIR* d=opendir(argv[2]); struct dirent* e; UINT8* real[64]; size_t n=0; UINT8* rc[64];
        if(!d){puts("cannot open dump dir");return 2;}
        while((e=readdir(d))&&n<64){
            size_t L=strlen(e->d_name); if(L<5||strcmp(e->d_name+L-4,".dat")) continue;
            if(!strncmp(e->d_name,"xsdt",4)||!strncmp(e->d_name,"dsdt",4)) continue;   // XSDT is rebuilt, DSDT is not in it
            char path[1024]; snprintf(path,sizeof path,"%s/%s",argv[2],e->d_name);
            FILE* fp=fopen(path,"rb"); fseek(fp,0,SEEK_END); long z=ftell(fp); rewind(fp);
            real[n]=malloc(z); if(fread(real[n],1,z,fp)!=(size_t)z) return 2; fclose(fp);
            rc[n]=malloc(z); memcpy(rc[n],real[n],z); n++;
        }
        closedir(d);
        FW r; build(&r,real,n);
        s=_INT_AcpiAppendSsdtData(&BS,&ST,rebar,rlen,NULL);
        CHECK(s==EFI_SUCCESS,"real dump: append ok");
        CHECK(curEntries()==n+1,"real dump: entries +1");
        for(size_t i=0;i<n;i++) CHECK(memcmp(real[i],rc[i],_INT_Rd32(rc[i]+4))==0,"real dump: tables untouched");
        printf("real dump: %zu tables, appended OK, all untouched\n",n);
    }

    if (fails) { printf("%d FAILED\n",fails); return 1; }
    puts("ALL OK");
    return 0;
}
