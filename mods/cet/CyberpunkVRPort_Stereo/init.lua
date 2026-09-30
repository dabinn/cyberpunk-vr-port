-- CyberpunkVRPort_Stereo — script-side VRCAM activation and HUD source publication.
--
-- Game objects are accessed only after CET onInit. Binding registration belongs
-- to module loading, when CET exposes registerHotkey.
-- VRCAM activation switches an entity component on.
-- entRenderToTextureCameraComponent.isEnabled is only reachable through the game's RTTI, which is
-- script-side, so the plugin asks and this mod does it. Everything else about the second view --
-- the view key, the render graph, the camera, the submit -- is native.
--
-- Files:
--   vrcam.json            which component to enable, and the full authored catalogue.
--                         Written by the launcher, read by both sides.
--   bridge/vrcam_enable.txt   native -> here: turn VRCAM on or off.
--   bridge/vrcam_active.txt   here -> native: the virtualCameraName actually enabled.
--
-- What used to live here and is gone: the testbed entity spawner, the file-IPC command channel to
-- the old dxgi.dll proxy, the RTT steer/capture harness and the ImGui overlay. The proxy they
-- talked to no longer exists, and the in-game overlay is drawn by the plugin now.

local VrcamSel = require("modules/vrcam_select")
local HudPanel = require("modules/hud_panel")
local VrOverlay = require("modules/vr_overlay")
local KeypadInput = require("modules/keypad_input")
local StoryAttention = require("modules/story_attention")

local Stereo = { ready = false, VrcamSel = VrcamSel, HudPanel = HudPanel, VrOverlay = VrOverlay, KeypadInput = KeypadInput, StoryAttention = StoryAttention }

-- Bind in CET > Bindings if vrcam.json was edited by hand.
registerHotkey("vrcam_reload_selection", "VRCAM: re-read vrcam.json", function()
    VrcamSel.reload()
    print("[Stereo.VRCAM] " .. VrcamSel.status())
end)

registerForEvent("onInit", function()
    VrcamSel.init()
    Stereo.ready = true
    print("[Stereo] " .. VrcamSel.status())

end)

registerForEvent("onUpdate", function(dt)
    if not Stereo.ready then return end
    VrcamSel.tick(dt)
    HudPanel.tick()
    VrOverlay.tick()
    KeypadInput.tick(dt, VrOverlay.active)
    StoryAttention.tick(dt, VrOverlay.active)
end)

registerForEvent("onShutdown", function()
    SetVRCetOverlayVisible(0)
    StoryAttention.shutdown();KeypadInput.shutdown();VrOverlay.shutdown();HudPanel.shutdown()
end)

registerForEvent("onOverlayOpen", function()
    SetVRCetOverlayVisible(1)
end)

registerForEvent("onOverlayClose", function()
    SetVRCetOverlayVisible(0)
end)

return Stereo
