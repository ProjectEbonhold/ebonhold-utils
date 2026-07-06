// dllmain.cpp — ebonhold.dll
//   * Generic framework to add custom Glue Lua C functions.
//   * Patch-hash check:
//       CheckPatches(apiUrl, realmName) -> MD5(base64) of Data\patch-4/5/6.MPQ,
//         GET <apiUrl><realm> for the realm's expected hashes, compare.
//       GetPatchCheckResult() -> nil while running, else "OK" | "OUTDATED:a,b" | "ERROR:..."
//   * Helpers exposed to Lua: EbonholdLog(msg), EbonholdOpenURL(url)
//   * Runs ebonhold_glue.lua in the glue state. The script is EMBEDDED
//     (glue_script.h, auto-generated from ebonhold_glue.lua by gen_glue.ps1);
//     a loose ebonhold_glue.lua next to the exe overrides the embedded copy.
//
// Build: 32-bit DLL, /MT /Od  (see build.bat).

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <mutex>
#include "offsets.h"
#include "glue_script.h"   // kGlueScript[] — embedded ebonhold_glue.lua (auto-generated)
#include "extbank/client/extbank_client.h"  // ExtBank client half (header-only)
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------- logging ----
// Log under %LOCALAPPDATA%\ebonhold\ebonhold.log (no hardcoded user path —
// hardcoded C:\Users\... paths are a malware-heuristic red flag and don't work
// on anyone else's machine). Path is resolved once and cached.
static const char* LogPath() {
    static char path[MAX_PATH] = { 0 };
    if (path[0]) return path;
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%s\\ebonhold", base);
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path), "%s\\ebonhold.log", dir);
    return path;
}

static void Log(const char* fmt, ...) {
    char buf[1200];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    buf[n] = '\n'; buf[n + 1] = 0;
    OutputDebugStringA(buf);
    const char* lp = LogPath();
    if (!lp) return;
    HANDLE h = CreateFileA(lp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, buf, (DWORD)(n + 1), &w, NULL); CloseHandle(h); }
}

// Bridge so extbank_client.h can log into ebonhold.log without seeing Log()'s
// internal linkage. Declared in extbank_client.h.
namespace ExtBankClient {
    void EBLog(const char* fmt, ...) {
        char buf[1024]; va_list ap; va_start(ap, fmt);
        _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
        buf[sizeof(buf) - 1] = 0;
        Log("%s", buf);
    }
}

// --------------------------------------------------------------- Lua glue ----
typedef int(__cdecl* lua_CFunction)(void* L);
static inline const char* Lua_ToString(void* L, int i) { return ((const char*(__cdecl*)(void*,int,size_t*))Off::lua_tolstring)(L,i,nullptr); }
static inline bool        Lua_IsString(void* L, int i) { return ((int(__cdecl*)(void*,int))Off::lua_isstring)(L,i)!=0; }
static inline void        Lua_PushString(void* L, const char* s) { ((void(__cdecl*)(void*,const char*))Off::lua_pushstring)(L, s?s:""); }
static inline int         Lua_Error(void* L, const char* m) { return ((int(__cdecl*)(void*,const char*,...))Off::luaL_error)(L,"%s",m); }

// ============================================================================
//  PATCH-HASH CHECK
// ============================================================================
static const char* kPatchFiles[] = { "patch-4.MPQ", "patch-5.MPQ", "patch-6.MPQ" };

static std::string GetExeDir() {
    char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH);
    std::string s(p); size_t i = s.find_last_of("\\/");
    return (i == std::string::npos) ? std::string() : s.substr(0, i + 1);
}

// MD5 of a file -> base64 (matches PowerShell [Convert]::ToBase64String)
static bool Md5Base64OfFile(const std::string& path, std::string& out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) { CloseHandle(f); return false; }
    CryptCreateHash(prov, CALG_MD5, 0, 0, &h);
    BYTE buf[65536]; DWORD rd;
    while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd) CryptHashData(h, buf, rd, 0);
    CloseHandle(f);
    BYTE digest[16]; DWORD dl = 16;
    BOOL okv = CryptGetHashParam(h, HP_HASHVAL, digest, &dl, 0);
    CryptDestroyHash(h); CryptReleaseContext(prov, 0);
    if (!okv) return false;
    DWORD n = 0;
    CryptBinaryToStringA(digest, 16, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n);
    std::string s(n, 0);
    CryptBinaryToStringA(digest, 16, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &n);
    s.resize(strlen(s.c_str()));
    out = s; return true;
}

