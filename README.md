# Ebonhold Client Mod

Client-side mod for the **Ebonhold**

A native DLL injected into `Wow.exe` via import-table patching — it auto-loads with the
client, no launcher hook required. It registers custom Lua C-functions into the GlueXML
(login / character-select) environment, verifies the installed game data against the
server's hash API before allowing login, and exposes a few helpers (open URL, logging).

Ships with an embedded glue Lua script and a small PE patcher utility that rewrites the
client's import table.

---

## Features

- **Static DLL injection** — `ebonhold_applymod.exe` appends an `.inj` section to `Wow.exe`
  with an extended import descriptor for `ebonhold.dll`, so the DLL loads automatically.
- **Generic glue Lua C-function registry** — hooks the GlueXML registration loop and exposes
  custom functions callable from char-select / char-create XML.
- **Patch-integrity gate** — on *Play*, hashes `Data\patch-4/5/6.MPQ` (MD5 → base64) and
  compares them against the server API; blocks login (and shows an update dialog) if the
  client is out of date. PTR realms get a tailored message.
- **Browser helper** — `EbonholdOpenURL(url)` opens a link in the default browser.
- **Embedded glue script** — `ebonhold_glue.lua` is baked into the DLL at build time; a loose
  `ebonhold_glue.lua` next to `Wow.exe` overrides it for development.

## Exposed Lua functions (GlueXML)

| Function | Description |
|---|---|
| `CheckPatches(apiUrl, realmName)` | Starts an async patch-hash check against the API. |
| `GetPatchCheckResult()` | Returns `nil` while running, then `"OK"`, `"OUTDATED:<files>"`, or `"ERROR:<reason>"`. |
| `EbonholdOpenURL(url)` | Opens an `http(s)` URL in the default browser. |
| `EbonholdLog(msg)` | Writes a line to `ebonhold.log`. |

## Patch-check protocol

The DLL requests:

```
GET https://api.project-ebonhold.com/api/launcher/file-hashes?server_name=<realm>
```

and expects:

```json
{
  "slug": "roguelike",
  "name": "Rogue-Lite (PTR)",
  "patches": {
    "patch-4": "<base64-md5>",
    "patch-5": "<base64-md5>",
    "patch-6": "<base64-md5>"
  }
}
```

It hashes only `Data\patch-4.MPQ`, `Data\patch-5.MPQ`, `Data\patch-6.MPQ` (full-file MD5,
base64-encoded) and compares each to the matching key. Any mismatch → `OUTDATED`; a missing
local file or absent server hash → `ERROR`.

---

## Repository layout

| File | Purpose |
|---|---|
| `dllmain.cpp` | The mod: glue-registration detour, patch check, Lua bindings. |
| `patcher.cpp` | Builds `ebonhold_applymod.exe`, which patches the client's import table. |
| `offsets.h` | Confirmed addresses/ABI for this exact `Wow.exe` build. |
| `ebonhold_glue.lua` | Source for the embedded glue script (the login gate UI/logic). |
| `gen_glue.ps1` | Embeds `ebonhold_glue.lua` into `glue_script.h` as a C string. |
| `glue_script.h` | **Generated** — do not edit by hand. |
| `build.bat` | One-shot build: generate → compile DLL → compile patcher → stage → patch. |

---

## Building

Requires the **Visual Studio x86 toolchain** (the build targets a 32-bit client).

```bat
build.bat
```

Steps performed:

1. `gen_glue.ps1` embeds `ebonhold_glue.lua` → `glue_script.h`.
2. Compile `ebonhold.dll` (`cl /LD /MT /Od /EHsc … /MACHINE:X86`).
3. Compile `ebonhold_applymod.exe` (`cl /O2 /MT /EHsc … /MANIFESTUAC:level='asInvoker'`).
4. Stage `ebonhold.dll` next to `Wow.exe`.
5. Patch `Wow.exe` → `Wow_patched.exe`.

> **Note:** the DLL is built `/Od` because the VS preview compiler ICEs on `/O2` for `dllmain.cpp`.

## Installing

1. Place `ebonhold.dll` in your WoW directory (next to `Wow.exe`).
2. Run `Wow_patched.exe` (or replace `Wow.exe` with it).

The DLL reads patches from `<WoW>\Data\` and writes diagnostics to `ebonhold.log`.

---

## Code signing (optional)

The binaries can be Authenticode-signed with the project's code-signing certificate. If the
cert is installed in the Windows store, sign by thumbprint (no password needed):

```powershell
$st = "C:\Program Files (x86)\Windows Kits\10\bin\<sdk>\x64\signtool.exe"
& $st sign /sha1 <THUMBPRINT> /fd sha256 /tr http://timestamp.digicert.com/ /td sha256 ebonhold.dll ebonhold_applymod.exe
```

> A self-signed certificate proves integrity but will **not** clear SmartScreen / AV
> detections. A CA-issued (OV/EV) code-signing certificate is required for that.

---

## Notes

- 32-bit x86 only — addresses in `offsets.h` are specific to one `Wow.exe` build and will not
  match a different client binary.
- The glue-registration detour is a **full replace**, not a trampoline (the target loop's
  back-edge re-enters its own entry bytes).
- This is a client-side mod for a private server. Use only with content you own/operate.
