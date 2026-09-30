// OverlayDebugDraw -- the two things drawn ON the world rather than in a panel.
//
// The hand locator answers "where does the plugin think the hand is", which is the first question in
// every hand or weapon problem and cannot be answered from a log: it is a position, and a position is
// read by looking at it.
//
// The barrel crosshair draws from g_lastLocateQuat -- the quaternion the LOCATE site published, not the
// composed one. That is deliberate: it shows where the engine's camera is pointing, so a disagreement
// between the crosshair and where a shot goes is exactly the diagnosis wanted.

#include "Overlay/ImGuiOverlay.hpp"
#include "Overlay/LiveControlsUi.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include "Core/LiveControls.hpp"
#include "Core/VrCoreShared.hpp"
#include "Utils/SharedSlots.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include "im3d.h"
#include "Camera/CameraLink.hpp"   // cvr::camera::BarrelFrameRead
#include "Overlay/OverlayInternal.hpp"

extern volatile int g_verboseLog; // per-frame log spam toggle (default off)
extern void Log(const char* fmt, ...);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern volatile float g_lastLocateQuat[4];
extern "C" int   CyberpunkVR_StereoModuleEnable;   // vr_core.cpp: did we install at all
extern "C" int   CyberpunkVR_StereoModuleLoaded;
extern "C" int32_t CyberpunkVR_StereoLog;
extern "C" int      CyberpunkVR_StereoSubmit;              // openxr_frameloop.cpp
extern "C" int32_t  CyberpunkVR_StereoEyeCapture;
extern "C" uint32_t CyberpunkVR_StereoEyeMaxAgeMs;
extern "C" uint32_t CyberpunkVR_DebugVrcamEyeAgeMs;        // 0xFFFFFFFF = never produced
extern "C" unsigned long long CyberpunkVR_DebugStereoEyeSubmits;
extern "C" int32_t CyberpunkVR_StableCopy;
extern "C" int32_t CyberpunkVR_StableFromTonemap;
extern "C" uint64_t CyberpunkVR_DebugStableCopies;
extern "C" uint64_t CyberpunkVR_DebugStableSkips;
extern "C" int32_t CyberpunkVR_VrcamDlss;
extern "C" int32_t CyberpunkVR_ForceVrcamCam;
extern "C" uint32_t CyberpunkVR_VrcamEnabled;
extern "C" void        CyberpunkVR_SetVrcamEnabled(uint32_t on);
extern "C" const char* CyberpunkVR_VrcamComponentName();
extern "C" const char* CyberpunkVR_VrcamCameraName();
extern "C" uint32_t CyberpunkVR_MirrorOutput;
extern "C" uint64_t CyberpunkVR_DebugVrcamNodeHits;   // 0 => the second view never dispatched
extern "C" uint64_t CyberpunkVR_DebugMirrorRtvHits;
extern "C" int CyberpunkVR_IsVrcamViewActive();
extern "C" float CyberpunkVR_DebugMainProjYY;
extern "C" float CyberpunkVR_DebugMainCamFov;
extern "C" float CyberpunkVR_MainAdsZoomFactor;
extern "C" float CyberpunkVR_DebugVrcamWantFov;
extern "C" float CyberpunkVR_DebugVrcamBaseFov;
extern "C" int32_t  CyberpunkVR_ProfEnable;
extern "C" double   CyberpunkVR_ProfFrameMs;
extern "C" double   CyberpunkVR_ProfDispMainMs;
extern "C" double   CyberpunkVR_ProfDispVrcamMs;
extern "C" uint32_t CyberpunkVR_ProfDispMainNodes;
extern "C" uint32_t CyberpunkVR_ProfDispVrcamNodes;
extern "C" void     CyberpunkVR_ProfDumpNodes();
extern "C" int      CyberpunkVR_ProfSnapshotNodes(uint32_t* rva, double* msv, double* msm,
                                                  uint32_t* cv, uint32_t* cm, int maxn);
extern "C" const char* CyberpunkVR_ProfNodeName(uint32_t rva);
extern "C" uint64_t CyberpunkVR_DebugViewKeyMainNodes;
extern "C" uint64_t CyberpunkVR_DebugViewKeyOtherNodes;
extern volatile int32_t g_lastLocatePosFP[3];
extern "C" float CyberpunkVRPort_HalfIpd();
extern "C" float GetGameRenderFovDeg();
extern "C" int CyberpunkVR_MainIsRightEye;
extern "C" UINT GetForcedDisplayModeWidth();
extern "C" UINT GetForcedDisplayModeHeight();

namespace overlay {

void DrawHandLocatorOverlay() {
    if (!g_drawHandLocator) return;

    OpenXRHeadPose head{};
    OpenXRHeadPose hands[2]{};
    if(!OpenXRManager::Get().GetPublishedHandFrame(&head,hands))return;
    const auto& left=hands[0];const auto& right=hands[1];
    const bool hasLeft=left.valid,hasRight=right.valid;
    if (!hasLeft && !hasRight) return;

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 1.0f || displaySize.y <= 1.0f) return;

