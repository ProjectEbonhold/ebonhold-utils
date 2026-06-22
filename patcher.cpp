// patcher.cpp — adds an import of ebonhold.dll!ebonhold_load to Wow.exe so the
// DLL auto-loads when the exe runs (no launcher needed).
//
// Technique: append a new section containing a fresh IMAGE_IMPORT_DESCRIPTOR
// array (all originals copied verbatim + one new entry + null terminator) plus
// the new entry's INT/IAT/name/import-by-name, then repoint the PE import data
// directory at the new array. Originals' thunks/names stay where they are, and
// the IAT data directory is left untouched (zeroing it makes the loader write to
// read-only .rdata -> crash).
//
// Usage:  ebonhold_applymod.exe <path\Wow.exe> [out.exe]
//   no out.exe -> patches in place and writes a <Wow.exe>.bak backup first.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* DLL_NAME  = "ebonhold.dll";
static const char* FUNC_NAME = "ebonhold_load";

static DWORD AlignUp(DWORD v, DWORD a) { return (v + a - 1) & ~(a - 1); }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: ebonhold_applymod.exe <Wow.exe> [out.exe]\n"); return 1; }
    const char* inPath  = argv[1];
    const char* outPath = (argc >= 3) ? argv[2] : argv[1];
    bool inPlace = (argc < 3);

    FILE* f = fopen(inPath, "rb");
    if (!f) { printf("ERROR: cannot open %s\n", inPath); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<BYTE> img(sz);
    fread(img.data(), 1, sz, f); fclose(f);

    BYTE* b = img.data();
    DWORD e = *(DWORD*)(b + 0x3C);
    if (*(DWORD*)(b + e) != 0x00004550) { printf("ERROR: not a PE\n"); return 1; }

    WORD  numSec  = *(WORD*)(b + e + 6);
    WORD  optSize = *(WORD*)(b + e + 20);
    BYTE* opt     = b + e + 24;
    if (*(WORD*)opt != 0x10B) { printf("ERROR: not PE32 (x86)\n"); return 1; }

    DWORD sectAlign = *(DWORD*)(opt + 0x20);
    DWORD fileAlign = *(DWORD*)(opt + 0x24);
    DWORD impDirRVA = *(DWORD*)(opt + 0x68);   // DataDirectory[1].VirtualAddress
    BYTE* secTab = opt + optSize;

    auto rvaToOff = [&](DWORD rva) -> DWORD {
        for (int i = 0; i < numSec; ++i) {
            BYTE* s = secTab + i * 40;
            DWORD va = *(DWORD*)(s + 12), rs = *(DWORD*)(s + 16), pr = *(DWORD*)(s + 20);
            if (rva >= va && rva < va + rs) return pr + (rva - va);
        }
        return 0;
    };
    DWORD impOff = rvaToOff(impDirRVA);
    if (!impOff) { printf("ERROR: cannot map import dir\n"); return 1; }

    int origCount = 0;
    for (;; ++origCount) {
        BYTE* d = b + impOff + origCount * 20;
        if (*(DWORD*)(d + 0) == 0 && *(DWORD*)(d + 12) == 0 && *(DWORD*)(d + 16) == 0) break;
    }
    printf("[patch] %d existing imports\n", origCount);

    DWORD lastVAEnd = 0;
    for (int i = 0; i < numSec; ++i) {
        BYTE* s = secTab + i * 40;
        DWORD va = *(DWORD*)(s + 12), vs = *(DWORD*)(s + 8), rs = *(DWORD*)(s + 16);
        DWORD vEnd = va + AlignUp(vs ? vs : rs, sectAlign);
        if (vEnd > lastVAEnd) lastVAEnd = vEnd;
    }
    DWORD newVA  = AlignUp(lastVAEnd, sectAlign);
    DWORD newOff = AlignUp((DWORD)img.size(), fileAlign);   // append after the whole file (keeps overlay)

    int newCount = origCount + 1;
    DWORD descArrBytes = (newCount + 1) * 20;
    std::vector<BYTE> blob;
    auto put32 = [&](DWORD v) { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 4); };
    auto put16 = [&](WORD v)  { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 2); };

    blob.resize(descArrBytes, 0);
    DWORD offINT  = (DWORD)blob.size(); put32(0); put32(0);
    DWORD offIAT  = (DWORD)blob.size(); put32(0); put32(0);
    DWORD offIBN  = (DWORD)blob.size(); put16(0);
    blob.insert(blob.end(), FUNC_NAME, FUNC_NAME + strlen(FUNC_NAME) + 1);
    DWORD offName = (DWORD)blob.size();
    blob.insert(blob.end(), DLL_NAME, DLL_NAME + strlen(DLL_NAME) + 1);

    *(DWORD*)(blob.data() + offINT) = newVA + offIBN;
    *(DWORD*)(blob.data() + offIAT) = newVA + offIBN;

    memcpy(blob.data(), b + impOff, origCount * 20);
    BYTE* nd = blob.data() + origCount * 20;
    *(DWORD*)(nd + 0)  = newVA + offINT;
    *(DWORD*)(nd + 12) = newVA + offName;
    *(DWORD*)(nd + 16) = newVA + offIAT;

    DWORD rawBlob = AlignUp((DWORD)blob.size(), fileAlign);
    blob.resize(rawBlob, 0);

    BYTE* nh = secTab + numSec * 40;
    memset(nh, 0, 40);
    memcpy(nh, ".inj", 4);
    *(DWORD*)(nh + 8)  = (DWORD)blob.size();
    *(DWORD*)(nh + 12) = newVA;
    *(DWORD*)(nh + 16) = rawBlob;
    *(DWORD*)(nh + 20) = newOff;
    *(DWORD*)(nh + 36) = 0xC0000040;   // INITIALIZED_DATA | READ | WRITE

    *(WORD*)(b + e + 6) = numSec + 1;
    *(DWORD*)(opt + 0x38) = AlignUp(newVA + (DWORD)blob.size(), sectAlign);  // SizeOfImage
    *(DWORD*)(opt + 0x68) = newVA;                                          // import dir RVA
    *(DWORD*)(opt + 0x6C) = (newCount + 1) * 20;                            // import dir size

    std::vector<BYTE> out(img.begin(), img.end());
    if (out.size() < newOff) out.resize(newOff, 0);
    out.resize(newOff + blob.size());
    memcpy(out.data() + newOff, blob.data(), blob.size());

    if (inPlace) {
        char bak[MAX_PATH]; _snprintf(bak, sizeof(bak), "%s.bak", inPath);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
            FILE* fb = fopen(bak, "wb");
            if (fb) { fwrite(img.data(), 1, img.size(), fb); fclose(fb); printf("[patch] backup -> %s\n", bak); }
        }
    }
    FILE* fo = fopen(outPath, "wb");
    if (!fo) { printf("ERROR: cannot write %s\n", outPath); return 1; }
    fwrite(out.data(), 1, out.size(), fo); fclose(fo);

    printf("[patch] wrote %s (added import %s!%s, new section .inj @ RVA 0x%X)\n",
           outPath, DLL_NAME, FUNC_NAME, newVA);
    return 0;
}
