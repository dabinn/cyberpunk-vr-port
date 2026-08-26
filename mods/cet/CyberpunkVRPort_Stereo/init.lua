-- CyberpunkVRPort_Stereo — the Lua half of the VRCAM view.
--
-- It owns the script-side VRCAM lifecycle: spawning the static world entity through Codeware and
-- switching one RTT component on. Everything after the authored camera component -- view key,
-- render graph, capture and submit -- remains native.
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

-- CET hotkeys must be registered at the mod root, outside event callbacks.
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
end)

registerForEvent("onShutdown", function()
    VrcamSel.shutdown()
    Stereo.ready = false
end)

return Stereo
