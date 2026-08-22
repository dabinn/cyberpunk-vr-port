<p align="right"><a href="README.md">English</a></p>

# CyberpunkVR Port - Tofu Express X

**Tofu Express X** 以 CyberpunkVR Port v0.1.2 為基礎，目標是讓玩家儘可能全程以 VR
遊玩《Cyberpunk 2077》，並以範圍小、可驗證且方便回饋上游的方式改善操作、瞄準、角色
身體、安裝與移除體驗。

原作者 [dariulone](https://github.com/dariulone) 的 CyberpunkVR Port 提供原生立體渲染、
OpenXR head tracking 與完整 VR 技術基礎。v0.1.2 已把原本的 Hands plugin 合併進
`CyberpunkVR_Stereo.dll`；Tofu Express 的移植版沿用這個單一 plugin 架構，不會重新加入
舊的 `CyberpunkVR_Hands.dll`。

完整相依套件、檔案配置、控制表、建置與疑難排解仍以 [英文 README](README.md) 為準。

## Tofu Express 改良摘要

### VR 控制器

- 補齊 D-pad、Back／Select 與額外組合鍵操作。
- 可在 F10 選擇 L3、R3 或 Right Thumbrest 作為組合鍵啟動方式；原本的 L3／R3 功能
  仍會保留。
- 可用組合鍵執行 VR recenter 或開啟 F10，不必回到鍵盤。
- 右握把會依 holster／reload／方向盤等 ownership 分流，避免同一個 grip 同時觸發多個功能。
- 搖桿推到底觸發 Sprint／Crouch 可個別開關，預設維持上游行為。

### 視野、瞄準與身體

- **Mouse Y 修正：** 關閉 **Disable Mouse Y** 後，滑鼠或右搖桿可重新控制垂直視野；
  v0.1.2 上游已採用與 Tofu Express 相同的核心作法。
- **Decoupled VR Head Aim：** 武器可跟隨 HMD 瞄準，身體與頭部仍保持解耦；彈道使用
  實際武器 muzzle，而不是從攝影機中心射出。
- **固定 Head Aim gameplay origin：** render camera 保持完整 6-DoF，但玩家實際 lean／
  平移頭部不會反向拖動武器與雙臂；Hip 與 ADS 都只以 live HMD rotation 驅動瞄準。
- **可選的右眼 ADS 對齊：** F10 的 **Align ADS sights to right eye** 預設關閉。開啟後才會
  把 authored ADS pose 從雙眼中心移向右眼，方便直接比較不同武器與瞄具。
- **Physical body rotation：** 角色透過遊戲自己的 heading channel 跟隨頭部，保留左右各
  `45°` free-look；camera compensation 只採用已由 engine body yaw 證實的角度。切換選項
  不會暗中改變 OpenXR base，真正 recenter 後才清除舊 tracking frame offset。

### 駕駛

v0.1.2 使用 iPowerTech 的實體方向盤互動：把控制器移到遊戲駕駛動畫的手部位置並按住
grip，即可用單手或雙手的實際傾斜控制方向；手放在方向盤中心可按喇叭。持槍時右扳機改為
開火，油門會暫時保持，並可用左搖桿微調。

### 安裝、移除與封裝

- Release ZIP 使用以 `Cyberpunk 2077` 為根的 Vortex 友善 payload，並包含 FOMOD metadata。
- `scripts/uninstall.ps1` 只移除本專案擁有的 plugin、CET、redscript、archive、設定與已知
  舊版殘留，不會掃除其他 mod。
- Repository 內的 **Cyberpunk VR Port Auto Installer** 可安裝 GitHub release 或本機
  developer build，也可依相同 ownership catalog 完整移除。

## 基本安裝

請先安裝英文 README 列出的 RED4ext、Cyber Engine Tweaks、ArchiveXL、TweakXL、redscript、
Codeware 等相依套件，再把 release archive 內 `Cyberpunk 2077` 資料夾的內容合併至遊戲根目錄。

請確認下列目錄只保留目前版本的單一 DLL：

```text
red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Stereo.dll
```

舊版 `CyberpunkVR_Hands.dll` 必須移除；RED4ext 會載入 plugin 目錄內每一個 DLL，把重新命名
的備份留在旁邊也會造成重複 hook 與崩潰。

啟動遊戲前先啟動 OpenXR runtime。進入遊戲後按 **F10** 開啟 in-headset 設定面板；按
**F7** 可重新置中。
