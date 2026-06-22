// offsets.h — addresses for the ebonhold Wow.exe (3.3.5a, ImageBase 0x400000).
// All values below were extracted by static analysis of this exact Wow.exe.
#pragma once
#include <cstdint>

namespace Off
{
    // Glue Lua C-function registrar: __cdecl(const char* name, lua_CFunction fn)
    constexpr uintptr_t FrameScript_RegisterFunction = 0x00817F90;

    // The routine that registers the 113 built-in glue functions (our detour point).
    // Its loop top is at +3 and the loop back-edge jumps there, so we must NOT
    // trampoline-steal its bytes — the detour reimplements the loop instead.
    constexpr uintptr_t GlueRegisterLoop             = 0x004DD580;
    constexpr uintptr_t GlueFuncTable                = 0x00AC3E00; // {const char* name, lua_CFunction fn}[]
    constexpr uintptr_t GlueFuncTableBytes           = 0x388;      // 113 entries * 8 bytes

    // Glue lua_State*  (read as *(void**)pGlueLuaState)
    constexpr uintptr_t pGlueLuaState                = 0x00D3F78C;

    // Runs a Lua string in the current (glue) state. __cdecl(code, source, 0).
    constexpr uintptr_t FrameScript_Execute          = 0x00819210;

    // Lua 5.1 C API (all __cdecl, L is a normal stack arg in this build)
    constexpr uintptr_t lua_tolstring                = 0x0084E0E0; // const char*(L,idx,size_t* len)
    constexpr uintptr_t lua_isstring                 = 0x0084DF60; // int (L,idx)
    constexpr uintptr_t lua_isnumber                 = 0x0084DF20; // int (L,idx)
    constexpr uintptr_t lua_tonumber                 = 0x0084E030; // double (L,idx)
    constexpr uintptr_t lua_pushstring               = 0x0084E350; // void (L,const char*)
    constexpr uintptr_t lua_pushcclosure             = 0x0084E400; // void (L,fn,nups)
    constexpr uintptr_t luaL_error                   = 0x0084F280; // int (L,fmt,...)
}