    // BACKGROUND, not foreground: this geometry is projected for one eye, and the second-eye
    // overlay pass (OverlayRecordIntoTarget) skips the background list for exactly that reason.
    // Being under the menu window rather than over it is the incidental other effect, and the
    // better of the two orders anyway.
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    Im3d::AppData& appData = Im3d::GetAppData();
    appData.m_viewOrigin = Im3d::Vec3(0.0f, 0.0f, 0.0f);
    appData.m_viewDirection = Im3d::Vec3(0.0f, 0.0f, -1.0f);
    appData.m_worldUp = Im3d::Vec3(0.0f, 1.0f, 0.0f);
    appData.m_viewportSize = Im3d::Vec2(displaySize.x, displaySize.y);
    appData.m_deltaTime = 1.0f / 90.0f;
    appData.m_projOrtho = false;
    // Same GAME-RENDER vertical tan as the point projectors (not the lens V-FOV),
    // so Im3d size attenuation agrees with where the points actually land.
    {
        float tx = 0.0f, ty = tanf(50.0f * (3.1415926535f / 180.0f));
        GetOverlayProjTans(displaySize, &tx, &ty);
        appData.m_projScaleY = ty;
    }
    Im3d::NewFrame();

    const auto drawEndpointLabel = [&](const OpenXRHeadPose& handPose, float x, float y, float z,
        ImU32 color, const char* text, const ImVec2& offset) {
        ImVec2 screen{};
        if (!ProjectHandLocalPoint(head, handPose, x, y, z, displaySize, &screen)) return;
        drawList->AddText(ImVec2(screen.x + offset.x, screen.y + offset.y), color, text);
    };

    const auto drawHandWire = [&](const OpenXRHeadPose& handPose, ImU32 bodyColor, const char* name, bool isLeftHand) {
        const ImU32 rightColor = IM_COL32(255, 80, 80, 255);
        const ImU32 upColor = IM_COL32(80, 255, 80, 255);
        const ImU32 fwdColor = IM_COL32(80, 160, 255, 255);
        const ImU32 textColor = IM_COL32(255, 255, 255, 255);

        const float s = g_handLocatorScale;
        const float wristHalfW = 0.026f * s;
        const float palmHalfW = 0.034f * s;
        const float wristY = -0.055f * s;
        const float palmMidY = -0.018f * s;
        const float knuckleY = 0.045f * s;
        const float fwdLen = 0.180f * s;

        // Hand abstract frame:
        //   +X = thumb side
        //   +Y = fingers direction
        //   +Z = palm normal
        // Empirical grip-pose basis from the current runtime/controller:
        //   +X = left, +Y = forward, -Z = up
        // Desired hand mapping:
        //   thumb   -> up      => -Z
        //   fingers -> forward => +Y
        //   palm    -> left/right => +X for right hand, -X for left hand
        const auto mapHandPoint = [&](float hx, float hy, float hz, float* cx, float* cy, float* cz) {
            if (isLeftHand) {
                *cx = -hz;
            } else {
                *cx = hz;
            }
            *cy = -hy;
            *cz = -hx;
        };

        const auto bone = [&](float ax, float ay, float az, float bx, float by, float bz, ImU32 color, float thickness) {
            float cax = 0.0f, cay = 0.0f, caz = 0.0f;
            float cbx = 0.0f, cby = 0.0f, cbz = 0.0f;
            mapHandPoint(ax, ay, az, &cax, &cay, &caz);
            mapHandPoint(bx, by, bz, &cbx, &cby, &cbz);
            DrawProjectedBone(drawList, head, handPose, cax, cay, caz, cbx, cby, cbz, displaySize, color, thickness);
        };

        const auto finger = [&](float baseX, float baseY, float midX, float midY, float tipX, float tipY) {
            bone(baseX, baseY, 0.0f, midX, midY, -0.010f * s, bodyColor, 2.0f);
            bone(midX, midY, -0.010f * s, tipX, tipY, -0.020f * s, bodyColor, 2.0f);
        };

        // Wrist and palm outline.
        bone(-wristHalfW, wristY, 0.0f, wristHalfW, wristY, 0.0f, bodyColor, 2.0f);
        bone(-wristHalfW, wristY, 0.0f, -palmHalfW, palmMidY, 0.0f, bodyColor, 2.0f);
        bone(wristHalfW, wristY, 0.0f, palmHalfW, palmMidY, 0.0f, bodyColor, 2.0f);
        bone(-palmHalfW, palmMidY, 0.0f, -palmHalfW, knuckleY, 0.0f, bodyColor, 2.0f);
        bone(palmHalfW, palmMidY, 0.0f, palmHalfW, knuckleY, 0.0f, bodyColor, 2.0f);
        bone(-palmHalfW, knuckleY, 0.0f, palmHalfW, knuckleY, 0.0f, bodyColor, 2.0f);

        // Fingers.
        finger(-0.024f * s, knuckleY, -0.026f * s, 0.074f * s, -0.026f * s, 0.096f * s); // pinky
        finger(-0.010f * s, knuckleY, -0.011f * s, 0.086f * s, -0.011f * s, 0.116f * s); // ring
        finger(0.006f * s, knuckleY, 0.006f * s, 0.094f * s, 0.006f * s, 0.128f * s);     // middle
        finger(0.022f * s, knuckleY, 0.023f * s, 0.086f * s, 0.023f * s, 0.116f * s);     // index

        // Thumb: always +X in abstract hand space. Handedness is handled by mapHandPoint.
        bone(0.020f * s, -0.004f * s, 0.0f,
             0.046f * s, 0.014f * s, -0.006f * s,
             bodyColor, 2.0f);
        bone(0.046f * s, 0.014f * s, -0.006f * s,
             0.065f * s, 0.035f * s, -0.016f * s,
             bodyColor, 2.0f);

        // Axes: right/left, up/down, forward.
        bone(-0.060f * s, 0.0f, 0.0f, 0.060f * s, 0.0f, 0.0f, rightColor, 2.0f);
        bone(0.0f, -0.060f * s, 0.0f, 0.0f, 0.060f * s, 0.0f, upColor, 2.0f);
        bone(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -fwdLen, fwdColor, 3.0f);

        // Forward arrow head.
        bone(0.0f, 0.0f, -fwdLen, 0.020f * s, 0.0f, -(fwdLen - 0.028f * s), fwdColor, 2.0f);
        bone(0.0f, 0.0f, -fwdLen, -0.020f * s, 0.0f, -(fwdLen - 0.028f * s), fwdColor, 2.0f);
        bone(0.0f, 0.0f, -fwdLen, 0.0f, 0.020f * s, -(fwdLen - 0.028f * s), fwdColor, 2.0f);
        bone(0.0f, 0.0f, -fwdLen, 0.0f, -0.020f * s, -(fwdLen - 0.028f * s), fwdColor, 2.0f);

        // Center dot.
        ImVec2 center{};
        if (ProjectHandLocalPoint(head, handPose, 0.0f, 0.0f, 0.0f, displaySize, &center)) {
            drawList->AddCircleFilled(center, 4.0f, bodyColor);
            drawList->AddText(ImVec2(center.x + 8.0f, center.y - 20.0f), bodyColor, name);
        }

        drawEndpointLabel(handPose, 0.070f * s, 0.0f, 0.0f, textColor, "R", ImVec2(4.0f, -6.0f));
        drawEndpointLabel(handPose, -0.070f * s, 0.0f, 0.0f, textColor, "L", ImVec2(-10.0f, -6.0f));
        drawEndpointLabel(handPose, 0.0f, 0.070f * s, 0.0f, textColor, "U", ImVec2(-4.0f, -14.0f));
        drawEndpointLabel(handPose, 0.0f, -0.070f * s, 0.0f, textColor, "D", ImVec2(-4.0f, 2.0f));
        drawEndpointLabel(handPose, 0.0f, 0.0f, -(fwdLen + 0.020f * s), textColor, "F", ImVec2(4.0f, -6.0f));
    };

