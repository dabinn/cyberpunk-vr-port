-- CyberpunkVRPort_Stereo — the Lua half of the VRCAM view.
--
-- It exists for exactly one thing the native plugin cannot do: switching an entity component on.
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

local Stereo = { ready = false, VrcamSel = VrcamSel }

registerForEvent("onInit", function()
    VrcamSel.init()
    Stereo.ready = true
    print("[Stereo] " .. VrcamSel.status())

    -- Bind in CET > Bindings if you edit vrcam.json by hand. The launcher's own writes are picked
    -- up on the next load anyway; this is for changing the pick without leaving the game.
    registerHotkey("vrcam_reload_selection", "VRCAM: re-read vrcam.json", function()
        VrcamSel.reload()
        print("[Stereo.VRCAM] " .. VrcamSel.status())
    end)
end)

registerForEvent("onUpdate", function(dt)
    if not Stereo.ready then return end
    VrcamSel.tick(dt)
end)

registerForEvent("onOverlayOpen", function()
    SetVRCetOverlayVisible(1)
end)

registerForEvent("onOverlayClose", function()
    SetVRCetOverlayVisible(0)
end)

return Stereo