static std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

static bool HttpGet(const std::string& url, std::string& body) {
    wchar_t wurl[2048]; MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl, 2048);
    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[1536];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 1536;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(L"ebonhold-patchcheck/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(r, NULL)) {
                DWORD avail;
                do { avail = 0; WinHttpQueryDataAvailable(r, &avail);
                    if (avail) { std::string chunk(avail, 0); DWORD got = 0;
                        WinHttpReadData(r, &chunk[0], avail, &got); chunk.resize(got); body += chunk; }
                } while (avail > 0);
                ok = true;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

// Pull a patch's hash out of the JSON. Matches the documented shape:
//   { "game_slug": "...", "patches": { "patch-4": "<base64>", ... } }
// `key` is the bare patch name ("patch-4"); finds the quoted key, takes the next "..." value.
static bool FindHashFor(const std::string& json, const std::string& key, std::string& out) {
    std::string lj = json; for (auto& c : lj) c = (char)tolower((unsigned char)c);
    std::string nk = "\""; nk += key; nk += "\"";
    for (auto& c : nk) c = (char)tolower((unsigned char)c);
    size_t pos = lj.find(nk);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + nk.size());  if (colon == std::string::npos) return false;
    size_t q1 = json.find('"', colon);               if (q1 == std::string::npos) return false;
    size_t q2 = json.find('"', q1 + 1);              if (q2 == std::string::npos) return false;
    out = json.substr(q1 + 1, q2 - q1 - 1);
    return !out.empty();
}

static std::mutex  g_pcMutex;
static std::string g_pcResult;
static bool        g_pcDone = false;
static bool        g_pcRunning = false;

struct PcArgs { std::string apiUrl, realm; };

static DWORD WINAPI PatchCheckThread(LPVOID param) {
    PcArgs* a = (PcArgs*)param;
    std::string result;
    std::string dataDir = GetExeDir() + "Data\\";
    // If the URL already ends with "=" (the query param name is supplied, e.g.
    // ".../file-hashes?server_name="), just append the encoded realm value.
    std::string url = a->apiUrl;
    if (!url.empty() && url.back() == '=') {
        url += UrlEncode(a->realm);
    } else {
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += "realm=" + UrlEncode(a->realm);
    }
    Log("[patch] check realm='%s' url=%s", a->realm.c_str(), url.c_str());

    std::string json;
    if (!HttpGet(url, json)) {
        result = "ERROR:http request failed";
    } else {
        Log("[patch] api response (%u bytes): %.300s", (unsigned)json.size(), json.c_str());
        std::string outdated, missing;
        for (const char* pf : kPatchFiles) {
            std::string local;
            if (!Md5Base64OfFile(dataDir + pf, local)) { missing += std::string(pf) + ","; continue; }
            std::string key = pf;                         // "patch-4.MPQ" -> "patch-4"
            size_t dot = key.find_last_of('.');
            if (dot != std::string::npos) key = key.substr(0, dot);
            std::string expected;
            if (!FindHashFor(json, key, expected)) { missing += std::string(pf) + "(no-server-hash),"; continue; }
            Log("[patch] %s local=%s expected=%s", pf, local.c_str(), expected.c_str());
            if (local != expected) outdated += std::string(pf) + ",";
        }
        if (!missing.empty())       result = "ERROR:missing " + missing;
        else if (!outdated.empty()) { if (outdated.back() == ',') outdated.pop_back(); result = "OUTDATED:" + outdated; }
        else                        result = "OK";
    }
    Log("[patch] result = %s", result.c_str());
    { std::lock_guard<std::mutex> lk(g_pcMutex); g_pcResult = result; g_pcDone = true; g_pcRunning = false; }
    delete a;
    return 0;
}

