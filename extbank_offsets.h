// extbank_offsets.h — client offsets ExtBank needs on top of the base offsets.h.
// Target: ebonhold Wow.exe, stock 3.3.5a **build 12340**, ImageBase 0x400000.
// All values below were extracted AND verified by disassembly of THIS exact
// Wow.exe (see extbank/client/RE_NOTES.md for the full derivation).
#pragma once
#include <cstdint>

namespace Off
{
    // ============================ SEND (verified) ============================
    // Global send: void __cdecl SendServerPacket(CDataStore* msg). Uses the
    // NetClient at *(void**)0xC79CF4 and calls NetClient::Send; applies the ARC4
    // header encryption when in-world. Simplest send path — call this directly.
    constexpr uintptr_t SendServerPacket     = 0x006B0B50;
    // NetClient::Send(CDataStore*) __thiscall — the funnel SendServerPacket calls
    // (kept for reference; not needed directly).
    constexpr uintptr_t NetClient_Send       = 0x00632B50;
    // *(NetClient**)0xC79CF4 — the live world NetClient instance (0 until connected).
    constexpr uintptr_t p_NetClient          = 0x00C79CF4;

    // CDataStore::PutInt32(uint32) __thiscall(this=ecx, val) — appends 4 bytes,
    // advances write pos. Used to build the outgoing message (opcode + payload).
    constexpr uintptr_t CDataStore_PutInt32  = 0x0047B0A0;

    // Default CDataStore allocator vtable (set at CDataStore+0x00). Only used if
    // the buffer must grow; with a caller-provided fixed buffer it's never
    // dereferenced, but set it anyway for safety.
    constexpr uintptr_t CDataStore_vtable    = 0x009E0E24;

    // CDataStore field layout (verified from PutInt32 0x47b0a0 / GetInt16 0x47b380):
    //   +0x00 vtable, +0x04 buffer, +0x08 base, +0x0C alloc,
    //   +0x10 writePos (m_size), +0x14 readPos (m_read)
    //   write addr = (*(u8**)(cds+4) - *(u32*)(cds+8)) + writePos
    //   send length = writePos - readPos ;  read addr uses readPos likewise
    constexpr uintptr_t CDS_VTBL  = 0x00;
    constexpr uintptr_t CDS_BUF   = 0x04;
    constexpr uintptr_t CDS_BASE  = 0x08;
    constexpr uintptr_t CDS_ALLOC = 0x0C;
    constexpr uintptr_t CDS_WRITE = 0x10;
    constexpr uintptr_t CDS_READ  = 0x14;

    // ============================ RECV (verified) ============================
    // NetClient::SetMessageHandler(opcode, fn, param) __thiscall — installs a
    // handler: [this+0x53C + op*4] = fn ; [this+0x19B8 + op*4] = param.
    // Register our SMSG handler: SetMessageHandler(*p_NetClient, SMSG_EXTBANK_UPDATE, h, 0).
    constexpr uintptr_t NetClient_SetHandler = 0x00631FA0;
    constexpr uintptr_t NetClient_HandlerTbl = 0x53C;    // this-relative fn table
    constexpr uintptr_t NetClient_ParamTbl   = 0x19B8;   // this-relative param table
    constexpr uintptr_t NetClient_MaxOpcode  = 0x51F;    // dispatch guard (op must be <)
    // Dispatched handler ABI (cdecl): void h(void* param, int opcode, void* arg1,
    // void* cds). On entry cds's readPos is already past the 4-byte... (2-byte)
    // opcode, so payload reads start at readPos.
    constexpr uintptr_t NetClient_Dispatch   = 0x00631FE0; // (reference; not hooked)
    constexpr uintptr_t CDataStore_GetInt16  = 0x0047B380; // (cds,&out_u16) reader

    inline bool ExtBank_Ready() {
        return true; // all offsets verified for this 12340 Wow.exe
    }
}
