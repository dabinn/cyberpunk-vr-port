local container = nil

-- RA01 force crosshair to show only when projectile launcher is active start
local LAUNCHER_CROSSHAIR_OFFSET_Y = 350.0 --adjust launcher crosshair position -ie. where the laucher is aimed from
            -- pos = y down, neg = y up ---- SetTranslation

local function isLauncherEquipped()
    local ok, result = pcall(function()
        local pl = Game.GetPlayer()
        local wpn = pl and pl:GetActiveWeapon()
        if not wpn then return false end
        local key = TDBID.ToStringDEBUG(ItemID.GetTDBID(wpn:GetItemID()))
        return key ~= nil and string.find(string.lower(key), 'projectilelauncher', 1, true) ~= nil
    end)
    return ok and result or false
end

-- RA01 hide the crosshair for melee weapons and melee-style cyberware start
-- GetVRWeaponAim() only reflects VR hand-aim mode for a real gun -- it reads false for melee
-- weapons and cyberware arms (nothing to "aim" there), which made applyCrosshairVisibility's
-- `not aimEnabled` fall back to its default-visible behavior for them, same as if no weapon were
-- out at all. Swords/knives, Mantis Blades, Gorilla Arms and Monowire all want the opposite: the
-- reticle should never show while they're in hand. Classified the same way the Weapon mod already
-- does (WeaponObject.IsMelee + TDBID name match for the cyberware arms, since IsMelee() and the
-- native "weapon flag" are both blind to cyberware -- see projectile-launcher-reticle-fix.md).
local function isMeleeOrCyberArmEquipped()
    local ok, result = pcall(function()
        local pl = Game.GetPlayer()
        local wpn = pl and pl:GetActiveWeapon()
        if not wpn then return false end
        if WeaponObject.IsMelee(wpn:GetItemID()) then return true end
        local key = TDBID.ToStringDEBUG(ItemID.GetTDBID(wpn:GetItemID()))
        local low = key and string.lower(key) or nil
        if not low then return false end
        return string.find(low, 'mantis', 1, true) ~= nil
            or string.find(low, 'strongarms', 1, true) ~= nil
            or string.find(low, 'gorilla', 1, true) ~= nil
            or string.find(low, 'nanowires', 1, true) ~= nil
            or string.find(low, 'monowire', 1, true) ~= nil
    end)
    return ok and result or false
end
-- RA01 hide the crosshair for melee weapons and melee-style cyberware end

local crosshairOffsetLoggedOnce = false

local function applyCrosshairVisibility()
    if not container then return end
    if not IsDefined(container) then
        container = nil
        return
    end

    local aimEnabled = GetVRWeaponAim()
    local forceShow = isLauncherEquipped()
    local forceHide = isMeleeOrCyberArmEquipped()
    local visible = (not forceHide) and (forceShow or (not aimEnabled))

    local root = container:GetRootWidget()
    local active = container:GetActiveCrosshairWidget()

    root:SetVisible(visible)
    active:SetVisible(visible)

    -- Nudge it down for the launcher only. pcall'd on its own so if SetTranslation/Vector2.new
    -- ever mismatches, only the positioning is lost -- the visibility toggle above still runs.
    local offsetY = forceShow and LAUNCHER_CROSSHAIR_OFFSET_Y or 0.0
    local ok = pcall(function()
        root:SetTranslation(Vector2.new(0.0, offsetY))
        active:SetTranslation(Vector2.new(0.0, offsetY))
    end)
    if not crosshairOffsetLoggedOnce then
        crosshairOffsetLoggedOnce = true
        if ok then
            print(string.format("[VR] Crosshair offset applied: y=%.1f", offsetY))
        else
            print("[VR] Crosshair offset FAILED -- SetTranslation/Vector2.new didn't take")
        end
    end
end
-- RA01 force crosshair to show only when projectile launcher is active end

registerForEvent("onInit", function()
    if type(GetVRWeaponAim) ~= 'function' then
        print("[VR] ERROR: GetVRWeaponAim function not found!")
        return
    end

    Observe("gameuiCrosshairContainerController", "OnInitialize", function(this)
        container = this
        print("[VR] Crosshair container captured")
    end)

    Observe("gameuiCrosshairBaseGameController", "OnCrosshairStateChange", function(this, old, new)
        applyCrosshairVisibility()
    end)

    Observe("gameuiCrosshairBaseGameController", "UpdateCrosshairState", function(this)
        applyCrosshairVisibility()
    end)

    -- Belt-and-suspenders: also re-assert every frame, in case the 
    -- Projectile launcher becomes active
    -- without either of the two events above firing on their own.
    registerForEvent('onUpdate', function(dt)
        pcall(applyCrosshairVisibility)
    end)
end)