static int __cdecl Lua_CheckPatches(void* L) {     // CheckPatches(apiUrl, realmName)
    if (!Lua_IsString(L, 1) || !Lua_IsString(L, 2))
        return Lua_Error(L, "Usage: CheckPatches(apiUrl, realmName)");
    { std::lock_guard<std::mutex> lk(g_pcMutex);
      if (g_pcRunning) return 0;
      g_pcRunning = true; g_pcDone = false; g_pcResult.clear(); }
    PcArgs* a = new PcArgs();
    a->apiUrl = Lua_ToString(L, 1);
    a->realm  = Lua_ToString(L, 2);
    CreateThread(nullptr, 0, PatchCheckThread, a, 0, nullptr);
    return 0;
}

static int __cdecl Lua_GetPatchCheckResult(void* L) {   // nil while running
    std::string r; bool done;
    { std::lock_guard<std::mutex> lk(g_pcMutex); done = g_pcDone; r = g_pcResult; }
    if (!done) return 0;
    Lua_PushString(L, r.c_str());
    return 1;
}

// ============================================================================
//  LOGIN MESSAGE (MOTD)
//    FetchLoginMessage(url)      -> async GET; expects {date:..., message:"..."}
//    GetLoginMessageResult()     -> nil while running, else
//                                   status ("OK"|"NONE"|"ERROR:..."), date, message
//  Empty body / {} / missing "message" field => "NONE" (nothing to show).
// ============================================================================
static std::mutex  g_lmMutex;
static std::string g_lmStatus, g_lmDate, g_lmMessage;
static bool        g_lmDone = false;
static bool        g_lmRunning = false;

// Extract the value of `key` from a small flat JSON object. Handles quoted
// strings (with \n \t \" \\ \/ escapes; \uXXXX degrades to '?') and bare
// values (numbers, unquoted dates). Returns false if the key is absent.
static bool JsonField(const std::string& json, const char* key, std::string& out) {
    std::string nk = "\""; nk += key; nk += "\"";
    size_t pos = json.find(nk);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + nk.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && isspace((unsigned char)json[i])) ++i;
    if (i >= json.size()) return false;
    out.clear();
    if (json[i] == '"') {
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            char c = json[i];
            if (c == '\\' && i + 1 < json.size()) {
                char e = json[++i];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': break;
                    case 'u': i += 4; out += '?'; break;
                    default:  out += e; break;   // \" \\ \/ ...
                }
            } else out += c;
        }
    } else {
        while (i < json.size() && json[i] != ',' && json[i] != '}') out += json[i++];
        while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    }
    return true;
}

struct LmArgs { std::string url; };

static DWORD WINAPI LoginMsgThread(LPVOID param) {
    LmArgs* a = (LmArgs*)param;
    std::string status = "NONE", date, msg, body;
    if (!HttpGet(a->url, body)) {
        status = "ERROR:http request failed";
    } else {
        Log("[motd] api response (%u bytes): %.300s", (unsigned)body.size(), body.c_str());
        if (JsonField(body, "message", msg) && !msg.empty()) {
            JsonField(body, "date", date);
            status = "OK";
        }
    }
    Log("[motd] result = %s", status.c_str());
    { std::lock_guard<std::mutex> lk(g_lmMutex);
      g_lmStatus = status; g_lmDate = date; g_lmMessage = msg;
      g_lmDone = true; g_lmRunning = false; }
    delete a;
    return 0;
}

static int __cdecl Lua_FetchLoginMessage(void* L) {     // FetchLoginMessage(url)
    if (!Lua_IsString(L, 1))
        return Lua_Error(L, "Usage: FetchLoginMessage(url)");
    { std::lock_guard<std::mutex> lk(g_lmMutex);
      if (g_lmRunning) return 0;
      g_lmRunning = true; g_lmDone = false; }
    LmArgs* a = new LmArgs();
    a->url = Lua_ToString(L, 1);
    CreateThread(nullptr, 0, LoginMsgThread, a, 0, nullptr);
    return 0;
}

static int __cdecl Lua_GetLoginMessageResult(void* L) {  // nil while running
    std::string s, d, m; bool done;
    { std::lock_guard<std::mutex> lk(g_lmMutex);
      done = g_lmDone; s = g_lmStatus; d = g_lmDate; m = g_lmMessage; }
    if (!done) return 0;
    Lua_PushString(L, s.c_str());
    Lua_PushString(L, d.c_str());
    Lua_PushString(L, m.c_str());
    return 3;
}

