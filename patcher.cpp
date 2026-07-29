// patcher.cpp — adds static imports to Wow.exe so helper DLLs auto-load when the
// exe runs (no launcher needed).
//
// Technique: append a new section containing a fresh IMAGE_IMPORT_DESCRIPTOR
// array (all originals copied verbatim + one new entry per added DLL + null
// terminator) plus each new entry's INT/IAT/name/import-by-name, then repoint the
// PE import data directory at the new array. Originals' thunks/names stay where
// they are, and the IAT data directory is left untouched (zeroing it makes the
// loader write to read-only .rdata -> crash).
//
// Two flavours of added import:
//   dll:func  -> normal import-by-name. The named export is never called; it just
//               gives the descriptor a symbol to resolve. Work happens in DllMain.
//   dll       -> the DLL exports NOTHING, so there is no symbol to name. We emit a
//               descriptor whose INT and IAT are each a bare null terminator. The
//               loader maps the DLL and runs its DllMain, then walks a thunk list
//               that is empty and snaps nothing. This is the only way to statically
//               bind a no-export DLL; import-by-name against one fails at load with
//               "the procedure entry point could not be located".
//
// Re-running is safe: a DLL already present in the import table is skipped rather
// than added twice, and the appended section gets a fresh name (.inj, .inj2, ...)
// so it never collides with a section from an earlier patch.
//
// Usage:  ebonhold_applymod.exe <path\Wow.exe> [out.exe] [--add dll[:func]]...
//   no out.exe -> patches in place and writes a <Wow.exe>.bak backup first.
//   no --add   -> uses the built-in default set below.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct AddImport {
    std::string dll;
    std::string func;      // empty -> DLL exports nothing, emit empty thunks
};

static DWORD AlignUp(DWORD v, DWORD a) { return (v + a - 1) & ~(a - 1); }