    if (hasLeft) {
        if (g_drawHandProxy3D) {
            EmitHandProxyIm3d(left, true, g_handLocatorScale, g_drawHandDebugAxes);
        }
        if (g_drawHandDebugAxes || !g_drawHandProxy3D) {
            drawHandWire(left, IM_COL32(0, 220, 255, 255), "LEFT", true);
        }
    }
    if (hasRight) {
        if (g_drawHandProxy3D) {
            EmitHandProxyIm3d(right, false, g_handLocatorScale, g_drawHandDebugAxes);
        }
        if (g_drawHandDebugAxes || !g_drawHandProxy3D) {
            drawHandWire(right, IM_COL32(255, 180, 0, 255), "RIGHT", false);
        }
    }

    Im3d::EndFrame();
    if (g_drawHandProxy3D) {
        RenderIm3dToDrawList(drawList, displaySize);
    }

    // Aim ray: a long bright line down each hand's forward (abstract -Z), i.e. the same
    // direction as the small "F" axis but extended several meters, so the user can see
    // in-headset where the controller / held weapon is pointing. Sampled in many short
    // segments so it still draws correctly when part of the ray falls behind the eye.
    if (g_drawAimRay) {
        // Laser sight: a long forward ray down each hand's pointing axis. The abstract hand
        // frame's FORWARD is +Y (user-confirmed in-headset: the bright +Y axis ran from the
        // fingers down the gun barrel; -Z had pointed sideways from the palm). Sampled in many
        // short segments so it still draws when part of the ray falls behind the eye.
        const float maxLen = (g_aimRayLenM > 0.5f) ? g_aimRayLenM : 8.0f;
        const auto laser = [&](const OpenXRHeadPose& hp, bool isLeft, ImU32 color, float thick) {
            const int N = 40;
            ImVec2 prev{};
            bool havePrev = false;
            for (int i = 0; i <= N; ++i) {
                const float t = (static_cast<float>(i) / static_cast<float>(N)) * maxLen;
                const Im3d::Vec3 p = AbstractHandPointToHeadSpace(hp, isLeft, 0.0f, t, 0.0f); // +Y = forward
                ImVec2 sc{};
                if (ProjectIm3dPointToScreen(p, displaySize, &sc)) {
                    if (havePrev) drawList->AddLine(prev, sc, color, thick);
                    prev = sc;
                    havePrev = true;
                } else {
                    havePrev = false;
                }
            }
            // Aim dot at ~4 m so "where it points" reads at a glance.
            const Im3d::Vec3 dot = AbstractHandPointToHeadSpace(hp, isLeft, 0.0f, 4.0f, 0.0f);
            ImVec2 dc{};
            if (ProjectIm3dPointToScreen(dot, displaySize, &dc)) {
                drawList->AddCircleFilled(dc, 6.0f, color);
                drawList->AddCircle(dc, 10.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
            }
        };
        if (hasRight) laser(right, false, IM_COL32(80, 255, 80, 255), 3.0f); // weapon hand: green
        if (hasLeft)  laser(left, true, IM_COL32(0, 200, 255, 160), 2.0f);   // off hand: cyan, dimmer
    }
}

// The barrel dot, in NDC, for the eye the overlay cannot reach. Written by DrawBarrelCrosshair
// below; read by the VRCAM eye composite (openxr_capture.cpp) and by the desktop mirror
// (sync_stereo.cpp). The tick is what makes a stale value harmless: the consumers ignore it once
// it stops being refreshed, so holstering the weapon removes the dot instead of freezing it.
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotNdcX = 0.0f;
// The SECOND eye needs its own value. The dot marks where the bullet goes, and the bullet
// line passes through the FIRST eye (the game aligns the weapon to its camera, which is
// MAIN). From the second eye, one IPD off that line, the same world point lies at a
// different angle -- exactly the parallax the reticle is now zeroed for. Without this the
// dot and the reticle disagree by that angle in the second eye, and the instrument would
// contradict the thing it is there to measure.
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotNdcX2 = 0.0f;
// Must match the distance sight_reflex_ps.hlsl was compiled with (build_sight.ps1 arg 2).
// 0 = the dot marks the BORE DIRECTION in both eyes, with no per-eye parallax. Back to that
// until the instance measurement says whether the two views place the weapon differently:
// a correction derived from the wrong model is worse than none, because it moves the very
// reference the reticle is being judged against.
extern "C" __declspec(dllexport) float    CyberpunkVR_SightZeroMeters = 20.0f;   // dot parallax distance
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotNdcY = 0.0f;
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotRadiusPx = 3.0f;
// Range at which the dot marks the bullet's line. A mark can only be exact at one distance --
// that is true of every sight -- but at any finite value both eyes agree on the same world point,
// which is the part that was broken.
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotDistM = 20.0f;
extern "C" __declspec(dllexport) int      CyberpunkVR_BarrelDotEyeSign = 1;
// Straight nudge of the SECOND eye's dot, in NDC (screen half-widths). Negative = left.
// 0.0025 is about 3 mrad at this field of view, i.e. 65 mm at 20 m -- the size of the error we
// have been chasing. Whatever value this lands on IS that error, measured, and the same number
// then applies to the reticle.
extern "C" __declspec(dllexport) float    CyberpunkVR_BarrelDotOffX2 = 0.0f;
// 0 = one projection for both eyes plus a constant parallax on the second (simple, steady).
// 1 = a real world point projected per eye (exact, but only as steady as the muzzle transform).
extern "C" __declspec(dllexport) int      CyberpunkVR_BarrelDotWorld = 1;
extern "C" int CyberpunkVR_MainIsRightEye;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_BarrelDotTick = 0;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_BarrelDotSecondEye = 1;
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_BarrelDotSecondVisible = 1;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugBarrelDotDraws = 0;
extern "C" int CyberpunkVR_WeaponClass;

// EXACT barrel crosshair. The plugin publishes the weapon muzzle WORLD forward (shared[24..26]); we
// rotate it into the located game camera's local frame (inv(camQuat) * fwd) and project that
// direction with the SAME view/FOV the eye renders through -> the dot lands exactly where the bullet
// goes (both derive from the same muzzle + camera). No controller-space guessing.
void DrawRadialLaserSpot(ImDrawList* drawList, const ImVec2& center, float coreRadiusPx) {
    if (!drawList || !(coreRadiusPx > 0.0f)) return;

    constexpr float kOuterCoreRatio = 2.5f;
    constexpr float kLn2 = 0.69314718f;
    constexpr float kRingCoreDistances[] = {
        0.25f, 0.50f, 0.75f, 1.00f, 1.30f, 1.65f, 2.00f, 2.25f, 2.50f
    };
    constexpr int kRingCount = static_cast<int>(IM_ARRAYSIZE(kRingCoreDistances));
    const float outerRadiusPx = coreRadiusPx * kOuterCoreRatio;
    const float segmentEstimate = 3.1415926535f /
        acosf(std::clamp(1.0f - 0.30f / std::max(outerRadiusPx, 0.31f), -1.0f, 1.0f));
    const int segments = std::clamp(static_cast<int>(ceilf(segmentEstimate)), 12, 128);
    const int vertexCount = 1 + kRingCount * segments;
    const int indexCount = segments * 3 + (kRingCount - 1) * segments * 6;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

    auto colorAt = [&](float coreDistance) -> ImU32 {
        const float intensity = expf(-kLn2 * coreDistance * coreDistance);
        const float fadeT = std::clamp((coreDistance - 2.25f) / 0.25f, 0.0f, 1.0f);
        const float smoothFade = fadeT * fadeT * (3.0f - 2.0f * fadeT);
        const float alpha = intensity * (1.0f - smoothFade);
        const float hotCore = expf(-4.0f * coreDistance * coreDistance);
        const float green = 0.045f + (0.82f - 0.045f) * hotCore;
        const float blue = 0.045f + (0.68f - 0.045f) * hotCore;
        return ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, green, blue, alpha));
    };

    drawList->PrimReserve(indexCount, vertexCount);
    const ImDrawIdx base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
    drawList->PrimWriteVtx(center, uv, colorAt(0.0f));
    for (int ring = 0; ring < kRingCount; ++ring) {
        const float coreDistance = kRingCoreDistances[ring];
        const float ringRadiusPx = coreRadiusPx * coreDistance;
        const ImU32 color = colorAt(coreDistance);
        for (int segment = 0; segment < segments; ++segment) {
            const float angle = (2.0f * 3.1415926535f * static_cast<float>(segment)) /
                                static_cast<float>(segments);
            drawList->PrimWriteVtx(
                ImVec2(center.x + cosf(angle) * ringRadiusPx,
                       center.y + sinf(angle) * ringRadiusPx),
                uv, color);
        }
    }

    const ImDrawIdx firstRing = static_cast<ImDrawIdx>(base + 1);
    for (int segment = 0; segment < segments; ++segment) {
        const ImDrawIdx next = static_cast<ImDrawIdx>((segment + 1) % segments);
        drawList->PrimWriteIdx(base);
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(firstRing + segment));
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(firstRing + next));
    }
    for (int ring = 1; ring < kRingCount; ++ring) {
        const ImDrawIdx inner = static_cast<ImDrawIdx>(base + 1 + (ring - 1) * segments);
        const ImDrawIdx outer = static_cast<ImDrawIdx>(base + 1 + ring * segments);
        for (int segment = 0; segment < segments; ++segment) {
            const ImDrawIdx next = static_cast<ImDrawIdx>((segment + 1) % segments);
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(inner + segment));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(outer + segment));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(outer + next));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(inner + segment));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(outer + next));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(inner + next));
        }
    }
}