// ---------------------------------------------- run Lua in the glue state ----
static void ExecGlue(const char* code, const char* src) {
    ((void(__cdecl*)(const char*, const char*, int))Off::FrameScript_Execute)(code, src, 0);
}
static std::string g_glueScript;

// Prefer a loose ebonhold_glue.lua (dev override) next to the exe; otherwise
// use the copy embedded in the DLL (glue_script.h).
static void LoadGlueScript() {
    std::string path = GetExeDir() + "ebonhold_glue.lua";
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(f, NULL), rd = 0;
        g_glueScript.resize(sz);
        ReadFile(f, &g_glueScript[0], sz, &rd, NULL);
        g_glueScript.resize(rd);
        CloseHandle(f);
        Log("[glue] loaded loose ebonhold_glue.lua (%u bytes, overrides embedded)", (unsigned)g_glueScript.size());
        return;
    }
    g_glueScript.assign(kGlueScript);
    Log("[glue] using embedded glue script (%u bytes)", (unsigned)g_glueScript.size());
}

// EbonholdLog("msg") — lets the glue script write into ebonhold.log
static int __cdecl Lua_EbonholdLog(void* L) {
    if (Lua_IsString(L, 1)) Log("[lua] %s", Lua_ToString(L, 1));
    return 0;
}

// EbonholdOpenURL("https://...") — opens the URL in the default browser.
static int __cdecl Lua_OpenURL(void* L) {
    if (Lua_IsString(L, 1)) {
        const char* url = Lua_ToString(L, 1);
        if (url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)) {
            Log("[url] opening %s", url);
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        }
    }
    return 0;
}

// -------------------------------------------------------- generic registry --
struct CustomFn { const char* name; lua_CFunction fn; };
static CustomFn g_customFns[] = {
    { "CheckPatches",         Lua_CheckPatches         },
    { "GetPatchCheckResult",  Lua_GetPatchCheckResult  },
    { "EbonholdLog",          Lua_EbonholdLog          },
    { "EbonholdOpenURL",      Lua_OpenURL              },
    { "FetchLoginMessage",     Lua_FetchLoginMessage    },
    { "GetLoginMessageResult", Lua_GetLoginMessageResult},
    // ExtBank client API. These are registered into BOTH the glue state (via the
    // glue detour) and the IN-GAME state (via InstallExecuteDetour, which
    // re-registers all custom fns whenever the active lua_State changes).
    { "ExtBankOpen",          ExtBankClient::Lua_ExtBankOpen      },
    { "ExtBankMove",          ExtBankClient::Lua_ExtBankMove      },
    { "ExtBankUnlock",        ExtBankClient::Lua_ExtBankUnlock    },
    { "ExtBankSetActive",     ExtBankClient::Lua_ExtBankSetActive },
    // add more custom Glue Lua functions here
};
static void RegisterAllCustomFns() {
    auto RegFn = (void(__cdecl*)(const char*, lua_CFunction))Off::FrameScript_RegisterFunction;
    for (auto& f : g_customFns) { RegFn(f.name, f.fn); Log("[reg] '%s'", f.name); }
}

