-- CyberpunkVRPort QuickBoot
-- Development-only helper: when the first main menu appears, resume the most recent save once.
-- It never runs again after returning to the menu later in the same process.

local QuickBoot = {
    enabled = true,
    delay = 2.5,
    armed = false,
    fired = false,
    elapsed = 0.0,
    configPath = "quickboot.json",
    statusPath = "quickboot_state.txt",
}

local function writeStatus(value)
    local file = io.open(QuickBoot.statusPath, "w")
    if not file then return end
    file:write(value .. "\n")
    file:close()
end

local function loadConfig()
    local file = io.open(QuickBoot.configPath, "r")
    if not file then return end
    local body = file:read("*a")
    file:close()

    local enabled = body:match('"enabled"%s*:%s*(%a+)')
    local delay = body:match('"delay"%s*:%s*([%d%.]+)')
    if enabled then QuickBoot.enabled = enabled == "true" end
    if delay then
        local parsed = tonumber(delay)
        if parsed and parsed >= 0.0 and parsed <= 30.0 then QuickBoot.delay = parsed end
    end
end

local function saveConfig()
    local file = io.open(QuickBoot.configPath, "w")
    if not file then return end
    file:write(string.format('{\n  "enabled": %s,\n  "delay": %.2f\n}\n',
        tostring(QuickBoot.enabled), QuickBoot.delay))
    file:close()
end

local function resumeLastSave()
    local handler = Game.GetSystemRequestsHandler()
    if not handler then
        print("[CyberpunkVR QuickBoot] SystemRequestsHandler is unavailable")
        writeStatus("failed:no-system-requests-handler")
        return false
    end

    -- The RTTI name has moved across game releases. Keep the alternatives isolated and report
    -- the exact one that works instead of silently binding the test flow to one guessed method.
    local attempts = {
        { label = "LoadLastCheckpoint(false)", call = function() handler:LoadLastCheckpoint(false) end },
        { label = "LoadLastCheckpoint()", call = function() handler:LoadLastCheckpoint() end },
        { label = "RequestLoadLastCheckpoint()", call = function() handler:RequestLoadLastCheckpoint() end },
    }

    for _, attempt in ipairs(attempts) do
        local ok, err = pcall(attempt.call)
        if ok then
            print("[CyberpunkVR QuickBoot] resumed via " .. attempt.label)
            writeStatus("resumed:" .. attempt.label)
            return true
        end
        print("[CyberpunkVR QuickBoot] " .. attempt.label .. " failed: " .. tostring(err))
    end

    writeStatus("failed:no-compatible-continue-method")
    print("[CyberpunkVR QuickBoot] no compatible Continue method was found")
    return false
end

local function armFromMainMenu(source)
    if QuickBoot.fired or QuickBoot.armed or not QuickBoot.enabled then return end
    QuickBoot.armed = true
    QuickBoot.elapsed = 0.0
    writeStatus("armed:" .. source)
    print(string.format("[CyberpunkVR QuickBoot] main menu observed by %s; resuming in %.2fs",
        source, QuickBoot.delay))
end

registerForEvent("onInit", function()
    loadConfig()
    saveConfig()
    writeStatus(QuickBoot.enabled and "ready" or "disabled")

    local observers = 0
    for _, className in ipairs({ "gameuiMainMenuGameController", "MenuScenario_MainMenu" }) do
        local ok = pcall(function()
            Observe(className, "OnInitialize", function()
                armFromMainMenu(className)
            end)
        end)
        if ok then observers = observers + 1 end
    end

    registerHotkey("cyberpunkvr_quickboot_toggle", "CyberpunkVR QuickBoot: toggle", function()
        QuickBoot.enabled = not QuickBoot.enabled
        saveConfig()
        writeStatus(QuickBoot.enabled and "ready" or "disabled")
        print("[CyberpunkVR QuickBoot] " .. (QuickBoot.enabled and "enabled" or "disabled") ..
            " for the next launch")
    end)

    print(string.format("[CyberpunkVR QuickBoot] ready; enabled=%s delay=%.2fs observers=%d",
        tostring(QuickBoot.enabled), QuickBoot.delay, observers))
end)
registerForEvent("onUpdate", function(deltaSeconds)
    if not QuickBoot.enabled then
        QuickBoot.armed = false
        return
    end
    if not QuickBoot.armed or QuickBoot.fired then return end
    QuickBoot.elapsed = QuickBoot.elapsed + deltaSeconds
    if QuickBoot.elapsed < QuickBoot.delay then return end

    -- Claim the one-shot before calling into the game. A throwing or renamed RTTI method must not
    -- turn into an every-frame load loop.
    QuickBoot.fired = true
    QuickBoot.armed = false
    writeStatus("resuming")
    resumeLastSave()
end)
