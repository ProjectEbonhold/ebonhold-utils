// extbank_client.h — ExtBank client half, header-only, #included by dllmain.cpp.
//
// All offsets are verified against this exact ebonhold Wow.exe (3.3.5a 12340) —
// see extbank_offsets.h + RE_NOTES.md. No detours/trampolines needed:
//   * SEND: build a CDataStore (fixed buffer, no heap grow) and call the game's
//     global SendServerPacket (0x6B0B50), which routes through NetClient::Send
//     and applies ARC4 header encryption.
//   * RECV: register a handler in the NetClient handler table via
//     NetClient::SetMessageHandler (0x631FA0) for SMSG_EXTBANK_UPDATE; the game's
//     own dispatch then calls us with the CDataStore positioned past the opcode.
//
// Lua C functions exposed to the in-game UI (registered in dllmain.cpp):
//   ExtBankOpen(), ExtBankMove(sBag,sSlot,dBag,dSlot,count), ExtBankUnlock()
// Server -> AddOn: ExtBank_OnPacket('<hex>') (called here from the recv handler).
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "extbank_offsets.h"

namespace ExtBankClient {

// ------------------------------------------------------------------ opcodes ---
// Must match the server's Opcodes.h. Placed past every stock 3.3.5 opcode (old
// NUM_MSG_TYPES=0x513) so there is no collision; SMSG stays < 0x51F so the client
// handler table (0x51F entries) still accepts it, and that slot is empty in the
// stock client (nothing above ~0x4FF is registered).
enum {
    CMSG_EXTBANK_OPEN   = 0x0513,
    CMSG_EXTBANK_MOVE   = 0x0514,
    SMSG_EXTBANK_UPDATE = 0x0515,   // < 0x51F (dispatch guard) — OK
    CMSG_EXTBANK_UNLOCK = 0x0516,
};

// --------------------------------------------------------- Lua stack helpers --
static inline double Lua_ToNumber(void* L, int i) { return ((double(__cdecl*)(void*,int))Off::lua_tonumber)(L,i); }
static inline int    Lua_IsNumber(void* L, int i) { return ((int(__cdecl*)(void*,int))Off::lua_isnumber)(L,i); }
static inline int    Lua_Err(void* L, const char* m){ return ((int(__cdecl*)(void*,const char*,...))Off::luaL_error)(L,"%s",m); }
static inline void   ExecInGame(const char* code, const char* src) {
    ((void(__cdecl*)(const char*,const char*,int))Off::FrameScript_Execute)(code, src, 0);
}
void EBLog(const char* fmt, ...);   // defined in dllmain.cpp

// ------------------------------------------------------------- CDataStore -----
// Minimal mirror of the game's CDataStore. With a caller-provided fixed buffer
// (base=0, alloc=sizeof) the game's PutInt32 never needs to grow, so the
// allocator vtable is never dereferenced.
struct CDataStore {
    void*    vtbl;   // +0x00
    uint8_t* buf;    // +0x04
    uint32_t base;   // +0x08
    uint32_t alloc;  // +0x0C
    uint32_t write;  // +0x10
    uint32_t read;   // +0x14
};
typedef void (__thiscall* tPutInt32)(void* self, uint32_t val);

// Build [opcode][words...] and send it. words may be null when nwords==0.
static bool SendMsg(uint32_t opcode, const uint32_t* words, int nwords) {
    if (!*(void**)Off::p_NetClient) {          // not connected / not in world yet
        EBLog("[extbank] send skipped: NetClient not ready (opcode %#x)", opcode);
        return false;
    }
    uint8_t storage[64];
    CDataStore cds;
    cds.vtbl  = (void*)Off::CDataStore_vtable;
    cds.buf   = storage;
    cds.base  = 0;
    cds.alloc = sizeof(storage);
    cds.write = 0;
    cds.read  = 0;

    tPutInt32 PutInt32 = (tPutInt32)Off::CDataStore_PutInt32;
    PutInt32(&cds, opcode);
    for (int i = 0; i < nwords; ++i) PutInt32(&cds, words[i]);
    // read stays 0; NetClient::Send uses length = write - read.
    ((void(__cdecl*)(void*))Off::SendServerPacket)(&cds);
    return true;
}

// ----------------------------------------------------- outgoing Lua C funcs ---
static int __cdecl Lua_ExtBankOpen(void* L);   // fwd (registers handler first)

static int __cdecl Lua_ExtBankMove(void* L) {
    for (int i = 1; i <= 5; ++i)
        if (!Lua_IsNumber(L, i))
            return Lua_Err(L, "Usage: ExtBankMove(srcBag,srcSlot,dstBag,dstSlot,count)");
    uint32_t w0 = ((uint32_t)(uint8_t)Lua_ToNumber(L,1))
                | ((uint32_t)(uint8_t)Lua_ToNumber(L,2) << 8)
                | ((uint32_t)(uint8_t)Lua_ToNumber(L,3) << 16)
                | ((uint32_t)(uint8_t)Lua_ToNumber(L,4) << 24);
    uint32_t w1 = (uint32_t)Lua_ToNumber(L, 5);
    uint32_t words[2] = { w0, w1 };
    SendMsg(CMSG_EXTBANK_MOVE, words, 2);
    return 0;
}

static int __cdecl Lua_ExtBankUnlock(void* L) { SendMsg(CMSG_EXTBANK_UNLOCK, nullptr, 0); return 0; }

// ------------------------------------------------- incoming SMSG handler ------
// Read the payload from the CDataStore (cursor already past the opcode) and hand
// it to the AddOn as hex.
static void ForwardToLua(const uint8_t* data, uint32_t len) {
    static const char* hex = "0123456789abcdef";
    std::string call = "ExtBank_OnPacket('";
    call.reserve(len * 2 + 24);
    for (uint32_t i = 0; i < len; ++i) { call += hex[data[i] >> 4]; call += hex[data[i] & 15]; }
    call += "')";
    ExecInGame(call.c_str(), "extbank");
}

// Dispatched handler ABI (cdecl): h(param, opcode, arg1, CDataStore*).
static void __cdecl RecvHandler(void* /*param*/, int /*opcode*/, void* /*arg1*/, void* cdsv) {
    if (!cdsv) return;
    uint8_t* buf  = *(uint8_t**)((uint8_t*)cdsv + Off::CDS_BUF);
    uint32_t base = *(uint32_t*)((uint8_t*)cdsv + Off::CDS_BASE);
    uint32_t rd   = *(uint32_t*)((uint8_t*)cdsv + Off::CDS_READ);
    uint32_t wr   = *(uint32_t*)((uint8_t*)cdsv + Off::CDS_WRITE);
    if (!buf || wr <= rd) return;
    const uint8_t* p = buf - base + rd;
    ForwardToLua(p, wr - rd);
}

// ---- suppress native bank deposits while the ext bank is active -------------
// The AddOn's right-click deposit uses a sanctioned post-hook (hooksecurefunc),
// which runs AFTER Blizzard's default handler has already told the server to
// deposit into the NATIVE bank (CMSG_AUTOBANK_ITEM / CMSG_AUTOSTORE_BANK_ITEM).
// A secure post-hook cannot cancel that. So we drop those two CMSGs in
// NetClient::Send while the ext bank is active — the native deposit never
// reaches the server, and only the AddOn's ExtBankMove lands the item in the
// ext store. Toggled by ExtBankSetActive(1/0) and auto-set on ExtBankOpen.
enum { CMSG_AUTOSTORE_BANK_ITEM = 0x0282, CMSG_AUTOBANK_ITEM = 0x0283 };
static volatile long g_extBankActive = 0;
static void*         g_netSendResume = (void*)0x00632B56;   // NetClient::Send + 6 (after stolen prologue)

// Only a native-bank DEPOSIT (item coming FROM the player's live inventory) is
// redirected to the ext bank. Native-bank WITHDRAWALS (source is a bank slot) and
// every drag operation (CMSG_SWAP_ITEM) pass through untouched — so the native
// bank stays fully usable (drag in/out, right-click withdraw). A live-inventory
// position is the backpack (bag 255, slots 23..38) or an equipped bag (19..22);
// bank positions (255/39..66, bags 67..73) are NOT live inventory.
static bool ExtBank_IsLiveInvPos(uint8_t bag, uint8_t slot) {
    if (bag == 255) return slot >= 23 && slot < 39;   // backpack
    return bag >= 19 && bag < 23;                      // equipped bags
}
// Returns 1 to drop the outgoing packet (native deposit while ext bank is open).
static int __cdecl ExtBank_ShouldDropSend(void* cds) {
    if (!g_extBankActive || !cds) return 0;
    uint8_t* buf  = *(uint8_t**)((uint8_t*)cds + Off::CDS_BUF);
    uint32_t base = *(uint32_t*)((uint8_t*)cds + Off::CDS_BASE);
    if (!buf) return 0;
    const uint8_t* p = buf - base;                     // outgoing read cursor = 0
    uint32_t opcode = *(const uint32_t*)p;
    if (opcode != CMSG_AUTOSTORE_BANK_ITEM && opcode != CMSG_AUTOBANK_ITEM) return 0;
    return ExtBank_IsLiveInvPos(p[4], p[5]) ? 1 : 0;   // payload: u8 bag, u8 slot
}

static __declspec(naked) void Detour_NetSend() {
    __asm {
        // __thiscall: ecx = NetClient, [esp+4] = CDataStore*
        mov  eax, [esp+4]                 // cds
        push ecx                          // preserve this across the C call
        push eax                          // arg: cds
        call ExtBank_ShouldDropSend       // __cdecl -> eax = 1 to drop
        add  esp, 4                       // clean the pushed arg
        pop  ecx                          // restore this
        test eax, eax
        jnz  drop
    passthrough:
        // stolen prologue of NetClient::Send (push ebp; mov ebp,esp; push esi; mov esi,ecx)
        push ebp
        mov  ebp, esp
        push esi
        mov  esi, ecx
        jmp  dword ptr [g_netSendResume]  // -> 0x00632B56
    drop:
        ret  4                            // __thiscall: swallow the packet, clean 1 arg
    }
}
static bool InstallSendSuppressDetour() {
    if (!Off::NetClient_Send) return false;
    BYTE* t = (BYTE*)Off::NetClient_Send; DWORD op;   // 0x00632B50
    VirtualProtect(t, 6, PAGE_READWRITE, &op);
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((BYTE*)&Detour_NetSend - (t + 5));
    t[5] = 0x90;                                       // pad the 6th stolen byte
    VirtualProtect(t, 6, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 6);
    EBLog("[extbank] NetClient::Send suppress-detour installed at 0x%p", (void*)t);
    return true;
}

// ExtBankSetActive(on) — AddOn calls this to gate native-bank-deposit suppression.
static int __cdecl Lua_ExtBankSetActive(void* L) {
    long on = (Lua_IsNumber(L, 1) && Lua_ToNumber(L, 1) != 0) ? 1 : 0;
    InterlockedExchange(&g_extBankActive, on);
    return 0;
}

// Register our handler in the live NetClient's table. Idempotent; re-registers if
// the NetClient object was recreated (reconnect). Safe to call often.
typedef void (__thiscall* tSetHandler)(void* self, int opcode, void* fn, void* param);
static void* g_registeredOn = nullptr;
static void EnsureHandler() {
    void* nc = *(void**)Off::p_NetClient;
    if (!nc) return;
    ((tSetHandler)Off::NetClient_SetHandler)(nc, SMSG_EXTBANK_UPDATE, (void*)&RecvHandler, nullptr);
    if (nc != g_registeredOn) {
        EBLog("[extbank] recv handler registered on NetClient %p (opcode %#x)", nc, SMSG_EXTBANK_UPDATE);
        g_registeredOn = nc;
    }
}

static int __cdecl Lua_ExtBankOpen(void* L) {
    InterlockedExchange(&g_extBankActive, 1);   // suppress native bank deposits while open
    EnsureHandler();                            // make sure we receive the reply
    SendMsg(CMSG_EXTBANK_OPEN, nullptr, 0);
    return 0;
}

// Called once from dllmain Init(): installs the send-suppress detour and does a
// best-effort early recv-handler registration (also re-tried on every ExtBankOpen).
static void InstallRecvDetour() {         // name kept for the dllmain call site
    static bool once = false;
    if (!once) { InstallSendSuppressDetour(); once = true; }
    EnsureHandler();
}

} // namespace ExtBankClient
