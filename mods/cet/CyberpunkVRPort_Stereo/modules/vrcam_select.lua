-- Enable exactly ONE authored VRCAM component, chosen by render resolution.
--
-- A dedicated static world entity carries one entRenderToTextureCameraComponent per resolution
-- (world_vrcam_<W>x<H>, virtualCameraName vrcam_feed_<W>x<H>), ALL authored with isEnabled=0.
-- The launcher writes the picked one into vrcam.json; this module spawns that entity once with
-- StaticEntitySpec.attached=true and switches exactly one component on. Test B waits one complete
-- CET update after that component is enabled, calls SetWorldTransform() once with the exact spawn
-- transform, then leaves the owner untouched forever to test for persistent RTT activation.
--
-- Selection is by NAME only. Nothing here inspects aspect ratio or resolution numbers, so
-- adding a resolution means adding a component plus a catalogue entry -- no code change.
--
-- The native plugin reads the same vrcam.json to derive the view key (CName hash of the
-- virtualCameraName). Both sides must agree, which is why there is a single file and it lives
-- here: CET sandboxes Lua file IO to the mod folder, so this side cannot reach out, while the
-- native side can and does.

local VrcamSelect = {
    selected     = nil,     -- component name from vrcam.json
    want         = true,    -- false once the overlay asks for VRCAM off
    appliedName  = nil,     -- what we last actually applied (nil = nothing applied yet)
    appliedWant  = nil,
    lastError    = nil,
    ticksToRetry = 0,
    enabledCount = 0,
    seenCount    = 0,
    appliedOwner = nil,
    staticId     = nil,
    staticEntity = nil,
    staticSystem = nil,
    staticState  = "idle",
    takeover     = false,
    fixedTransform = nil,
    bPhase       = "idle",
    bWaitUpdates = 0,
    bSetterCount = 0,
    bUpdateCount = 0,
}

local RETRY_TICKS = 30      -- ~150 ms @ 200 fps; the player is not there during load
local NAME_PREFIX = "vrcam_"
local WORLD_NAME_PREFIX = "world_vrcam_"
local WORLD_TEMPLATE = "base\\vrport\\vrport_world_vrcam.ent"
-- Archive build: select the attached static world RTT entity. Native binding follows the
-- component actually published below; switch this off only for the separate player-owner A/B.
local USE_STATIC_WORLD = true

local function safeCall(obj, methodName, ...)
    if obj == nil then return false, "nil obj" end
    local m = obj[methodName]
    if m == nil then return false, "method missing" end
    local ok, ret = pcall(m, obj, ...)
    if not ok then return false, "throw: " .. tostring(ret) end
    return true, ret
end

-- CName reads back differently across CET builds (userdata with .value, or tostring-able).
local function cnameToString(v)
    if v == nil then return nil end
    local ok, s = pcall(function() return v.value end)
    if ok and type(s) == "string" and s ~= "" then return s end
    local ok2, s2 = pcall(function() return tostring(v) end)
    if ok2 and type(s2) == "string" and s2 ~= "" then return s2 end
    return nil
end

-- Read "component" out of the flat JSON. A pattern rather than a JSON library: the file is two
-- fields, and the "_comment" block is an array so it cannot match a "key": "value" pair.
local function readSelection()
    local f = io.open("vrcam.json", "r")
    if not f then return nil, "vrcam.json not found in the mod folder" end
    local body = f:read("*a")
    f:close()
    if not body or body == "" then return nil, "vrcam.json is empty" end
    local name = body:match('"component"%s*:%s*"([^"]+)"')
    if not name then return nil, 'vrcam.json has no "component" field' end
    return name, nil
end

-- The overlay's Disable/Enable VRCAM button writes this. Component isEnabled is only reachable
-- through the game's RTTI, i.e. from here, so native asks and this side does it.
local function readEnableRequest()
    local f = io.open("bridge/vrcam_enable.txt", "r")
    if not f then return nil end
    local body = f:read("*a")
    f:close()
    if not body then return nil end
    if body:find("0", 1, true) then return false end
    if body:find("1", 1, true) then return true end
    return nil
end

function VrcamSelect.reload()
    local name, err = readSelection()
    VrcamSelect.selected    = name
    VrcamSelect.lastError   = err
    VrcamSelect.appliedName = nil     -- force one re-apply after a manual reload
    VrcamSelect.appliedWant = nil
    VrcamSelect.appliedOwner = nil
    if err then
        print("[Stereo.VRCAM] " .. err .. " -- no component will be enabled")
    else
        print("[Stereo.VRCAM] selection: " .. name)
    end
    return name
