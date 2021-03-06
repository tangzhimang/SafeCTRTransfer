#include "nandcmac.h"
#include "sha.h"
#include "aes.h"
#include "qff.h"

// CMAC types, see:
// https://www.3dbrew.org/wiki/Savegames#AES_CMAC_header
// https://3dbrew.org/wiki/Nand/private/movable.sed
// https://3dbrew.org/wiki/3DS_Virtual_Console#NAND_Savegame
#define CMAC_EXTDATA_SD     1
#define CMAC_EXTDATA_SYS    2
#define CMAC_SAVEDATA_SYS   3
#define CMAC_SAVE_GAMECARD  4
#define CMAC_SAVEGAME       5 // this is not calclated into a CMAC
#define CMAC_SAVEDATA_SD    6
#define CMAC_TITLEDB_SYS    7
#define CMAC_TITLEDB_SD     8
#define CMAC_MOVABLE        9

// see: https://www.3dbrew.org/wiki/Savegames#AES_CMAC_header
#define CMAC_SAVETYPE NULL, "CTR-EXT0", "CTR-EXT0", "CTR-SYS0", "CTR-NOR0", "CTR-SAV0", "CTR-SIGN", "CTR-9DB0", "CTR-9DB0", NULL, NULL

// see: http://3dbrew.org/wiki/AES_Registers#Keyslots
#define CMAC_KEYSLOT 0xFF, 0x30 /*0x3A?*/, 0x30, 0x30, 0x33 /*0x19*/, 0xFF, 0x30, 0x0B, 0x30, 0x0B, 0x24

// see: https://www.3dbrew.org/wiki/Title_Database
#define SYS_DB_NAMES "ticket.db", "certs.db", "title.db", "import.db", "tmp_t.db", "tmp_i.db"

// CMAC scan paths - just for reference, information used below
// see: https://www.3dbrew.org/wiki/Savegames#AES_CMAC_header
// see: https://www.3dbrew.org/wiki/Title_Database
//  "%c:/extdata/%08lx/%08lx/%08lx/%08lx"                       SD extdata - drv / xid_high / xid_low / fid_low / fid_high
//  "%c:/data/%016llx%016llx/extdata/%08lx/%08lx/%08lx/%08lx"   SYS extdata - drv / id0_high / id0_low / xid_high / xid_low / fid_low / fid_high
//  "%c:/data/%016llx%016llx/extdata/%08lx/%08lx/Quota.dat"     SYS extdata - drv / id0_high / id0_low / xid_high / xid_low
//  "%c:/data/%016llx%016llx/sysdata/%08lx/%08lx                SYS savedata - drv / id0_high / id0_low / fid_low / fid_high
//  "%c:/title/%08lx/%08lx/data/%08lx.sav"                      SD savedata - drv / tid_high / tid_low / sid
//  "%c:/dbs/ticket.db"                                         SYS database (0x0)
//  "%c:/dbs/cert.db"                                           SYS database (0x1)
//  "%c:/dbs/title.db"                                          SYS database (0x2)
//  "%c:/dbs/import.db"                                         SYS database (0x3)
//  "%c:/dbs/tmp_t.db"                                          SYS database (0x4)
//  "%c:/dbs/tmp_i.db"                                          SYS database (0x5)
//  "%c:/private/movable.sed"                                   movable.sed
//  "%c:/agbsave.bin"                                           virtual AGBSAVE file (not supported!)


u32 CheckCmacHeader(const char* path) {
    u8 cmac_hdr[0x100];
    UINT br;
    
    if ((f_qread(path, cmac_hdr, 0, 0x100, &br) != FR_OK) || (br != 0x100))
        return 1;
    for (u32 i = 0x10; i < 0x100; i++)
        if (cmac_hdr[i] != 0x00) return 1;
    
    return 0;
}

u32 CheckCmacPath(const char* path) {
    return (CalculateFileCmac(path, NULL)) ? 0 : 1;
}

