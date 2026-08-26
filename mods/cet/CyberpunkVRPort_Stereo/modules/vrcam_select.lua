-- Enable exactly ONE authored VRCAM component, chosen by render resolution.
--
-- A dedicated world entity carries one entRenderToTextureCameraComponent per resolution
-- (world_vrcam_<W>x<H>, virtualCameraName vrcam_feed_<W>x<H>), ALL authored with isEnabled=0.
-- The launcher writes the picked one into vrcam.json; this module spawns that entity near MAIN and
-- switches exactly one component on. The entity transform owns the pose in the SetWorldTransform A/B.
-- Player-authored VRCAM components remain only as a safe fallback while this path is validated.
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
    worldId      = nil,
    worldEntity  = nil,
    worldSystem  = nil,
    worldState   = "idle",
    worldTakeover = false,
    noPlayerTime = 0.0,
}

local RETRY_TICKS = 30      -- ~150 ms @ 200 fps; the player is not there during load
local NAME_PREFIX = "vrcam_"
local WORLD_NAME_PREFIX = "world_vrcam_"
local WORLD_TEMPLATE = "base\\vrport\\vrport_world_vrcam.ent"

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
local function readEnabled(comp)
    local okField, fieldValue = pcall(function() return comp.isEnabled end)
    if okField and fieldValue ~= nil then
        return "field=" .. tostring(fieldValue)
    end
    local okMethod, methodValue = pcall(function()
        if comp.IsEnabled == nil then return nil end
        return comp:IsEnabled()
    end)
    if okMethod and methodValue ~= nil then
        return "method=" .. tostring(methodValue)
    end
    return "unavailable"
end

local function setEnabled(comp, want)
    local ok, result = safeCall(comp, "Toggle", want)
    if ok then return true, "Toggle", result, readEnabled(comp) end
    local okSet = pcall(function() comp.isEnabled = want end)
    if okSet then return true, "isEnabled", "assigned", readEnabled(comp) end
    return false, "no way to toggle", nil, readEnabled(comp)
end

