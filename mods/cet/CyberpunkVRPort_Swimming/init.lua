-- Only state is bridged here. Controller/head poses and gesture recognition
-- remain in the plugin's coherent native hand publication.
local fast = -1
local reportedError = false

registerForEvent("onInit", function()
    -- Script decisions and native locomotion must see the same gesture input.
    -- Keep other actors/actions and the original input values in their chain.
    local actions = { MoveY = 20, ToggleSprint = 21, Jump = 22, Dive = 23 }
    local function valueFor(owner, name)
        local player = Game.GetPlayer()
        if not owner or not player or owner:GetEntityID().hash ~= player:GetEntityID().hash then return 0 end
        local index = actions[name.value]
        return index and Game.GetVRSwimmingDebug(index) or 0
    end
    Override("gamestateMachineGameScriptInterface", "GetActionValue", function(self, name, wrapped)
        local value = wrapped(name)
        local added = valueFor(self.owner, name)
        return added > 0 and math.max(value, added) or value
    end)
    Override("gamestateMachineGameScriptInterface", "IsActionJustPressed", function(self, name, wrapped)
        local pressed = wrapped(name)
        -- Dive is a surface transition; the command clears upon entering water.
        return pressed or (name.value == "Dive" and valueFor(self.owner, name) > 0)
    end)
    local function watch(class, method, value)
        ObserveAfter(class, method, function() fast = value end)
    end
    watch("SwimmingSurfaceEvents", "OnEnter", 0)
    watch("SwimmingDivingEvents", "OnEnter", 0)
    for _, class in ipairs({ "SwimmingSurfaceFastEvents", "SwimmingFastDivingEvents" }) do
        watch(class, "OnEnter", 1)
        watch(class, "OnExit", 0)
    end
    ObserveAfter("PlayerPuppet", "OnGameAttached", function() fast = -1 end)
end)

registerForEvent("onUpdate", function()
    local ok, err = pcall(function()
        local player = Game.GetPlayer()
        local state = -1
        local waterContext = false
        local paused = Game.GetSystemRequestsHandler():IsGamePaused()
        if player then
            local board = player:GetPlayerStateMachineBlackboard()
            if board then
                local defs = GetAllBlackboardDefs().PlayerStateMachine
                state = board:GetInt(defs.Swimming)
                -- HighLevel stays Swimming (6) while Surface/Diving transitions
                -- briefly clear the narrower Swimming blackboard value to 0.
                waterContext = board:GetInt(defs.HighLevel) == 6
            end
            waterContext = waterContext or player:IsUnderwater() or player:HasHeadUnderwater()
        end
        if state == 0 or state == 3 then fast = -1 end
        Game.SetVRSwimmingState(player, state, fast, paused and 1 or 0, waterContext and 1 or 0)
    end)
    if not ok and not reportedError then
        reportedError = true
        print("[swimming] state bridge unavailable: " .. tostring(err))
    elseif ok then
        reportedError = false
    end
end)

registerForEvent("onShutdown", function()
    pcall(function() Game.SetVRSwimmingState(nil, -1, -1, 1, 0) end)
end)