u32 ReadFileCmac(const char* path, u8* cmac) {
    u32 cmac_type = CalculateFileCmac(path, NULL);
    u32 offset = 0;
    UINT br;
    
    if (!cmac_type) return 1;
    else if (cmac_type == CMAC_MOVABLE) offset = 0x130;
    else offset = 0x000;
    
    return ((f_qread(path, cmac, offset, 0x10, &br) != FR_OK) || (br != 0x10)) ? 1 : 0;
}

u32 WriteFileCmac(const char* path, u8* cmac) {
    u32 cmac_type = CalculateFileCmac(path, NULL);
    u32 offset = 0;
    UINT bw;
    
    if (!cmac_type) return 1;
    else if (cmac_type == CMAC_MOVABLE) offset = 0x130;
    else offset = 0x000;
    
    return ((f_qwrite(path, cmac, offset, 0x10, &bw) != FR_OK) || (bw != 0x10)) ? 1 : 0;
}

u32 CalculateFileCmac(const char* path, u8* cmac) {
    u32 cmac_type = 0;
    char drv = *path; // drive letter
    u32 xid_high, xid_low; // extdata ID
    u32 fid_high, fid_low; // extfile ID
    u32 tid_high, tid_low; // title ID
    u32 sid; // save ID / various uses
    char* name;
    char* ext;
    
    name = strrchr(path, '/'); // filename
    if (!name) return 0; // will not happen
    name++;
    ext = strrchr(name, '.'); // extension
    if (ext) ext++;
    
    if ((drv == 'A') || (drv == 'B')) { // data installed on SD
        if (sscanf(path, "%c:/extdata/%08lx/%08lx/%08lx/%08lx", &drv, &xid_high, &xid_low, &fid_high, &fid_low) == 5) {
            sid = 1;
            cmac_type = CMAC_EXTDATA_SD;
        } else if ((sscanf(path, "%c:/title/%08lx/%08lx/data/%08lx.sav", &drv, &tid_high, &tid_low, &sid) == 4) &&
            ext && (strncmp(ext, "sav", 4) == 0) && (CheckCmacHeader(path) == 0)) {
            cmac_type = CMAC_SAVEDATA_SD;
        }
    } else if ((drv == '1') || (drv == '4') || (drv == '7')) { // data on CTRNAND
        u64 id0_high, id0_low; // ID0
        if (sscanf(path, "%c:/data/%016llx%016llx/extdata/%08lx/%08lx/%08lx/%08lx", &drv, &id0_high, &id0_low, &xid_high, &xid_low, &fid_high, &fid_low) == 7) {
            sid = 1;
            cmac_type = CMAC_EXTDATA_SYS;
        } else if ((sscanf(path, "%c:/data/%016llx%016llx/extdata/%08lx/%08lx/Quota.dat", &drv, &id0_high, &id0_low, &xid_high, &xid_low) == 5) && (strncasecmp(name, "Quota.dat", 10) == 0)) {
            sid = 0;
            fid_low = fid_high = 0;
            cmac_type = CMAC_EXTDATA_SYS;
        } else if (sscanf(path, "%c:/data/%016llx%016llx/sysdata/%08lx/%08lx", &drv, &id0_high, &id0_low, &fid_low, &fid_high) == 5)
            cmac_type = CMAC_SAVEDATA_SYS;
    }
    
    if (!cmac_type) { // path independent stuff
        const char* db_names[] = { SYS_DB_NAMES };
        for (sid = 0; sid < sizeof(db_names) / sizeof(char*); sid++)
            if (strncmp(name, db_names[sid], 16) == 0) break;
        if (sid < sizeof(db_names) / sizeof(char*))
            cmac_type = ((drv == 'A') || (drv == 'B')) ? CMAC_TITLEDB_SD : CMAC_TITLEDB_SYS;
        else if (strncmp(name, "movable.sed", 16) == 0)
            cmac_type = CMAC_MOVABLE;
    }
    
    // exit with cmac_type if (u8*) cmac is NULL
    // somewhat hacky, but can be used to check if file has a CMAC
    if (!cmac) return cmac_type;
    else if (!cmac_type) return 1;
    
    const u32 cmac_keyslot[] = { CMAC_KEYSLOT };
    u8* data = (u8*) WORK_BUFFER;
    u32 keyslot = cmac_keyslot[cmac_type];
    u8* hashdata = NULL; 
    u32 hashsize = 0;
    UINT br;
    
    // setup slot 0x30 via movable.sed
    if (keyslot == 0x30) {
        u8 keyy[16] __attribute__((aligned(32)));
        char movable_path[32];
        snprintf(movable_path, 32, "%c:/private/movable.sed", (drv == 'A') ? '1' : (drv == 'B') ? '4' : drv);
        if ((f_qread(movable_path, keyy, 0x110, 0x10, &br) != FR_OK) || (br != 0x10)) return 1;
        setup_aeskeyY(0x30, keyy);
        use_aeskey(0x30);
    }
    
    // build hash data block, get size
    if (cmac_type == CMAC_MOVABLE) { // movable.sed
        // see: https://3dbrew.org/wiki/Nand/private/movable.sed
        if ((f_qread(path, data, 0, 0x140, &br) != FR_OK) || (br != 0x140))
            return 1;
        hashsize = 0x130;
        hashdata = data;
    } else { // "savegame" CMACs
        // see: https://3dbrew.org/wiki/Savegames
        const char* cmac_savetype[] = { CMAC_SAVETYPE };
        u8* disa = data;
        hashdata = data + 0x400;
        if ((f_qread(path, disa, 0x100, 0x100, &br) != FR_OK) || (br != 0x100))
            return 1;
        memcpy(hashdata, cmac_savetype[cmac_type], 8);
        if ((cmac_type == CMAC_EXTDATA_SD) || (cmac_type == CMAC_EXTDATA_SYS)) {
            memcpy(hashdata + 0x08, &xid_low, 4);
            memcpy(hashdata + 0x0C, &xid_high, 4);
            memcpy(hashdata + 0x10, &sid, 4);
            memcpy(hashdata + 0x14, &fid_low, 4);
            memcpy(hashdata + 0x18, &fid_high, 4);
            memcpy(hashdata + 0x1C, disa, 0x100);
            hashsize = 0x11C;
        } else if (cmac_type == CMAC_SAVEDATA_SYS) {
            memcpy(hashdata + 0x08, &fid_low, 4);
            memcpy(hashdata + 0x0C, &fid_high, 4);
            memcpy(hashdata + 0x10, disa, 0x100);
            hashsize = 0x110;
        } else if (cmac_type == CMAC_SAVEDATA_SD) {
            u8* hashdata0 = data + 0x200;
            memcpy(hashdata0 + 0x00, cmac_savetype[CMAC_SAVEGAME], 8);
            memcpy(hashdata0 + 0x08, disa, 0x100);
            memcpy(hashdata + 0x08, &tid_low, 4);
            memcpy(hashdata + 0x0C, &tid_high, 4);
            sha_quick(hashdata + 0x10, hashdata0, 0x108, SHA256_MODE);
            hashsize = 0x30;
        } else if ((cmac_type == CMAC_TITLEDB_SD) || (cmac_type == CMAC_TITLEDB_SYS)) {
            memcpy(hashdata + 0x08, &sid, 4);
            memcpy(hashdata + 0x0C, disa, 0x100);
            hashsize = 0x10C;
        }
    }
        
    // calculate CMAC
    u8 shasum[32];
    if (!hashdata || !hashsize) return 1;
    sha_quick(shasum, hashdata, hashsize, SHA256_MODE);
    use_aeskey(keyslot);
    aes_cmac(shasum, cmac, 2);
    
    return 0;
}

u32 CheckFileCmac(const char* path) {
    u8 fcmac[16];
    u8 ccmac[16];
    return ((ReadFileCmac(path, fcmac) == 0) && (CalculateFileCmac(path, ccmac) == 0) &&
        (memcmp(fcmac, ccmac, 16) == 0)) ? 0 : 1;
}

u32 FixFileCmac(const char* path) {
    u8 ccmac[16];
    return ((CalculateFileCmac(path, ccmac) == 0) && (WriteFileCmac(path, ccmac) == 0)) ? 0 : 1;
}
