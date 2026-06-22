#pragma once
// AUTO-GENERATED from ebonhold_glue.lua by gen_glue.ps1 - do not edit.
static const char kGlueScript[] = R"EBONHOLD(
-- ebonhold_glue.lua — SOURCE for the glue script that ebonhold.dll runs in the
-- GLUE Lua state. build.bat embeds this into the DLL (glue_script.h) via
-- gen_glue.ps1. A loose ebonhold_glue.lua placed next to Wow.exe overrides the
-- embedded copy at runtime (handy for tweaking without a rebuild).
--
-- It runs EARLY (during glue C-function registration), so it waits until the
-- character-select GlueXML symbols exist, then:
--   * runs CheckPatches(API_URL, realm) when char-select shows
--   * disables "Enter World" and blocks login until the check passes
--   * pops an update dialog (and keeps login blocked) if patches are outdated
--   * confirms before connecting to a PTR realm.

local API_URL       = "https://api.project-ebonhold.com/api/launcher/file-hashes?server_name="
local POLL_INTERVAL = 0.2
local FAIL_CLOSED   = false    -- true = block login if the API can't be reached / errors

local function log(m) if EbonholdLog then EbonholdLog(m) end end
log("ebonhold_glue.lua: running")

-- ------------------------------------------------------------- the gate -----
local function InstallGate()
    log("ebonhold_glue.lua: installing patch gate")

    local function GetRealm()
        if GetServerName then return GetServerName() or "" end
        if GetRealmName  then return GetRealmName()  or "" end
        return "Unknown"
    end

    local state, checking, elapsed = "pending", false, 0
    local origEnterWorld, wantEnterWorld = nil, false   -- check runs on Play; proceed when it passes
    local poller = CreateFrame("Frame")

    local function SetEnterWorldEnabled(on)
        if CharSelectEnterWorldButton then
            if on then CharSelectEnterWorldButton:Enable() else CharSelectEnterWorldButton:Disable() end
        end
    end

    local GUIDE_URL = "https://project-ebonhold.com/support/installation-guide"
    local blockedMsg, blockedKind = nil, nil   -- popup text + reason ("outdated"/"error")

    -- Custom glue dialog: "Open Installation Guide" opens the browser (via the
    -- DLL's EbonholdOpenURL); the second button quits the game.
    if GlueDialogTypes and not GlueDialogTypes["EBONHOLD_OUTDATED"] then
        GlueDialogTypes["EBONHOLD_OUTDATED"] = {
            text = "",
            button1 = "Open Installation Guide",
            button2 = "Quit the Game",
            escapeHides = false,
            OnAccept = function() if EbonholdOpenURL then EbonholdOpenURL(GUIDE_URL) end end,
            OnCancel = function() if QuitGame then QuitGame() else Quit() end end,
        }
    end

    local function ShowBlocked()
        if not blockedMsg then return end
        if blockedKind == "outdated" and EbonholdOpenURL and GlueDialog_Show
           and GlueDialogTypes and GlueDialogTypes["EBONHOLD_OUTDATED"] then
            GlueDialog_Show("EBONHOLD_OUTDATED", blockedMsg)   -- "Open Installation Guide" button
        elseif GlueDialog_Show then
            GlueDialog_Show("OKAY", blockedMsg)
        else
            message(blockedMsg)
        end
    end

    local OUTDATED_NORMAL =
        "Your game installation is not up to date.\n\n" ..
        "If you use the launcher, just restart your game with it and the update will start automatically.\n\n" ..
        "If you installed manually, re-download the required files from the website, " ..
        "clear your cache, and restart the game."

    local OUTDATED_PTR =
        "You are trying to connect to the Public Test Realm, but your game is not up to date.\n\n" ..
        "The PTR can only be updated with the launcher. Please close the game, run the launcher " ..
        "to download the latest test build, then reconnect."

    local function BuildOutdatedMsg(list)
        local realm = GetRealm()
        local isPTR = realm and string.find(string.upper(realm), "(PTR)", 1, true) ~= nil
        if isPTR then
            return OUTDATED_PTR
        end
        local msg = OUTDATED_NORMAL
        if list and list ~= "" then
            msg = msg .. "\n\nFiles to update: " .. string.gsub(list, ",", ", ")
        end
        return msg
    end

    local function StartCheck()
        if not CheckPatches or checking then return end
        state, checking, elapsed = "pending", true, 0
        SetEnterWorldEnabled(false)
        log("patch check: realm="..GetRealm())
        CheckPatches(API_URL, GetRealm())
        poller:Show()
    end

    poller:SetScript("OnUpdate", function(self, dt)
        if not checking then self:Hide(); return end
        elapsed = elapsed + (dt or 0)
        if elapsed < POLL_INTERVAL then return end
        elapsed = 0
        local res = GetPatchCheckResult and GetPatchCheckResult()
        if not res then return end
        checking = false
        self:Hide()
        log("patch check result: "..res)
        if res == "OK" then
            state = "ok"; blockedMsg = nil; blockedKind = nil; SetEnterWorldEnabled(true); log("login ENABLED (patches OK)")
            if wantEnterWorld then wantEnterWorld = false; if origEnterWorld then origEnterWorld() end end
        elseif string.sub(res, 1, 9) == "OUTDATED:" then
            wantEnterWorld = false
            local list = string.sub(res, 10)
            state = "blocked"; blockedKind = "outdated"; blockedMsg = BuildOutdatedMsg(list); SetEnterWorldEnabled(false); ShowBlocked()
            log("login BLOCKED (outdated: "..list..")")
        else -- ERROR:...
            if FAIL_CLOSED then
                wantEnterWorld = false
                state = "blocked"; blockedKind = "error"
                blockedMsg = "Could not verify your game files. Please check your connection and try again later."
                SetEnterWorldEnabled(false); ShowBlocked()
                log("login BLOCKED (fail-closed: "..res..")")
            else
                state = "ok"; blockedMsg = nil; SetEnterWorldEnabled(true); log("login ENABLED (fail-open despite "..res..")")
                if wantEnterWorld then wantEnterWorld = false; if origEnterWorld then origEnterWorld() end end
            end
        end
    end)

    -- (PTR realm confirmation is handled directly in the client's GlueXML.)

    -- The check runs ONLY when the player presses Play (Enter World) — not when
    -- the character-select screen appears. On the first Play press we start the
    -- check and block; when it passes we enter the world automatically.
    if type(CharacterSelect_EnterWorld) == "function" then
        origEnterWorld = CharacterSelect_EnterWorld
        CharacterSelect_EnterWorld = function(...)
            if state == "ok" then return origEnterWorld(...) end
            wantEnterWorld = true
            if state == "blocked" then ShowBlocked(); return end   -- already known outdated
            if not checking then StartCheck() end                   -- kick off the check now
            return                                                   -- block until it returns
        end
    end
end

-- --------------------------------------------- defer until glue UI loaded ---
-- We run before the char-select GlueXML is loaded, so wait for its symbols.
if not CreateFrame then
    log("ebonhold_glue.lua: ERROR CreateFrame missing at boot")
    return
end
local boot = CreateFrame("Frame")
boot:SetScript("OnUpdate", function(self)
    if type(CharacterSelect_EnterWorld) == "function" and hooksecurefunc then
        self:SetScript("OnUpdate", nil)
        InstallGate()
    end
end)
log("ebonhold_glue.lua: boot frame armed")

)EBONHOLD";