// ----- glue register loop detour (reimplemented loop; see notes in offsets.h) -
typedef void(__cdecl* t_regfn)(const char*, lua_CFunction);
static void __cdecl Detour_GlueRegister() {
    auto RegFn = (t_regfn)Off::FrameScript_RegisterFunction;
    for (uintptr_t off = 0; off < Off::GlueFuncTableBytes; off += 8)
        RegFn(*(const char**)(Off::GlueFuncTable + off), *(lua_CFunction*)(Off::GlueFuncTable + off + 4));
    Log("[hook] re-ran %u builtin glue registrations; adding custom...", (unsigned)(Off::GlueFuncTableBytes/8));
    RegisterAllCustomFns();
    if (!g_glueScript.empty()) {
        Log("[glue] executing glue script in glue state");
        ExecGlue(g_glueScript.c_str(), "ebonhold_glue.lua");
    }
}
static bool InstallGlueDetour() {
    BYTE* t = (BYTE*)Off::GlueRegisterLoop; DWORD op;
    // W^X: write with PAGE_READWRITE (writable, NOT executable), then restore the
    // original protection (PAGE_EXECUTE_READ). The page is never simultaneously
    // writable+executable, so the PAGE_EXECUTE_READWRITE (RWX) constant — a top
    // static heuristic flag — never appears in the binary. Safe here because Init
    // runs before the game ever executes this code path.
    VirtualProtect(t, 5, PAGE_READWRITE, &op);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((BYTE*)&Detour_GlueRegister - (t + 5));
    VirtualProtect(t, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    Log("[hook] glue detour installed at 0x%p", (void*)t);
    return true;
}

// ---- in-game custom-fn registration -----------------------------------------
// FrameScript_RegisterFunction registers into the ACTIVE FrameScript lua_State
// (*(void**)0x00D3F78C), which the client DESTROYS and RE-CREATES when switching
// between the glue (login) and in-game states. The glue detour above only
// populates the glue state, so custom fns are absent in-game (ExtBank* == nil).
//
// Fix: prefix-detour FrameScript_Execute (0x00819210) — it runs in whatever state
// is active — and, whenever the active-state pointer changes, re-register all
// custom fns into the new state. This transparently covers the in-game state.
// Stolen prologue (11 bytes, through the tick-counter add):
//   55 8B EC 51 83 05 A0 13 D4 00 01  -> resume at 0x0081921B.
static void*      g_lastLuaState  = nullptr;
static void*      g_execResumePtr = (void*)0x0081921B;         // FrameScript_Execute + 11
static uint32_t** const kpFSTick  = (uint32_t**)0x00D413A0;    // tick counter (replicated add)
static void** const kpActiveState = (void**)0x00D3F78C;        // active lua_State*

static void __cdecl RegisterCustomFnsIntoActiveState() { RegisterAllCustomFns(); }

static __declspec(naked) void Detour_FSExecute() {
    __asm {
        pushad
        pushfd
        // replicate the clobbered `add [0x00D413A0], 1` (protected; eax restored by popad)
        mov  eax, kpFSTick
        add  dword ptr [eax], 1
        // if the active lua_State changed, (re)register our custom fns into it
        mov  ecx, kpActiveState
        mov  eax, [ecx]
        test eax, eax
        jz   skip_reg
        cmp  eax, g_lastLuaState
        je   skip_reg
        mov  g_lastLuaState, eax
        call RegisterCustomFnsIntoActiveState
    skip_reg:
        popfd
        popad
        // stolen prologue of FrameScript_Execute (push ebp; mov ebp,esp; push ecx)
        push ebp
        mov  ebp, esp
        push ecx
        jmp  dword ptr [g_execResumePtr]      // -> 0x0081921B (after the replicated add)
    }
}
static bool InstallExecuteDetour() {
    BYTE* t = (BYTE*)Off::FrameScript_Execute; DWORD op;   // 0x00819210
    VirtualProtect(t, 11, PAGE_READWRITE, &op);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((BYTE*)&Detour_FSExecute - (t + 5));
    for (int i = 5; i < 11; ++i) t[i] = 0x90;             // pad the tail of the stolen region
    VirtualProtect(t, 11, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 11);
    Log("[hook] FrameScript_Execute detour installed at 0x%p (in-game custom-fn registration)", (void*)t);
    return true;
}

// ---------------------------------------------------------------- entry ------
static DWORD WINAPI Init(LPVOID) {
    Log("=========================================================");
    Log("[init] ebonhold.dll loaded (framework + patch check + embedded glue)");
    LoadGlueScript();      // loose override, else embedded
    InstallGlueDetour();   // registers custom fns + runs the glue script in the glue state
    InstallExecuteDetour();// re-registers custom fns into the IN-GAME lua_State (ExtBank* callable in world)
    ExtBankClient::InstallRecvDetour();  // registers SMSG_EXTBANK_UPDATE handler once in world
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        Log("[dllmain] attach (hMod=%p)", (void*)h);
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl ebonhold_load() {}
