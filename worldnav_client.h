// worldnav_client.h — world->screen projection + local player position,
// header-only, #included by dllmain.cpp. Powers the Dragonflight-style quest
// tracker's super-tracked POI pin (screen-edge clamping when the objective is
// outside the camera's field of view).
//
// No detours needed — both functions are plain reads of live client state and
// only ever run on the main thread (called from FrameScript handlers):
//   * camera chain: WorldFrame* at 0x00B7436C, active CGCamera* at +0x7E20;
//     CGCamera: pos +0x08 (3 floats), facing 3x3 row-major +0x14,
//     nearZ +0x38, farZ +0x3C, fov +0x40, aspect +0x44 (w/h).
//   * player: ClntObjMgr::GetActivePlayerObj (0x004038F0, __cdecl) -> CGUnit*;
//     position at +0x798 (x,y,z), facing (radians) at +0x7A4.
// These are the long-established, widely verified 3.3.5a build-12340 offsets;
// same client family as the rest of this DLL (FrameScript_* in offsets.h).
//
// Lua C functions exposed (registered in dllmain.cpp, available in-game via the
// Execute detour):
//   EbonholdWorldToScreen(wx, wy, wz)
//     -> ndcX, ndcY, depth, dist   | nil when no camera (loading/glue screen)
//     ndcX/ndcY are normalized device coords: on-screen iff depth > 0 and both
//     within [-1, 1]. depth = distance along the camera forward axis (yards,
//     negative = behind the camera). dist = straight-line camera distance.
//     UI coords: sx = (ndcX+1)/2 * width ; sy = (ndcY+1)/2 * height (bottom-up).
//   EbonholdGetPlayerPosition()
//     -> x, y, z, facing           | nil when not in world
#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>
#include "offsets.h"

namespace WorldNav {

// ------------------------------------------------------------------ offsets ---
constexpr uintptr_t pWorldFrame        = 0x00B7436C;  // CGWorldFrame**
constexpr uintptr_t CameraPtrOffset    = 0x7E20;      // WorldFrame + off -> CGCamera*
constexpr uintptr_t Cam_Pos            = 0x08;        // float[3]
constexpr uintptr_t Cam_Matrix         = 0x14;        // float[3][3] row-major
constexpr uintptr_t Cam_Fov            = 0x40;        // float (radians)
constexpr uintptr_t Cam_Aspect         = 0x44;        // float (width / height)
constexpr uintptr_t GetActivePlayerObj = 0x004038F0;  // CGUnit* __cdecl ()
constexpr uintptr_t Unit_Pos           = 0x798;       // float[3] in CGUnit
constexpr uintptr_t Unit_Facing        = 0x7A4;       // float (radians)

// The client's projection uses a slightly narrower horizontal FOV than the raw
// camera value; 44/45 is the classic, community-verified correction for 12340.
constexpr float kFovCorrection = 44.0f / 45.0f;

// --------------------------------------------------------- Lua stack helpers --
static inline double WN_ToNumber(void* L, int i)  { return ((double(__cdecl*)(void*,int))Off::lua_tonumber)(L,i); }
static inline int    WN_IsNumber(void* L, int i)  { return ((int(__cdecl*)(void*,int))Off::lua_isnumber)(L,i); }
static inline void   WN_PushNumber(void* L, double v) { ((void(__cdecl*)(void*,double))Off::lua_pushnumber)(L,v); }
static inline void   WN_PushNil(void* L)          { ((void(__cdecl*)(void*))Off::lua_pushnil)(L); }
static inline int    WN_Err(void* L, const char* m){ return ((int(__cdecl*)(void*,const char*,...))Off::luaL_error)(L,"%s",m); }

static inline const float* ActiveCamera() {
    uintptr_t wf = *(uintptr_t*)pWorldFrame;
    if (wf < 0x10000) return nullptr;
    uintptr_t cam = *(uintptr_t*)(wf + CameraPtrOffset);
    if (cam < 0x10000) return nullptr;
    return (const float*)cam;
}

// EbonholdWorldToScreen(wx, wy, wz) -> ndcX, ndcY, depth, dist | nil
static int __cdecl Lua_WorldToScreen(void* L) {
    if (!WN_IsNumber(L,1) || !WN_IsNumber(L,2) || !WN_IsNumber(L,3))
        return WN_Err(L, "Usage: EbonholdWorldToScreen(x, y, z)");
    const float* cam = ActiveCamera();
    if (!cam) { WN_PushNil(L); return 1; }

    const float* pos = (const float*)((uintptr_t)cam + Cam_Pos);
    const float* m   = (const float*)((uintptr_t)cam + Cam_Matrix);
    float fov    = *(const float*)((uintptr_t)cam + Cam_Fov);
    float aspect = *(const float*)((uintptr_t)cam + Cam_Aspect);
    if (fov <= 0.01f || fov >= 3.1f) { WN_PushNil(L); return 1; }
    if (aspect < 0.1f || aspect > 10.0f) aspect = 4.0f / 3.0f;

    float dx = (float)WN_ToNumber(L,1) - pos[0];
    float dy = (float)WN_ToNumber(L,2) - pos[1];
    float dz = (float)WN_ToNumber(L,3) - pos[2];

    // camera space: rows of the facing matrix are the camera basis vectors
    // (row0 = forward, row1 = left, row2 = up), so dot(rel, row_i) projects.
    float fwd  =   dx*m[0] + dy*m[1] + dz*m[2];
    float right = -(dx*m[3] + dy*m[4] + dz*m[5]);
    float up    =   dx*m[6] + dy*m[7] + dz*m[8];

    float tanX = tanf(fov * kFovCorrection * 0.5f);
    float tanY = tanX / aspect;
    float d = fwd;
    if (d > -0.001f && d < 0.001f) d = (d < 0) ? -0.001f : 0.001f;  // avoid /0 at 90° sideways

    WN_PushNumber(L, right / (d * tanX));
    WN_PushNumber(L, up    / (d * tanY));
    WN_PushNumber(L, fwd);
    WN_PushNumber(L, sqrtf(dx*dx + dy*dy + dz*dz));
    return 4;
}

// EbonholdGetPlayerPosition() -> x, y, z, facing | nil
static int __cdecl Lua_GetPlayerPosition(void* L) {
    uintptr_t unit = ((uintptr_t(__cdecl*)())GetActivePlayerObj)();
    if (unit < 0x10000) { WN_PushNil(L); return 1; }
    const float* p = (const float*)(unit + Unit_Pos);
    WN_PushNumber(L, p[0]);
    WN_PushNumber(L, p[1]);
    WN_PushNumber(L, p[2]);
    WN_PushNumber(L, *(const float*)(unit + Unit_Facing));
    return 4;
}

} // namespace WorldNav