static bool IEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: ebonhold_applymod.exe <Wow.exe> [out.exe] [--add dll[:func]]...\n");
        return 1;
    }

    std::vector<AddImport> wanted;
    const char* inPath  = NULL;
    const char* outPath = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--add") == 0 && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t colon = spec.find(':');
            AddImport a;
            if (colon == std::string::npos) { a.dll = spec; }
            else { a.dll = spec.substr(0, colon); a.func = spec.substr(colon + 1); }
            wanted.push_back(a);
        } else if (!inPath)  { inPath  = argv[i]; }
        else if (!outPath)   { outPath = argv[i]; }
    }
    if (!inPath) { printf("ERROR: no input exe given\n"); return 1; }

    if (wanted.empty()) {
        AddImport a; a.dll = "ebonhold.dll";  a.func = "ebonhold_load";  wanted.push_back(a);
        // ClientExt.dll is deliberately NOT statically imported. It is an anti-cheat
        // module whose DllMain spawns a watchdog that calls into game code before
        // the client's globals exist, so a process-init (static) load crashes.
        // ebonhold.dll LoadLibrary()s it once the client is up (see dllmain.cpp,
        // LoadClientExtOnce), which also guarantees ebonhold initializes first.
    }

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

    // Enumerate existing imports (count + names, for the already-present check).
    int origCount = 0;
    std::vector<std::string> origNames;
    for (;; ++origCount) {
        BYTE* d = b + impOff + origCount * 20;
        if (*(DWORD*)(d + 0) == 0 && *(DWORD*)(d + 12) == 0 && *(DWORD*)(d + 16) == 0) break;
        DWORD nOff = rvaToOff(*(DWORD*)(d + 12));
        origNames.push_back(nOff ? (const char*)(b + nOff) : "");
    }
    printf("[patch] %d existing imports\n", origCount);

    std::vector<AddImport> toAdd;
    for (size_t i = 0; i < wanted.size(); ++i) {
        bool present = false;
        for (size_t j = 0; j < origNames.size(); ++j)
            if (IEquals(origNames[j], wanted[i].dll)) { present = true; break; }
        if (present) printf("[patch] %s already imported - skipped\n", wanted[i].dll.c_str());
        else         toAdd.push_back(wanted[i]);
    }
    if (toAdd.empty()) {
        printf("[patch] nothing to add; imports already present\n");
        if (!inPlace) {                     // still emit the requested output (a copy)
            FILE* fo = fopen(outPath, "wb");
            if (!fo) { printf("ERROR: cannot write %s\n", outPath); return 1; }
            fwrite(img.data(), 1, img.size(), fo); fclose(fo);
            printf("[patch] wrote %s (unchanged copy - ebonhold.dll already imported)\n", outPath);
        }
        return 0;
    }

    DWORD lastVAEnd = 0;
    for (int i = 0; i < numSec; ++i) {
        BYTE* s = secTab + i * 40;
        DWORD va = *(DWORD*)(s + 12), vs = *(DWORD*)(s + 8), rs = *(DWORD*)(s + 16);
        DWORD vEnd = va + AlignUp(vs ? vs : rs, sectAlign);
        if (vEnd > lastVAEnd) lastVAEnd = vEnd;
    }
    DWORD newVA  = AlignUp(lastVAEnd, sectAlign);
    DWORD newOff = AlignUp((DWORD)img.size(), fileAlign);   // append after the whole file (keeps overlay)

    int newCount = origCount + (int)toAdd.size();
    DWORD descArrBytes = (newCount + 1) * 20;
    std::vector<BYTE> blob;
    auto put32 = [&](DWORD v) { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 4); };
    auto put16 = [&](WORD v)  { blob.insert(blob.end(), (BYTE*)&v, (BYTE*)&v + 2); };
    auto putStr = [&](const std::string& s) {
        blob.insert(blob.end(), s.c_str(), s.c_str() + s.size() + 1);
        if (blob.size() & 1) blob.push_back(0);   // keep following WORDs aligned
    };

    blob.resize(descArrBytes, 0);
    memcpy(blob.data(), b + impOff, origCount * 20);   // originals verbatim

    for (size_t i = 0; i < toAdd.size(); ++i) {
        const bool named = !toAdd[i].func.empty();

        DWORD offINT = (DWORD)blob.size();
        if (named) put32(0);
        put32(0);                                  // INT null terminator
        DWORD offIAT = (DWORD)blob.size();
        if (named) put32(0);
        put32(0);                                  // IAT null terminator

        DWORD offIBN = 0;
        if (named) {
            offIBN = (DWORD)blob.size();
            put16(0);                              // hint
            putStr(toAdd[i].func);
        }
        DWORD offName = (DWORD)blob.size();
        putStr(toAdd[i].dll);

        if (named) {
            *(DWORD*)(blob.data() + offINT) = newVA + offIBN;
            *(DWORD*)(blob.data() + offIAT) = newVA + offIBN;
        }

        BYTE* nd = blob.data() + (origCount + (int)i) * 20;
        *(DWORD*)(nd + 0)  = newVA + offINT;       // OriginalFirstThunk
        *(DWORD*)(nd + 12) = newVA + offName;      // Name
        *(DWORD*)(nd + 16) = newVA + offIAT;       // FirstThunk
    }

    DWORD rawBlob = AlignUp((DWORD)blob.size(), fileAlign);
    blob.resize(rawBlob, 0);

    // Pick a section name not already used, so repeat patches don't collide.
    char secName[9] = ".inj";
    for (int n = 2; ; ++n) {
        bool clash = false;
        for (int i = 0; i < numSec; ++i)
            if (memcmp(secTab + i * 40, secName, 8) == 0) { clash = true; break; }
        if (!clash) break;
        _snprintf(secName, sizeof(secName), ".inj%d", n);
    }

    BYTE* nh = secTab + numSec * 40;
    memset(nh, 0, 40);
    memcpy(nh, secName, strlen(secName));
    *(DWORD*)(nh + 8)  = (DWORD)blob.size();
    *(DWORD*)(nh + 12) = newVA;
    *(DWORD*)(nh + 16) = rawBlob;
    *(DWORD*)(nh + 20) = newOff;
    *(DWORD*)(nh + 36) = 0xC0000040;   // INITIALIZED_DATA | READ | WRITE — same as a
                                       // normal .idata. WRITE is required: this section
                                       // holds the IAT, and the loader writes each
                                       // resolved address into it. Marking it read-only
                                       // faults the loader (verified: 0xC0000005 at
                                       // startup). Still no EXECUTE.

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
            if (fb) { fwrite(pristine.data(), 1, pristine.size(), fb); fclose(fb); printf("[patch] backup -> %s\n", bak); }
        }
    }
    FILE* fo = fopen(outPath, "wb");
    if (!fo) { printf("ERROR: cannot write %s\n", outPath); return 1; }
    fwrite(out.data(), 1, out.size(), fo); fclose(fo);

    printf("[patch] wrote %s (new section %s @ RVA 0x%X)\n", outPath, secName, newVA);
    for (size_t i = 0; i < toAdd.size(); ++i)
        printf("[patch]   + import %s%s%s\n", toAdd[i].dll.c_str(),
               toAdd[i].func.empty() ? " (no exports - empty thunks)" : "!",
               toAdd[i].func.c_str());
    return 0;
}
