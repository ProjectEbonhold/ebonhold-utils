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

-- Login-screen message (MOTD). Empty URL = feature disabled.
local MOTD_URL      = "https://api.project-ebonhold.com/api/launcher/login-message"
local MOTD_INTERVAL = 60       -- seconds between endpoint queries
local MOTD_POLL     = 0.2      -- polling of the async HTTP result

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

-- ------------------------------------------------ login-screen message ------
-- Queries MOTD_URL every MOTD_INTERVAL seconds while the LOGIN screen
-- (AccountLogin) is up. If the endpoint returns {date=..., message="..."},
-- shows the message in a banner above the login form, with a close (X) button.
--   * while the banner is shown, the endpoint is NOT queried
--   * closing it resumes polling, but the same message (same date) is ignored;
--     it reappears only when the endpoint serves a different date
--   * dismissal is per-session: a restart shows the message again
local function InstallMotd()
    if MOTD_URL == "" then return end
    if not FetchLoginMessage or not GetLoginMessageResult then
        log("motd: DLL functions missing, skipping")
        return
    end
    if not AccountLogin then
        log("motd: AccountLogin frame missing, skipping")
        return
    end
    log("motd: installed")

    -- Parented to AccountLogin so it can only ever appear on the login page
    -- and auto-hides when the client moves to realm/character select.
    local banner = CreateFrame("Frame", "EbonholdMotdFrame", AccountLogin)
    banner:SetWidth(450)
    banner:SetBackdrop({
        bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 11, top = 11, bottom = 11 },
    })
    if AccountLoginAccountEdit then
        banner:SetPoint("BOTTOM", AccountLoginAccountEdit, "TOP", 0, 70)
    else
        banner:SetPoint("TOP", AccountLogin, "TOP", 0, -120)
    end
    banner:Hide()

    local TEXT_WIDTH = 410   -- banner width minus 20px insets each side

    local text = banner:CreateFontString(nil, "ARTWORK", "GlueFontHighlight")
    text:SetPoint("TOP", banner, "TOP", 0, -20)
    text:SetWidth(TEXT_WIDTH)
    text:SetHeight(0)        -- auto: word-wraps within TEXT_WIDTH
    text:SetJustifyH("CENTER")
    text:SetJustifyV("TOP")

    local close = CreateFrame("Button", nil, banner)
    close:SetWidth(32); close:SetHeight(32)
    close:SetPoint("TOPRIGHT", banner, "TOPRIGHT", -6, -6)
    close:SetNormalTexture("Interface\\Buttons\\UI-Panel-MinimizeButton-Up")
    close:SetPushedTexture("Interface\\Buttons\\UI-Panel-MinimizeButton-Down")
    close:SetHighlightTexture("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight")

    local shownDate, lastClosedDate = nil, nil

    -- In the glue state GetStringHeight() returns the height of ONE line even
    -- when the string is word-wrapped (verified in-game), so the wrapped
    -- height cannot be read back from the widget. Instead, replicate the
    -- engine's greedy word-wrap with a hidden measuring FontString and
    -- GetStringWidth() (which IS reliable) to count lines, then size the box.
    local measure = banner:CreateFontString(nil, "ARTWORK", "GlueFontHighlight")
    measure:Hide()

    local function WrappedLineCount(msg)
        local lines = 0
        for para in string.gmatch(msg .. "\n", "([^\n]*)\n") do
            local line = ""
            for word in string.gmatch(para, "%S+") do
                local try = (line == "") and word or (line .. " " .. word)
                measure:SetText(try)
                if measure:GetStringWidth() > TEXT_WIDTH and line ~= "" then
                    lines = lines + 1    -- current line is full, wrap
                    line = word
                else
                    line = try
                end
            end
            lines = lines + 1            -- last (or empty) line of the paragraph
        end
        if lines < 1 then lines = 1 end
        return lines
    end

    local function ShowMessage(date, msg)
        shownDate = date
        text:SetText(msg)
        measure:SetText("Ag")
        local lineH = measure:GetStringHeight() or 15
        local n = WrappedLineCount(msg)
        banner:SetHeight(n * (lineH + 2) + 40)
        banner:Show()
        log("motd: showing message (date="..tostring(date)..", lines="..n..", lineH="..lineH..")")
    end

    close:SetScript("OnClick", function()
        lastClosedDate = shownDate
        banner:Hide()
        log("motd: closed by user (date="..tostring(shownDate)..")")
    end)

    -- First query fires immediately (sinceFetch starts at the interval).
    local fetching, sinceFetch, sincePoll = false, MOTD_INTERVAL, 0
    local ticker = CreateFrame("Frame")
    ticker:SetScript("OnUpdate", function(self, dt)
        dt = dt or 0
        if not AccountLogin:IsShown() then return end  -- login page only
        if banner:IsShown() then return end            -- no query while displayed
        if fetching then
            sincePoll = sincePoll + dt
            if sincePoll < MOTD_POLL then return end
            sincePoll = 0
            local status, date, msg = GetLoginMessageResult()
            if not status then return end
            fetching = false
            sinceFetch = 0
            if status == "OK" and msg ~= "" and date ~= lastClosedDate then
                ShowMessage(date, msg)
            elseif status ~= "OK" and status ~= "NONE" then
                log("motd: "..status)
            end
        else
            sinceFetch = sinceFetch + dt
            if sinceFetch < MOTD_INTERVAL then return end
            fetching = true
            sincePoll = 0
            FetchLoginMessage(MOTD_URL)
        end
    end)
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
        InstallMotd()
    end
end)
log("ebonhold_glue.lua: boot frame armed")

)EBONHOLD";
