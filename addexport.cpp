// addexport.cpp — gives a DLL that exports nothing a minimal export directory,
// so it can be bound into an exe's import table by name.
//
// Why this exists: ClientExt.dll does all of its work in DllMain and exports no
// symbols at all. A static import needs a symbol to name — and an import
// descriptor with empty INT/IAT arrays does NOT work: the Windows loader has
// nothing to snap, skips the descriptor entirely, and never loads the DLL
// (verified experimentally, not assumed). So we append an export directory
// advertising one dummy symbol.
//
// The exported symbol is never called. Importing a function only resolves its
// address into the exe's IAT; nothing invokes it. We still point it at a real
// `ret` instruction found in an executable section, so the address is valid and
// harmless if anything ever did call it. Behaviour of the DLL is unchanged —
// DllMain still runs exactly as before.
//
// Usage:  ebonhold_addexport.exe <dll> [out.dll] [--name SYMBOL]
//   no out.dll -> rewrites in place (a .bak is written first).
//   default SYMBOL is "ClientExt_load".

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static DWORD AlignUp(DWORD v, DWORD a) { return (v + a - 1) & ~(a - 1); }

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: ebonhold_addexport.exe <dll> [out.dll] [--name SYMBOL]\n");
        return 1;
    }
    const char* inPath = NULL; const char* outPath = NULL;
    std::string sym = "ClientExt_load";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) sym = argv[++i];
        else if (!inPath) inPath = argv[i];
        else if (!outPath) outPath = argv[i];
    }
    if (!inPath) { printf("ERROR: no input dll\n"); return 1; }
    bool inPlace = (outPath == NULL);
    if (!outPath) outPath = inPath;

    FILE* f = fopen(inPath, "rb");
    if (!f) { printf("ERROR: cannot open %s\n", inPath); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<BYTE> img(sz);
    fread(img.data(), 1, sz, f); fclose(f);

    // Snapshot before any header mutation — the .bak below must be the untouched
    // input, not the half-updated image.
    const std::vector<BYTE> pristine(img);

    BYTE* b = img.data();
    DWORD e = *(DWORD*)(b + 0x3C);
    if (*(DWORD*)(b + e) != 0x00004550) { printf("ERROR: not a PE\n"); return 1; }

    WORD  numSec  = *(WORD*)(b + e + 6);
    WORD  optSize = *(WORD*)(b + e + 20);
    BYTE* opt     = b + e + 24;
    if (*(WORD*)opt != 0x10B) { printf("ERROR: not PE32 (x86)\n"); return 1; }

    DWORD sectAlign  = *(DWORD*)(opt + 0x20);
    DWORD fileAlign  = *(DWORD*)(opt + 0x24);
    DWORD sizeOfHdrs = *(DWORD*)(opt + 0x3C);
    BYTE* secTab = opt + optSize;

    if (*(DWORD*)(opt + 0x60) != 0) {
        printf("[export] %s already has an export directory (RVA 0x%X) - nothing to do\n",
               inPath, *(DWORD*)(opt + 0x60));
        if (!inPlace) {   // still produce the requested output
            FILE* fo = fopen(outPath, "wb");
            if (!fo) { printf("ERROR: cannot write %s\n", outPath); return 1; }
            fwrite(img.data(), 1, img.size(), fo); fclose(fo);
        }
        return 0;
    }

    // Room in the header area for one more 40-byte section header?
    DWORD hdrEnd = (DWORD)((secTab + numSec * 40) - b);
    if (hdrEnd + 40 > sizeOfHdrs) {
        printf("ERROR: no room for another section header (0x%X + 40 > SizeOfHeaders 0x%X)\n",
               hdrEnd, sizeOfHdrs);
        return 1;
    }

    // Find a `ret` (0xC3) inside an executable section to point the export at.
    DWORD retRVA = 0;
    for (int i = 0; i < numSec && !retRVA; ++i) {
        BYTE* s = secTab + i * 40;
        DWORD chr = *(DWORD*)(s + 36);
        if (!(chr & IMAGE_SCN_MEM_EXECUTE)) continue;
        DWORD va = *(DWORD*)(s + 12), rs = *(DWORD*)(s + 16), pr = *(DWORD*)(s + 20);
        for (DWORD k = 0; k < rs; ++k)
            if (b[pr + k] == 0xC3) { retRVA = va + k; break; }
    }
    if (!retRVA) { printf("ERROR: no `ret` found in an executable section\n"); return 1; }

    DWORD lastVAEnd = 0;
    for (int i = 0; i < numSec; ++i) {
        BYTE* s = secTab + i * 40;
        DWORD va = *(DWORD*)(s + 12), vs = *(DWORD*)(s + 8), rs = *(DWORD*)(s + 16);
        DWORD vEnd = va + AlignUp(vs ? vs : rs, sectAlign);
        if (vEnd > lastVAEnd) lastVAEnd = vEnd;
    }
    DWORD newVA  = AlignUp(lastVAEnd, sectAlign);
    DWORD newOff = AlignUp((DWORD)img.size(), fileAlign);

    // Derive the module name the export directory advertises from the output name.
    std::string modName = outPath;
    size_t slash = modName.find_last_of("\\/");
    if (slash != std::string::npos) modName = modName.substr(slash + 1);

    std::vector<BYTE> blob;
    auto put32 = [&](DWORD v) { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 4); };
    auto put16 = [&](WORD v)  { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 2); };

    blob.resize(40, 0);                       // IMAGE_EXPORT_DIRECTORY
    DWORD offFuncs = (DWORD)blob.size(); put32(retRVA);   // AddressOfFunctions[1]
    DWORD offNames = (DWORD)blob.size(); put32(0);        // AddressOfNames[1] (patched below)
    DWORD offOrds  = (DWORD)blob.size(); put16(0);        // AddressOfNameOrdinals[1]
    DWORD offMod   = (DWORD)blob.size();
    blob.insert(blob.end(), modName.c_str(), modName.c_str() + modName.size() + 1);
    DWORD offSym   = (DWORD)blob.size();
    blob.insert(blob.end(), sym.c_str(), sym.c_str() + sym.size() + 1);

    *(DWORD*)(blob.data() + offNames) = newVA + offSym;

    BYTE* ed = blob.data();
    *(DWORD*)(ed + 12) = newVA + offMod;      // Name
    *(DWORD*)(ed + 16) = 1;                   // Base (ordinal 1)
    *(DWORD*)(ed + 20) = 1;                   // NumberOfFunctions
    *(DWORD*)(ed + 24) = 1;                   // NumberOfNames
    *(DWORD*)(ed + 28) = newVA + offFuncs;    // AddressOfFunctions
    *(DWORD*)(ed + 32) = newVA + offNames;    // AddressOfNames
    *(DWORD*)(ed + 36) = newVA + offOrds;     // AddressOfNameOrdinals

    DWORD virtSize = (DWORD)blob.size();
    DWORD rawBlob  = AlignUp(virtSize, fileAlign);
    blob.resize(rawBlob, 0);

    char secName[9] = ".edat";
    for (int n = 2; ; ++n) {
        bool clash = false;
        for (int i = 0; i < numSec; ++i)
            if (memcmp(secTab + i * 40, secName, 8) == 0) { clash = true; break; }
        if (!clash) break;
        _snprintf(secName, sizeof(secName), ".edat%d", n);
    }

    BYTE* nh = secTab + numSec * 40;
    memset(nh, 0, 40);
    memcpy(nh, secName, strlen(secName));
    *(DWORD*)(nh + 8)  = virtSize;
    *(DWORD*)(nh + 12) = newVA;
    *(DWORD*)(nh + 16) = rawBlob;
    *(DWORD*)(nh + 20) = newOff;
    *(DWORD*)(nh + 36) = 0x40000040;          // INITIALIZED_DATA | READ

    *(WORD*)(b + e + 6) = numSec + 1;
    *(DWORD*)(opt + 0x38) = AlignUp(newVA + virtSize, sectAlign);   // SizeOfImage
    *(DWORD*)(opt + 0x60) = newVA;                                  // export dir RVA
    *(DWORD*)(opt + 0x64) = virtSize;                               // export dir size

    std::vector<BYTE> out(img.begin(), img.end());
    if (out.size() < newOff) out.resize(newOff, 0);
    out.resize(newOff + blob.size());
    memcpy(out.data() + newOff, blob.data(), blob.size());

    if (inPlace) {
        char bak[MAX_PATH]; _snprintf(bak, sizeof(bak), "%s.bak", inPath);
        if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
            FILE* fb = fopen(bak, "wb");
            if (fb) { fwrite(pristine.data(), 1, pristine.size(), fb); fclose(fb); printf("[export] backup -> %s\n", bak); }
        }
    }
    FILE* fo = fopen(outPath, "wb");
    if (!fo) { printf("ERROR: cannot write %s\n", outPath); return 1; }
    fwrite(out.data(), 1, out.size(), fo); fclose(fo);

    printf("[export] %s: added export %s (ordinal 1 -> RVA 0x%X), section %s @ RVA 0x%X\n",
           outPath, sym.c_str(), retRVA, secName, newVA);
    return 0;
}