local function apply(entity, wanted, want_on, owner, publish)
    if not wanted then return false end
    local worldOwned = owner == "world"
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
    local targetCall, targetResult, targetReadback = "not seen", "n/a", "n/a"
    for i = 1, n do
        local c = comps[i]
        local nm = c and cnameToString(c.name) or nil
        if nm and nm:sub(1, #scanPrefix) == scanPrefix then
            seen = seen + 1
            local want = want_on and (nm == targetName)
            local ok, via, result, readback = setEnabled(c, want)
            if nm == targetName then
                targetCall = tostring(via)
                targetResult = tostring(result)
                targetReadback = tostring(readback)
            end
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
        local message = string.format("%s not found among %d %s* component(s) -- all left disabled",
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

    print(string.format("[Stereo.VRCAM][toggle] owner=%s component=%s requested=%s " ..
                        "call=%s return=%s readback=%s",
                        tostring(owner), targetName, tostring(want_on), targetCall,
                        targetResult, targetReadback))

    if publish == false then return true end

    if want_on then
        print(string.format("[Stereo.VRCAM] enabled %s on %s (camera %s) via %s " ..
                            "(1 of %d, %d disabled)", wanted, tostring(owner),
                            activeCam ~= "" and activeCam or "?", tostring(how), seen, disabled))
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

local function readMainPose()
    if type(VRVrcamWorldPos) ~= "function" or type(VRMainCameraWorldRot) ~= "function" or
       type(VRMainCameraSeq) ~= "function" then
        return nil, nil, "MAIN camera natives unavailable"
    end
    -- The position is derived from MAIN's rendered snapshot plus one signed full IPD. The
    -- world-owned component name intentionally bypasses the old player-camera patch, so this is
    -- the one and only eye offset applied to its root.
    for _ = 1, 4 do
        local okS1, seq1 = pcall(function() return VRMainCameraSeq() end)
        local okP, p = pcall(function() return VRVrcamWorldPos() end)
        local okQ, q = pcall(function() return VRMainCameraWorldRot() end)
        local okS2, seq2 = pcall(function() return VRMainCameraSeq() end)
        if okS1 and okS2 and okP and okQ and p and q and
           seq1 ~= 0 and seq1 == seq2 and (p.w or 0.0) >= 0.5 then
            return p, q, nil
        end
    end
    return nil, nil, "MAIN camera pose not ready"
end

local function getWorldSystem()
    if VrcamSelect.worldSystem then return VrcamSelect.worldSystem end
    local ok, system = pcall(function() return Game.GetDynamicEntitySystem() end)
    if ok and system then VrcamSelect.worldSystem = system end
    return VrcamSelect.worldSystem
end

local function clearWorldEntity(deleteManaged)
    local system = VrcamSelect.worldSystem
    if deleteManaged and system and VrcamSelect.worldId then
        pcall(function() system:DeleteEntity(VrcamSelect.worldId) end)
    end
    VrcamSelect.worldId = nil
    VrcamSelect.worldEntity = nil
    VrcamSelect.worldSystem = nil
    VrcamSelect.worldState = "idle"
    VrcamSelect.worldTakeover = false
    VrcamSelect.noPlayerTime = 0.0
    VrcamSelect.appliedOwner = nil
end

local function readWorldEntityLifecycle(entity)
    if not entity then return -1, false end
    local okStatus, status = pcall(function() return VRWorldEntityStatus(entity) end)
    local okReady, ready = pcall(function() return VRWorldEntityReady(entity) end)
    if not okStatus or not okReady then return -2, false end
    return tonumber(status) or -2, ready == true
end

local function abandonWorldEntity(status, reason)
    local message = string.format("%s (status=%s)", reason, tostring(status))
    print("[Stereo.VRCAM] " .. message)
    clearWorldEntity(false)
    VrcamSelect.worldState = message
end

local function validateCachedWorldEntity()
    if not VrcamSelect.worldEntity then return true end
    local status, ready = readWorldEntityLifecycle(VrcamSelect.worldEntity)
    if ready then return true end

    -- A Lua handle can remain truthy after REDengine has uninitialized the entity. Never enter
    -- Codeware's SetWorldTransform wrapper once the transform component has been detached.
    abandonWorldEntity(status, "world entity lifecycle ended")
    return false
end

local function updateWorldEntity(p, q)
    local system = getWorldSystem()
    if not system then
        VrcamSelect.worldState = "Codeware DynamicEntitySystem unavailable"
        return nil
    end
    local okReady, ready = pcall(function() return system:IsReady() end)
    if not okReady or not ready then
        VrcamSelect.worldState = "DynamicEntitySystem not ready"
        return nil
    end

    if not VrcamSelect.worldId then
        local ok, id = pcall(function()
            local spec = DynamicEntitySpec.new()
            spec.templatePath = ResRef.FromString(WORLD_TEMPLATE)
            spec.position = Vector4.new(p.x, p.y, p.z, 1.0)
            spec.orientation = Quaternion.new(q.i, q.j, q.k, q.r)
            spec.persistState = false
            spec.persistSpawn = false
            spec.alwaysSpawned = true
            spec.spawnInView = true
            spec.active = true
            return system:CreateEntity(spec)
        end)
        if not ok or not id then
            VrcamSelect.worldState = "spawn failed: " .. tostring(id)
            return nil
        end
        VrcamSelect.worldId = id
        VrcamSelect.worldState = "spawning"
        print("[Stereo.VRCAM] spawning world entity " .. WORLD_TEMPLATE)
        return nil
    end

    if not VrcamSelect.worldEntity then
        local ok, entity = pcall(function() return system:GetEntity(VrcamSelect.worldId) end)
        if not ok or not entity then return nil end
        local status, ready = readWorldEntityLifecycle(entity)
        if not ready then
            if status >= 5 or status < 0 then
                abandonWorldEntity(status, "world entity became terminal before attach")
            else
                VrcamSelect.worldState = "spawning status=" .. tostring(status)
            end
            return nil
        end
        VrcamSelect.worldEntity = entity
        VrcamSelect.worldState = "attached"
        VrcamSelect.appliedOwner = nil
        print("[Stereo.VRCAM] world entity attached; transform lifecycle gate passed")
    end

    -- Keep the dynamic entity on the engine's normal transform/lifecycle path. The previous A/B
    -- removed this call and the selected world component's post-store hook fired only at assembly;
    -- this version deliberately restores the original SetWorldTransform behaviour and disables
    -- the native +0xE0/+0xF0 overwrite so the HMD and view-key counters can test that path alone.
    local okMove, moveErr = pcall(function()
        local transform = WorldTransform.new()
        transform:SetPosition(Vector4.new(p.x, p.y, p.z, 1.0))
        transform:SetOrientation(Quaternion.new(q.i, q.j, q.k, q.r))
        VrcamSelect.worldEntity:SetWorldTransform(transform)
    end)
    if okMove then
        VrcamSelect.worldState = "SetWorldTransform tracking"
    else
        VrcamSelect.worldState = "SetWorldTransform failed: " .. tostring(moveErr)
    end
    return VrcamSelect.worldEntity
end

function VrcamSelect.init()
    VrcamSelect.reload()
end

function VrcamSelect.tick(dt)
    -- Validate before the player/world transition handling and before any component enumeration.
    -- Game.GetPlayer() can remain non-nil while a DynamicEntitySystem entity is already stale.
    validateCachedWorldEntity()

    local player = Game.GetPlayer and Game.GetPlayer() or nil
    if not player then
        -- Game.GetPlayer() can disappear briefly during streaming/camera transitions. Keep a
        -- completed world takeover sticky so one transient tick cannot delete the entity and
        -- reactivate the player camera with the same virtualCameraName.
        local elapsed = tonumber(dt) or 0.0
        if elapsed <= 0.0 then elapsed = 1.0 / 60.0 end
        VrcamSelect.noPlayerTime = VrcamSelect.noPlayerTime + elapsed
        if VrcamSelect.noPlayerTime >= 5.0 then
            VrcamSelect.appliedName = nil
            VrcamSelect.appliedWant = nil
            VrcamSelect.appliedOwner = nil
            clearWorldEntity(true)
        end
        return
    end
    VrcamSelect.noPlayerTime = 0.0

    local world = VrcamSelect.worldEntity
    -- Read and apply MAIN on every update, including after the entity has spawned. Besides moving
    -- the root, SetWorldTransform drives the engine propagation whose absence this A/B is testing.
    local mainPos, mainRot, poseErr = readMainPose()
    if mainPos then
        updateWorldEntity(mainPos, mainRot)
        world = VrcamSelect.worldEntity
    else
        VrcamSelect.worldState = poseErr
    end

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
    local owner = (world or VrcamSelect.worldTakeover) and "world" or "player fallback"
    if VrcamSelect.appliedName == VrcamSelect.selected and
       VrcamSelect.appliedWant == VrcamSelect.want and
       VrcamSelect.appliedOwner == owner then
        return
    end

    local applied = false
    if world then
        -- Handoff only after the world component is known-good: enable it first, then turn every
        -- legacy player VRCAM off in the same update. There is never a field-copy camera pair.
        applied = apply(world, VrcamSelect.selected, VrcamSelect.want, owner, true)
        if applied then
            apply(player, VrcamSelect.selected, false, "player fallback", false)
            VrcamSelect.worldTakeover = true
        end
    elseif not VrcamSelect.worldTakeover then
        applied = apply(player, VrcamSelect.selected, VrcamSelect.want, owner, true)
    else
        -- A completed takeover never falls back because of a transient pose/entity lookup miss.
        -- Keep the last applied state and retry the world path on the next update.
        return
    end
    if applied then
        VrcamSelect.appliedName = VrcamSelect.selected
        VrcamSelect.appliedWant = VrcamSelect.want
        VrcamSelect.appliedOwner = owner
    end
end

function VrcamSelect.shutdown()
    clearWorldEntity(true)
end

function VrcamSelect.status()
    return string.format("selected=%s want=%s applied=%s owner=%s world=%s " ..
        "vrcamComps=%d enabled=%d%s",
        tostring(VrcamSelect.selected), tostring(VrcamSelect.want),
        tostring(VrcamSelect.appliedName), tostring(VrcamSelect.appliedOwner),
        tostring(VrcamSelect.worldState), VrcamSelect.seenCount, VrcamSelect.enabledCount,
        VrcamSelect.lastError and (" err=" .. VrcamSelect.lastError) or "")
end

return VrcamSelect