// The compact ADS-camera panel, drawn with the game running so the numbers can be read while
// aiming rather than reconstructed from a log afterwards. A window, not the background list, so it
// reaches both eyes through the second-eye overlay pass. Off by default (dabinn, TofuExpress
// ec1aa65c): it is an instrument, not a HUD.
void DrawCompactAdsCameraTelemetry() {
    if (!g_showCompactAdsTelemetry) return;

    AdsCameraTelemetryUiState t{};
    GetAdsCameraTelemetryUiState(&t);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(display.x * g_compactAdsTelemetryX, display.y * g_compactAdsTelemetryY),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("ADS camera telemetry##compact", nullptr, flags)) {
        if (!t.available) {
            ImGui::TextUnformatted("ADS CAM  waiting for gameplay camera...");
        } else {
            const ImVec4 stateColor = t.aiming
                ? ImVec4(1.0f, 0.78f, 0.24f, 1.0f)
                : ImVec4(0.45f, 0.9f, 0.55f, 1.0f);
            const float mainAdsZoom = CyberpunkVR_MainAdsZoomFactor;
            const float sharedZoomRaw = OpenXRManager::Get().GetSharedSlot(28);
            const float finalZoom = (std::isfinite(mainAdsZoom) && mainAdsZoom > 0.0f)
                ? mainAdsZoom : 1.0f;
            ImGui::TextColored(stateColor, "ADS CAM  %s", t.aiming ? "ON" : "HIP");
            ImGui::Text("zoom MAIN %.3fx   final %.3fx", mainAdsZoom, finalZoom);
            ImGui::TextDisabled("shared[28] %.3fx   diagnostic only", sharedZoomRaw);
            if (!t.baselineValid) {
                ImGui::TextUnformatted("Hold hip-fire briefly to capture baseline");
            } else {
                ImGui::Text("delta cm  R %+6.2f  F %+6.2f  U %+6.2f",
                            t.deltaRight * 100.0f, t.deltaForward * 100.0f, t.deltaUp * 100.0f);
                ImGui::Text("peak  cm  R %6.2f  F %6.2f  U %6.2f   n=%u",
                            t.peakRight * 100.0f, t.peakForward * 100.0f,
                            t.peakUp * 100.0f, t.samples);
                ImGui::TextDisabled("raw   cm  R %+6.2f  F %+6.2f  U %+6.2f",
                                    t.residualRight * 100.0f, t.residualForward * 100.0f,
                                    t.residualUp * 100.0f);
            }
        }
    }
    ImGui::End();
}

