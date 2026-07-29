// questpoi_client.h — force a quest POI query, header-only, #included by dllmain.cpp.
//
// Problem: the 3.3.5 client only sends CMSG_QUEST_POI_QUERY from its C-side
// world-map-open path — no Lua API triggers it. So quest POI data (pin for a
// freshly accepted quest, turn-in position once a quest completes) only
// refreshes when the player opens the map. The addon's quest tracker
// (super_tracker.lua) needs it on demand.
//
// Fix: expose EbonholdRequestQuestPOI(id1, id2, ...) to Lua. It builds the
// STOCK packet and sends it through the game's own send path (same recipe as
// extbank_client.h). The response, SMSG_QUEST_POI_QUERY_RESPONSE, is a stock
// opcode: the client's own handler parses it into the POI cache and fires
// QUEST_POI_UPDATE — no recv handler needed here, and the addon already
// listens for that event.
//
// Wire format (matches TrinityCore 3.3.5 HandleQuestPoiQuery):
//   CMSG_QUEST_POI_QUERY (0x01E3): uint32 count, then count x uint32 questId
//   (count is capped server-side at MAX_QUEST_LOG_SIZE = 25).
#pragma once
#include <windows.h>
#include <cstdint>
#include "offsets.h"           // Off:: Lua C API
#include "extbank_offsets.h"   // Off:: send-path offsets (verified for this Wow.exe)
#include "extbank_client.h"    // ExtBankClient::EBLog (log bridge into ebonhold.log)

namespace QuestPOIClient {

enum {
    CMSG_QUEST_POI_QUERY = 0x01E3,   // stock 3.3.5 opcode
    MAX_POI_QUERY_QUESTS = 25,       // MAX_QUEST_LOG_SIZE
};

// Client-side rate limit, independent of how often the addon calls us: the
// addon polls every 2s, but there is no reason to hit the server harder than
// this even if a future addon change misbehaves.
enum { MIN_QUERY_INTERVAL_MS = 2000 };

static inline double QP_ToNumber(void* L, int i) { return ((double(__cdecl*)(void*,int))Off::lua_tonumber)(L,i); }
static inline int    QP_IsNumber(void* L, int i) { return ((int(__cdecl*)(void*,int))Off::lua_isnumber)(L,i)!=0; }

// Same fixed-buffer CDataStore recipe as ExtBankClient::SendMsg, with a buffer
// sized for the worst case here: opcode + count + 25 ids = 108 bytes (the
// ExtBank helper's 64-byte buffer is too small, hence a local copy).
static bool SendPOIQuery(const uint32_t* questIds, int count) {
    if (!*(void**)Off::p_NetClient) {
        ExtBankClient::EBLog("[questpoi] send skipped: NetClient not ready");
        return false;
    }
    struct CDataStore {
        void*    vtbl;
        uint8_t* buf;
        uint32_t base, alloc, write, read;
    };
    uint8_t storage[160];
    CDataStore cds;
    cds.vtbl  = (void*)Off::CDataStore_vtable;
    cds.buf   = storage;
    cds.base  = 0;
    cds.alloc = sizeof(storage);
    cds.write = 0;
    cds.read  = 0;

    typedef void (__thiscall* tPutInt32)(void* self, uint32_t val);
    tPutInt32 PutInt32 = (tPutInt32)Off::CDataStore_PutInt32;
    PutInt32(&cds, CMSG_QUEST_POI_QUERY);
    PutInt32(&cds, (uint32_t)count);
    for (int i = 0; i < count; ++i) PutInt32(&cds, questIds[i]);
    ((void(__cdecl*)(void*))Off::SendServerPacket)(&cds);
    return true;
}

// EbonholdRequestQuestPOI(id1, id2, ...) — up to 25 numeric quest ids (the
// addon reads them from the quest log). Returns nothing; the refreshed data
// announces itself through the stock QUEST_POI_UPDATE event.
static int __cdecl Lua_RequestQuestPOI(void* L) {
    static DWORD lastSent = 0;
    DWORD now = GetTickCount();
    if (lastSent && (now - lastSent) < MIN_QUERY_INTERVAL_MS) return 0;

    uint32_t ids[MAX_POI_QUERY_QUESTS];
    int count = 0;
    for (int i = 1; i <= MAX_POI_QUERY_QUESTS && QP_IsNumber(L, i); ++i) {
        uint32_t id = (uint32_t)QP_ToNumber(L, i);
        if (id > 0) ids[count++] = id;
    }
    if (count == 0) return 0;

    if (SendPOIQuery(ids, count)) lastSent = now;
    return 0;
}

} // namespace QuestPOIClient