end

-- Flip one component. Toggle() is the engine's own enable/disable entry point; the direct field
-- write is a fallback for builds where it is not exposed to scripts.
local function setEnabled(comp, want)
    local ok = safeCall(comp, "Toggle", want)
    if ok then return true, "Toggle" end
    local okSet = pcall(function() comp.isEnabled = want end)
    if okSet then return true, "isEnabled" end
    return false, "no way to toggle"
end

local function apply(entity, wanted, want_on, owner, publish)
    if not wanted then return false end

    local worldOwned = owner == "static world"
    local scanPrefix = worldOwned and WORLD_NAME_PREFIX or NAME_PREFIX
    local targetName = worldOwned and ("world_" .. wanted) or wanted
    local function reportError(message)
        if publish ~= false then
            VrcamSelect.lastError = message
        else
            print("[Stereo.VRCAM] fallback cleanup warning: " .. message)
        end
    end

    local okL, comps = safeCall(entity, "GetComponents")
    if not okL or not comps then
        reportError(tostring(owner) .. ":GetComponents() unavailable")
        return false
    end
    local n = 0
    pcall(function() n = #comps end)
    if n == 0 then
        reportError(tostring(owner) .. " has no components yet")
        return false
    end

    local seen, enabled, disabled, found, how = 0, 0, 0, false, nil
    local enabledComp = nil
    for i = 1, n do
        local c = comps[i]
        local nm = c and cnameToString(c.name) or nil
        if nm and nm:sub(1, #scanPrefix) == scanPrefix then
            seen = seen + 1
            local want = want_on and (nm == targetName)
            local ok, via = setEnabled(c, want)
            if ok then
                if want then found, enabled, how, enabledComp = true, enabled + 1, via, c
                else disabled = disabled + 1 end
            else
                reportError("cannot toggle " .. nm .. ": " .. tostring(via))
            end
        end
    end

    if publish ~= false then
        VrcamSelect.seenCount    = seen
        VrcamSelect.enabledCount = enabled
    end

    if seen == 0 then
        -- Do not retry forever on an entity that simply has no VRCAM components authored.
        reportError("no " .. scanPrefix .. "* components on " .. tostring(owner))
        return false
    end
    if want_on and not found then
        local message = string.format(
            "%s not found among %d %s* component(s) -- all left disabled",
            targetName, seen, scanPrefix)
        reportError(message)
        print("[Stereo.VRCAM] " .. message)
        return true      -- the entity was read fine; retrying will not change the outcome
    end

    if publish ~= false then VrcamSelect.lastError = nil end
    -- Hand the native side the ACTUAL virtualCameraName of the component we just enabled.
    -- The render path identifies the VRCAM view by the CName hash of that name, and deriving
    -- it from the component name assumes the asset follows vrcam_feed_<suffix>. When it does
    -- not (a component renamed in WolvenKit while its camera field kept the old value) the
    -- key matches no view and everything downstream goes silent -- no stereo, no mirror --
    -- while every log line still names the right component. Reading it back off the live
    -- component removes that failure mode: the asset itself is the source of truth.
    local activeCam = ""
    if want_on and enabledComp then
        activeCam = cnameToString(enabledComp.virtualCameraName) or ""
    end
    if publish ~= false then
        local wf = io.open("bridge/vrcam_active.txt", "w")
        if wf then wf:write(activeCam); wf:close() end
        VrcamSelect.activeCamera = activeCam
    end

    if publish == false then return true end

    if want_on then
        print(string.format(
            "[Stereo.VRCAM] enabled %s on %s (camera %s) via %s (1 of %d, %d disabled)",
            targetName, tostring(owner), activeCam ~= "" and activeCam or "?", tostring(how),
            seen, disabled))
        local expect = "vrcam_feed_" .. wanted:sub(#NAME_PREFIX + 1)
        if activeCam ~= "" and activeCam ~= expect then
            print(string.format(
                "[Stereo.VRCAM] %s has virtualCameraName=%s (expected %s) -- native will use the real one",
                wanted, activeCam, expect))
        end
    else
        print(string.format("[Stereo.VRCAM] VRCAM off (%d component(s) disabled)", disabled))
    end
    return true
end

local function clearStaticEntity()
    if VrcamSelect.staticSystem and VrcamSelect.staticId then
        safeCall(VrcamSelect.staticSystem, "DespawnEntity", VrcamSelect.staticId)
    end
    VrcamSelect.staticId = nil
    VrcamSelect.staticEntity = nil
    VrcamSelect.staticSystem = nil
    VrcamSelect.staticState = "idle"
    VrcamSelect.takeover = false
    VrcamSelect.fixedTransform = nil
    VrcamSelect.bPhase = "idle"
    VrcamSelect.bWaitUpdates = 0
    VrcamSelect.bSetterCount = 0
    VrcamSelect.bUpdateCount = 0
    VrcamSelect.appliedName = nil
    VrcamSelect.appliedWant = nil
    VrcamSelect.appliedOwner = nil
end

local function getStaticSystem()
    if VrcamSelect.staticSystem then return VrcamSelect.staticSystem end
    local ok, system = pcall(function()
        if not Game.GetStaticEntitySystem then return nil end
        return Game.GetStaticEntitySystem()
    end)
    if ok and system then
        VrcamSelect.staticSystem = system
        return system
    end
    VrcamSelect.staticState = "StaticEntitySystem unavailable (Codeware missing?)"
    return nil
end

local function spawnStaticEntity(player, system)
    local okReady, ready = safeCall(system, "IsReady")
    if okReady and not ready then
        VrcamSelect.staticState = "waiting for StaticEntitySystem"
        return
    end

    local okPos, pos = safeCall(player, "GetWorldPosition")
    local okRot, rot = safeCall(player, "GetWorldOrientation")
    if not okPos or not pos or not okRot or not rot then
        VrcamSelect.staticState = "waiting for initial player world pose"
        return
    end

    local fixedPosition = Vector4.new(pos.x, pos.y, pos.z + 1.7, 1.0)
    local fixedOrientation = Quaternion.new(rot.i, rot.j, rot.k, rot.r)
    local fixedTransform = WorldTransform.new()
    fixedTransform:SetPosition(fixedPosition)
    fixedTransform:SetOrientation(fixedOrientation)

    local ok, id = pcall(function()
        local spec = StaticEntitySpec.new()
        spec.templatePath = ResRef.FromString(WORLD_TEMPLATE)
        spec.position = fixedPosition
        spec.orientation = fixedOrientation
        spec.attached = true
        return system:SpawnEntity(spec)
    end)
    if not ok or not id then
        VrcamSelect.staticState = "static spawn failed: " .. tostring(id)
        return
    end

    VrcamSelect.staticId = id
    VrcamSelect.fixedTransform = fixedTransform
    VrcamSelect.staticState = "spawning attached static entity"
    VrcamSelect.bPhase = "waiting for attached entity"
    VrcamSelect.bWaitUpdates = 0
    VrcamSelect.bSetterCount = 0
    VrcamSelect.bUpdateCount = 0
    print(string.format(
        "[Stereo.VRCAM] static spawn requested template=%s attached=true " ..
        "initial=(%.3f, %.3f, %.3f) transformWrites=0",
        WORLD_TEMPLATE, pos.x, pos.y, pos.z + 1.7))
end

local function ensureStaticEntity(player)
    local system = getStaticSystem()
    if not system then return nil end

    if not VrcamSelect.staticId then
        spawnStaticEntity(player, system)
        return nil
    end

    if VrcamSelect.staticEntity then
        local okManaged, managed = safeCall(system, "IsManaged", VrcamSelect.staticId)
        if okManaged and not managed then
            print("[Stereo.VRCAM] static entity is no longer managed; respawning")
            clearStaticEntity()
            return nil
        end
        return VrcamSelect.staticEntity
    end

    local ok, entity = safeCall(system, "GetEntity", VrcamSelect.staticId)
    if not ok or not entity then
        VrcamSelect.staticState = "waiting for attached static entity"
        return nil
    end

    VrcamSelect.staticEntity = entity
    VrcamSelect.staticState = "attached; transformWrites=0"
    VrcamSelect.bPhase = "waiting for selected RTT Toggle"
    VrcamSelect.appliedOwner = nil
    print("[Stereo.VRCAM] static entity attached; owner will remain static (transformWrites=0)")
    return entity
end

local function armBOneShot()
    if VrcamSelect.bPhase ~= "waiting for selected RTT Toggle" then return end
    VrcamSelect.bPhase = "wait one complete update after Toggle"
    VrcamSelect.bWaitUpdates = 1
    print(string.format(
        "[worldvrcam-test] B begin attached=1 toggle=1 waitUpdates=%d setterCount=%d",
        VrcamSelect.bWaitUpdates, VrcamSelect.bSetterCount))
end

local function updateBOneShot(world)
    if not world or not VrcamSelect.fixedTransform then return end
    VrcamSelect.bUpdateCount = VrcamSelect.bUpdateCount + 1

    if VrcamSelect.bPhase == "wait one complete update after Toggle" then
        if VrcamSelect.bWaitUpdates > 0 then
            VrcamSelect.bWaitUpdates = VrcamSelect.bWaitUpdates - 1
            print(string.format(
                "[worldvrcam-test] B wait update=%d remaining=%d setterCount=%d",
                VrcamSelect.bUpdateCount, VrcamSelect.bWaitUpdates,
                VrcamSelect.bSetterCount))
            return
        end
        VrcamSelect.bPhase = "pulse"
    end

    if VrcamSelect.bPhase ~= "pulse" then return end

    VrcamSelect.bSetterCount = VrcamSelect.bSetterCount + 1
    local ok, err = pcall(function()
        world:SetWorldTransform(VrcamSelect.fixedTransform)
    end)
    if not ok then
        VrcamSelect.bPhase = "pulse failed"
        VrcamSelect.staticState = "attached; B pulse failed"
        VrcamSelect.lastError = "B SetWorldTransform failed: " .. tostring(err)
        print(string.format(
            "[worldvrcam-test] B pulse failed update=%d setterCount=%d error=%s",
            VrcamSelect.bUpdateCount, VrcamSelect.bSetterCount, tostring(err)))
        return
    end

    VrcamSelect.bPhase = "observe"
    VrcamSelect.staticState = "attached; B observe setterCount=1"
    print(string.format(
        "[worldvrcam-test] B pulse update=%d setterCount=%d",
        VrcamSelect.bUpdateCount, VrcamSelect.bSetterCount))
    print("[worldvrcam-test] B observe transform untouched from now on")
end

function VrcamSelect.init()
    VrcamSelect.reload()
end

function VrcamSelect.tick(dt)
    local player = Game.GetPlayer and Game.GetPlayer() or nil
    if not player then
        clearStaticEntity()
        return
    end

    local world = USE_STATIC_WORLD and ensureStaticEntity(player) or nil
    updateBOneShot(world)
    if VrcamSelect.ticksToRetry > 0 then
        VrcamSelect.ticksToRetry = VrcamSelect.ticksToRetry - 1
        return
    end
    VrcamSelect.ticksToRetry = RETRY_TICKS

    local req = readEnableRequest()
    if req ~= nil then VrcamSelect.want = req end

    -- Re-apply only when the DESIRED state changed. The old guard compared tostring(player),
    -- but CET hands out a fresh wrapper on every Game.GetPlayer() call, so that string differed
    -- every time and the module re-applied (and logged) a few times a second.
    local owner = world and "static world" or "player fallback"
    if VrcamSelect.appliedName == VrcamSelect.selected and
       VrcamSelect.appliedWant == VrcamSelect.want and
       VrcamSelect.appliedOwner == owner then
        return
    end

    local applied = false
    if world then
        applied = apply(world, VrcamSelect.selected, VrcamSelect.want, owner, true)
        if applied then
            apply(player, VrcamSelect.selected, false, "player fallback", false)
            VrcamSelect.takeover = true
            if VrcamSelect.want then armBOneShot() end
        end
    elseif not VrcamSelect.takeover then
        applied = apply(player, VrcamSelect.selected, VrcamSelect.want, owner, true)
    end

    if applied then
        VrcamSelect.appliedName = VrcamSelect.selected
        VrcamSelect.appliedWant = VrcamSelect.want
        VrcamSelect.appliedOwner = owner
    end
end

function VrcamSelect.shutdown()
    clearStaticEntity()
end

function VrcamSelect.status()
    return string.format(
        "selected=%s want=%s applied=%s owner=%s static=%s B=%s setters=%d vrcamComps=%d enabled=%d%s",
        tostring(VrcamSelect.selected), tostring(VrcamSelect.want),
        tostring(VrcamSelect.appliedName), tostring(VrcamSelect.appliedOwner),
        tostring(VrcamSelect.staticState), tostring(VrcamSelect.bPhase),
        VrcamSelect.bSetterCount, VrcamSelect.seenCount, VrcamSelect.enabledCount,
        VrcamSelect.lastError and (" err=" .. VrcamSelect.lastError) or "")
end

return VrcamSelect