void DrawBarrelCrosshair() {
    int laserDotMode = g_liveControls.xrLaserDotMode;
    if (laserDotMode < 0 || laserDotMode > 2) laserDotMode = 1;
    CyberpunkVR_BarrelDotWorld = laserDotMode;

    const bool surfaceMode = laserDotMode == 2;
    constexpr unsigned long long kLaserWeaponFreshMs = 250;
    const unsigned long long nowMs = GetTickCount64();
    const unsigned long long weaponUpdatedMs =
        g_laserRangedWeaponUpdatedMs.load(std::memory_order_relaxed);
    const bool rangedWeaponActive =
        g_laserRangedWeaponActive.load(std::memory_order_relaxed) != 0 &&
        weaponUpdatedMs != 0 && nowMs - weaponUpdatedMs <= kLaserWeaponFreshMs;
    const bool worldMapOpen = OpenXRManager::Get().GetSharedSlot(81) != 0.0f;
    const bool deviceScreenOpen =
        OpenXRManager::Get().GetSharedSlot(vrshared::kDeviceScreenOpen) > 0.5f;
    const bool gameUiActive =
        g_menuModeValue != 0 || worldMapOpen ||
        g_uiPopupOpen.load(std::memory_order_relaxed) != 0 || deviceScreenOpen;
    const bool hideForAds = g_drawBarrelCross && g_liveControls.xrHideLaserDotAds != 0 && g_isAiming;
    const bool drawLaser = g_drawBarrelCross && rangedWeaponActive && !gameUiActive && !hideForAds;
    const bool raycastActive = drawLaser && surfaceMode;

    // CET owns every physics query. Publish the common mode/UI gate before any early return so
    // disabling the dot also stops the game-thread ray and LOS work.
    OpenXRManager::Get().SetSharedSlot(vrshared::kBarrelRayActive, raycastActive ? 1.0f : 0.0f);

    // Legacy publication feeds only the desktop second-eye mirror now. The HMD second eye gets a
    // dedicated ImGui list which is reset every frame.
    CyberpunkVR_BarrelDotTick = 0;
    CyberpunkVR_BarrelDotSecondVisible = 0;
    if (!drawLaser) return;

    cvr::camera::BarrelFrame bf{};
    if (!cvr::camera::BarrelFrameRead(&bf)) return;
    const float mfx = bf.muzzleFwd[0], mfy = bf.muzzleFwd[1], mfz = bf.muzzleFwd[2];
    if (mfx*mfx + mfy*mfy + mfz*mfz < 0.25f) return;
    const float cqx = bf.camQuat[0], cqy = bf.camQuat[1], cqz = bf.camQuat[2], cqw = bf.camQuat[3];

    // Muzzle position is published from the game side and can momentarily read as zero during an
    // otherwise valid frame. Keep the last real world-space sample; a muzzle does not teleport to
    // the origin just because one shared-memory sample was empty.
    static float s_mp[3] = {0.0f, 0.0f, 0.0f};
    {
        const float rx = OpenXRManager::Get().GetSharedSlot(200);
        const float ry = OpenXRManager::Get().GetSharedSlot(201);
        const float rz = OpenXRManager::Get().GetSharedSlot(202);
        if (rx*rx + ry*ry + rz*rz > 1.0f) {
            s_mp[0] = rx; s_mp[1] = ry; s_mp[2] = rz;
        }
    }
    const float mpx = s_mp[0], mpy = s_mp[1], mpz = s_mp[2];

    // v0.1.7 publishes this as the rendered HEAD CENTRE. Eye offsets therefore come from the same
    // XrFrameSlot the submit path will use for the image, instead of adding/subtracting camera-right.
    const float hcx = static_cast<float>(g_lastLocatePosFP[0]) / 131072.0f;
    const float hcy = static_cast<float>(g_lastLocatePosFP[1]) / 131072.0f;
    const float hcz = static_cast<float>(g_lastLocatePosFP[2]) / 131072.0f;
    const bool haveWorld = (mpx * mpx + mpy * mpy + mpz * mpz) > 1.0f &&
                           (hcx * hcx + hcy * hcy + hcz * hcz) > 1.0f;

    // CET owns the surface query and publishes one seqlocked hit+visibility packet. Freshness is
    // driven by SEQUENCE ADVANCE, not by the packet merely remaining valid, so a stopped CET tick
    // cannot leave the old hit glued to a wall.
    static uint32_t s_lastRaySeq = 0;
    static uint64_t s_lastRayTick = 0;
    float hitx = 0.0f, hity = 0.0f, hitz = 0.0f, hitValid = 0.0f;
    float mainVisible = 1.0f, secondVisible = 1.0f;
    uint32_t raySeq = 0;
    bool rayPacket = false;
    for (int attempt = 0; surfaceMode && attempt < 3 && !rayPacket; ++attempt) {
        const uint32_t s0 = static_cast<uint32_t>(
            OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRaySeq));
        if (s0 == 0u || (s0 & 1u)) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const float x = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRayHitX + 0);
        const float y = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRayHitX + 1);
        const float z = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRayHitX + 2);
        const float v = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRayHitValid);
        const float vm = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelMainVisible);
        const float vs = OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelSecondVisible);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s1 = static_cast<uint32_t>(
            OpenXRManager::Get().GetSharedSlot(vrshared::kBarrelRaySeq));
        if (s0 == s1 && !(s1 & 1u)) {
            hitx = x; hity = y; hitz = z; hitValid = v;
            mainVisible = vm; secondVisible = vs;
            raySeq = s1; rayPacket = true;
        }
    }
    if (rayPacket && raySeq != s_lastRaySeq) {
        s_lastRaySeq = raySeq;
        s_lastRayTick = GetTickCount64();
    }
    const uint64_t nowTick = GetTickCount64();
    const bool rayFresh = rayPacket && s_lastRayTick != 0 && (nowTick - s_lastRayTick) <= 20u;
    const bool haveRayHit = surfaceMode && rayFresh && hitValid > 0.5f &&
                            (hitx * hitx + hity * hity + hitz * hitz) > 1.0f;

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 1.0f || displaySize.y <= 1.0f) return;

    // User radius is a world-space beam core. Modes 0/1 use a stable 10 m reference size. In
    // surface mode optional distance scaling grows the physical footprint with beam travel while
    // perspective turns it back into an angular screen radius.
    constexpr float kLaserDotReferenceDepthM = 10.0f;
    constexpr float kLaserDotHalfDivergenceRad = 0.00075f;
    constexpr float kLaserDotMissAngularRadiusRad = 0.0005f;
    constexpr float kLaserDotSafetyMaxAngularRadius = 1.0f;
    const float radiusMm = std::clamp(
        static_cast<float>(g_liveControls.xrLaserDotRadiusMm), 1.0f, 50.0f);
    const float baseRadiusM = radiusMm * 0.001f;
    const bool scaleWithDistance = surfaceMode && g_liveControls.xrLaserDotScaleWithDistance != 0;
    float angularRadius = baseRadiusM / kLaserDotReferenceDepthM;
    if (scaleWithDistance) {
        if (!haveRayHit) {
            angularRadius = tanf(kLaserDotMissAngularRadiusRad);
        } else {
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            RotateVectorByQuaternion(hitx - hcx, hity - hcy, hitz - hcz,
                                     -cqx, -cqy, -cqz, cqw, &lx, &ly, &lz);
            if (ly > 0.01f) {
                float beamTravelM = ly;
                if (haveWorld) {
                    const float bdx = hitx - mpx;
                    const float bdy = hity - mpy;
                    const float bdz = hitz - mpz;
                    beamTravelM = sqrtf(bdx * bdx + bdy * bdy + bdz * bdz);
                }
                const float beamSpreadM = beamTravelM * tanf(kLaserDotHalfDivergenceRad);
                const float surfaceRadiusM =
                    sqrtf(baseRadiusM * baseRadiusM + beamSpreadM * beamSpreadM);
                angularRadius = surfaceRadiusM / ly;
            } else {
                angularRadius = kLaserDotSafetyMaxAngularRadius;
            }
        }
    }
    float rad = 3.0f;
    float tanHalfX = 0.0f, tanHalfY = 0.0f;
    if (GetOverlayProjTans(displaySize, &tanHalfX, &tanHalfY) && tanHalfX > 0.0001f) {
        angularRadius = std::clamp(angularRadius, 0.0f, kLaserDotSafetyMaxAngularRadius);
        rad = angularRadius * (displaySize.x * 0.5f) / tanHalfX;
    }

    // Resolve the two physical eye origins from the coherent XR frame slot for THIS image. At this
    // point HookedPresent has not called OnPresent yet, so presentCount+1 is exactly the serial the
    // submit path will consume moments later. The relative offsets are converted from LOCAL-space
    // XR coordinates into head-local axes, scaled to the port's desired IPD, then rotated by the
    // rendered game camera quaternion and anchored at the rendered head centre.
    float eyeWorld[2][3]{}; // [OpenXR view index: 0=left, 1=right]
    bool haveCoherentEyes = false;
    {
        auto& xr = OpenXRManager::Get();
        OpenXRManager::XrFrameSlot slot{};
        const uint64_t imageSerial = xr.GetPresentCount() + 1u;
        if (xr.GetFrameSlot(imageSerial, &slot)) {
            float eyeHead[2][3]{};
            for (int eye = 0; eye < 2; ++eye) {
                const float dx = slot.viewPose[eye].position.x - slot.headPoseLocal.position.x;
                const float dy = slot.viewPose[eye].position.y - slot.headPoseLocal.position.y;
                const float dz = slot.viewPose[eye].position.z - slot.headPoseLocal.position.z;
                float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                const XrQuaternionf& hq = slot.headPoseLocal.orientation;
                RotateVectorByQuaternion(dx, dy, dz, -hq.x, -hq.y, -hq.z, hq.w,
                                         &hx, &hy, &hz);
                // XR head axes: +X right, +Y up, -Z forward.
                // Game camera axes: +X right, +Y forward, +Z up.
                eyeHead[eye][0] = hx;
                eyeHead[eye][1] = -hz;
                eyeHead[eye][2] = hy;
            }

            const float sx = eyeHead[1][0] - eyeHead[0][0];
            const float sy = eyeHead[1][1] - eyeHead[0][1];
            const float sz = eyeHead[1][2] - eyeHead[0][2];
            const float rawIpd = sqrtf(sx*sx + sy*sy + sz*sz);
            float desiredHalfIpd = CyberpunkVRPort_HalfIpd();
            if (!(desiredHalfIpd > 0.0001f)) desiredHalfIpd = xr.GetSharedSlot(95);
            if (!(desiredHalfIpd > 0.0001f)) desiredHalfIpd = rawIpd * 0.5f;
            if (rawIpd > 0.001f && desiredHalfIpd > 0.0001f) {
                const float scale = (2.0f * desiredHalfIpd) / rawIpd;
                for (int eye = 0; eye < 2; ++eye) {
                    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
                    RotateVectorByQuaternion(
                        eyeHead[eye][0] * scale,
                        eyeHead[eye][1] * scale,
                        eyeHead[eye][2] * scale,
                        cqx, cqy, cqz, cqw, &ox, &oy, &oz);
                    eyeWorld[eye][0] = hcx + ox;
                    eyeWorld[eye][1] = hcy + oy;
                    eyeWorld[eye][2] = hcz + oz;
                }
                haveCoherentEyes = true;
            }
        }
    }

    const int mainView = CyberpunkVR_MainIsRightEye ? 1 : 0;
    const int secondView = 1 - mainView;
    const float* mainEye = haveCoherentEyes ? eyeWorld[mainView] : nullptr;
    const float* secondEye = haveCoherentEyes ? eyeWorld[secondView] : nullptr;

    // Publish the same image-coherent origins to CET for its per-eye LOS queries. If no coherent
    // frame slot is available, leave the sequence untouched; CET deliberately treats stale eyes as
    // visibility fail-open while continuing to provide the surface hit.
    if (mainEye && secondEye) {
        static uint32_t s_eyeSeq = 0;
        uint32_t nextEven = s_eyeSeq + 2u;
        if (nextEven >= 1000000u) nextEven = 2u;
        auto& xr = OpenXRManager::Get();
        xr.SetSharedSlot(vrshared::kBarrelEyeSeq, static_cast<float>(nextEven - 1u));
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) {
            xr.SetSharedSlot(vrshared::kBarrelMainEyeX + i, mainEye[i]);
            xr.SetSharedSlot(vrshared::kBarrelSecondEyeX + i, secondEye[i]);
        }
        std::atomic_thread_fence(std::memory_order_release);
        xr.SetSharedSlot(vrshared::kBarrelEyeSeq, static_cast<float>(nextEven));
        s_eyeSeq = nextEven;
    }

    auto projectWorldPointFromEye = [&](const float* eye, float px, float py, float pz,
                                        ImVec2* out) -> bool {
        if (!eye) return false;
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        RotateVectorByQuaternion(px - eye[0], py - eye[1], pz - eye[2],
                                 -cqx, -cqy, -cqz, cqw, &lx, &ly, &lz);
        return ProjectHeadSpacePointToScreen(lx, lz, -ly, displaySize, out);
    };

    auto finiteDotForEye = [&](const float* eye, ImVec2* out) -> bool {
        if (!haveWorld || !eye) return false;
        const float D = CyberpunkVR_BarrelDotDistM;
        if (!(D > 0.5f)) return false;
        return projectWorldPointFromEye(
            eye, mpx + mfx * D, mpy + mfy * D, mpz + mfz * D, out);
    };

    auto surfaceDotForEye = [&](const float* eye, ImVec2* out) -> bool {
        return haveRayHit && projectWorldPointFromEye(eye, hitx, hity, hitz, out);
    };

    // Mode 0 stays a steady bore-direction cue. Mode 1 projects a finite point on the muzzle ray.
    // Mode 2 uses the CET surface hit while fresh and falls back to the direction cue on a miss.
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    RotateVectorByQuaternion(mfx, mfy, mfz, -cqx, -cqy, -cqz, cqw, &vx, &vy, &vz);
    ImVec2 sc{};
    ImVec2 scSecond{};
    bool worldOk = false;
    if (laserDotMode == 1) {
        if (!mainEye || !secondEye) return;
        const bool mainOk = finiteDotForEye(mainEye, &sc);
        const bool secondOk = finiteDotForEye(secondEye, &scSecond);
        if (haveWorld && (!mainOk || !secondOk)) return;
        worldOk = mainOk && secondOk;
    } else if (surfaceMode && haveRayHit) {
        if (!mainEye || !secondEye) return;
        const bool mainOk = surfaceDotForEye(mainEye, &sc);
        const bool secondOk = surfaceDotForEye(secondEye, &scSecond);
        if (!mainOk || !secondOk) return;
        worldOk = true;
    }

    if (!worldOk) {
        if (!ProjectHeadSpacePointToScreen(vx, vz, -vy, displaySize, &sc)) return;
        scSecond = sc;
    }

    CyberpunkVR_BarrelDotNdcX = (sc.x / displaySize.x) * 2.0f - 1.0f;
    CyberpunkVR_BarrelDotNdcY = 1.0f - (sc.y / displaySize.y) * 2.0f;

    // Steady mode keeps the established finite-zero parallax without depending on a muzzle
    // position sample. Finite/surface modes already projected the actual world point per eye.
    if (laserDotMode == 0) {
        float thx = 0.0f, thy = 0.0f;
        float dx = 0.0f;
        const float zeroM = CyberpunkVR_SightZeroMeters;
        if (zeroM > 0.1f && GetOverlayProjTans(displaySize, &thx, &thy)) {
            const float ipd = OpenXRManager::Get().GetRuntimeIpd();
            if (ipd > 0.001f) {
                dx = -(ipd / zeroM) / thx;
                if (CyberpunkVR_MainIsRightEye) dx = -dx;
            }
        }
        CyberpunkVR_BarrelDotNdcX2 = CyberpunkVR_BarrelDotNdcX + dx + CyberpunkVR_BarrelDotOffX2;
    } else {
        CyberpunkVR_BarrelDotNdcX2 =
            ((scSecond.x / displaySize.x) * 2.0f - 1.0f) + CyberpunkVR_BarrelDotOffX2;
    }

    const ImVec2 secondEyeSc{
        (CyberpunkVR_BarrelDotNdcX2 + 1.0f) * displaySize.x * 0.5f,
        (1.0f - CyberpunkVR_BarrelDotNdcY) * displaySize.y * 0.5f};

    // RecordDot is retained for the desktop mirror and receives the full halo extent. Both HMD
    // eyes draw the same radial ImGui mesh from the core radius.
    CyberpunkVR_BarrelDotRadiusPx = rad * 2.5f;
    CyberpunkVR_BarrelDotTick = GetTickCount64();
    const bool mainDotVisible = !surfaceMode || !rayFresh || mainVisible > 0.5f;
    CyberpunkVR_BarrelDotSecondVisible =
        (!surfaceMode || !rayFresh || secondVisible > 0.5f) ? 1 : 0;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl && mainDotVisible) DrawRadialLaserSpot(dl, sc, rad);
    if (g_secondEyeWorldDrawList && CyberpunkVR_BarrelDotSecondEye &&
        CyberpunkVR_BarrelDotSecondVisible) {
        DrawRadialLaserSpot(g_secondEyeWorldDrawList, secondEyeSc, rad);
    }
}

}  // namespace overlay
using namespace overlay;
