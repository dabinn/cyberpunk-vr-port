<p align="right"><a href="README.md">English</a></p>

# CyberpunkVR Port - Tofu Express X

**Tofu Express X** 是 CyberpunkVR Port 的另一個分支版本，在保留原版功能的同時，也提供更多額外的選項與設定，讓玩家能更自由地選擇自己喜歡的方式，在 VR 中體驗《Cyberpunk 2077》。

這個專案最初著重於協助修復早期版本的問題，讓 VR Port 儘快達到可完整遊玩的狀態。隨著 VR Port 逐漸成熟，Tofu Express X 也開始在原本目標的基礎上，嘗試更廣泛的改善方向。

由 [dariulone](https://github.com/dariulone) 開發的原始 CyberpunkVR Port 擁有非常優秀的技術基礎。它的核心設計提供了充足的發展空間，能夠持續改進、實驗並延伸出新的想法。

**版本命名：** Tofu Express X 採用「**上游版本號 + X 修訂號**」的格式。每次 Tofu Express 更新時，X 數字都會遞增，因此只要比較 X 的數字，就能直接判斷哪個版本較新。

## Cyberpunk VR Port自動安裝工具

**強烈推薦。** 為了讓安裝與移除比原版 Mod 更方便，Tofu Express 提供獨立的 Cyberpunk VR Port Auto Installer，可一鍵下載、安裝或解除安裝 Mod。除了 Tofu Express 之外，也支援原版 Cyberpunk VR Port，以及我所知道仍在持續開發的其他分支，方便快速切換並嘗試不同版本的 VR Mod。

Auto Installer 也包含 Developer Mode，方便開發者測試本機檔案。

詳細資訊與下載請見 [Cyberpunk VR Port Auto Installer](https://github.com/dabinn/cyberpunk-vr-port/releases/tag/Cyberpunk-VR-Port-Auto-Installer)。

### Vortex Mod Manager 支援

如果你習慣使用 Vortex 管理所有 Mod，Tofu Express 的壓縮檔結構也已調整，讓 Vortex 能正確辨識需要安裝的檔案。只要將 ZIP 檔拖入 Vortex 即可安裝；解除安裝時，Vortex 也能正確移除所有已安裝的檔案。

> **注意：** 從 Tofu Express 3（TE3）開始支援 Vortex 安裝。

## 支援各種控制方式

這個 Mod 並不侷限於 VR 控制器。如果你偏好使用手把，甚至鍵盤滑鼠，也可以自由選擇最適合自己的控制方式。

## VR 控制器改進

* **完整對應所有 Xbox 手把按鍵。** 已補上原版 Mod 缺少的輸入，現在可以透過 VR 控制器使用所有 Xbox 手把按鍵。

* **彈性的組合鍵啟動方式。** 可依照自己的偏好選擇 L3、R3，或 Right Thumbrest 觸控感應區作為啟動鍵，同時保留 L3 與 R3 原本的遊戲功能。

* **可選的 VR 重新置中與 F10 選單組合鍵捷徑。** 執行這些操作時不再需要鍵盤。

* **依情境切換 Right Grip 功能。** 修正原版 Mod 缺少 RB 輸入的問題：在肩部或腰部槍套區域中，可使用沉浸式拔槍／收槍動作；其他位置則送出 RB。駕駛時會停用槍套手勢，避免誤觸。

* **駕駛時自動交換 Trigger / Grip。** Right Trigger 在步行與車內都能維持射擊功能，而 VR 控制器的類比 Grip 則負責加速與煞車。實體 Xbox 手把的輸入不受影響。

[在 YouTube 上觀看車輛戰鬥示範](https://www.youtube.com/watch?v=n6bx6JbvSgs)

* **可切換的搖桿到底 Sprint / Crouch。** 原版 Mod 會在左搖桿推到底時自動衝刺、右搖桿向下推到底時自動蹲下。現在可以從 F10 選單中自行開啟或關閉這些行為。

## 視角控制與更可靠的武器瞄準

* **修正垂直視角控制。** 關閉 **Disable Mouse Y** 後，現在可以正確恢復滑鼠或右搖桿的垂直視角控制，讓進階玩家擁有更多控制自由。

* **Decoupled VR Head Aim** 是為偏好手把的玩家設計。不同於傳統的頭瞄——有時會被戲稱為「gun-face」——頭部與身體旋轉彼此獨立。你可以自由用頭部瞄準武器，而不會改變身體面向的方向。

* **子彈現在會從即時槍口位置射出。** Head Aim 不再使用原版那種「用眼睛開槍」的計算方式。Head Aim 與 Hand Aim 現在都會使用武器當下的槍口位置與方向，因此雷射點、武器瞄具與實際彈著點都使用相同的射擊基準。

* **腰射與 ADS 瞄準維持一致。** 抬起武器進入 ADS 時，不會再讓瞄準位置偏離原本的方向。即使停用 VRIK，Mod 也會盡可能保留進入 ADS 前的瞄準方向。

* **快速轉頭時，外部槍口準星更加穩定。** 大幅減少長拖影以及多個分離準星點的情況。

* **武器瞄具會維持與外部準星對齊。** 反射式瞄具準星、狙擊鏡十字線、外部槍口準星，以及放大後的 ADS 畫面，在轉頭時不再彼此漂移。

* **ADS 使用右眼作為瞄準眼。** 使用 Head Aim 而非 Hand Aim 時，進入 ADS 會自動將武器瞄具移到右眼前方，而不是停在兩眼之間。高倍率瞄準時尤其明顯。

* **實體身體旋轉系統已重新改寫。** 角色現在會跟隨頭部旋轉，而不會出現原版實作中令人不適的視角跳動。同時保留左右各 `45°` 的自由轉頭範圍，因此轉頭並不會直接鎖定身體前進方向。


## 控制器設定

按下 **F10**，開啟 **Controls**，再依照自己的偏好調整各項設定。

![F10 選單中的新控制器設定](images/v0.1.1-controller-enhancements.png)

### 模擬十字鍵與其他控制功能

選擇一種 **Chord Activation Method**。預設使用原版的 L3 啟動方式：

| 啟動方式                   | 十字鍵 | Back / Select | VR Recenter | F10 選單 |
| ---------------------- | --- | ------------- | ----------- | ------ |
| 按住 **L3**（左搖桿按下）       | 右搖桿 | Left Menu     | A           | B      |
| 按住 **R3**（右搖桿按下）       | 左搖桿 | Left Menu     | X           | Y      |
| 觸碰 **Right Thumbrest** | 左搖桿 | Left Menu     | X           | Y      |

先按住啟動鍵，再移動搖桿或按下其他按鍵。L3 與 R3 仍會保留原本的正常功能。

Left Menu 會送出 Start；組合鍵 + Left Menu 則送出 Back / Select。十字鍵與 Back / Select 永遠啟用。預設開啟的 **Extra Chord Actions** 會額外加入 VR Recenter 與 F10 選單捷徑。

**Right Thumbrest** 是面板按鍵旁、拇指自然放置位置的觸控感應區，不需要實際按下即可啟動組合鍵。只有部分 VR 控制器支援這項功能，包括 Quest 3；如果你的控制器不支援，請使用 L3。

原本的鍵盤快捷鍵仍然可以使用：**F7** 為 VR Recenter，**F10 / Insert** 則開啟選單。

### Right Grip 與虛擬槍套

* 在肩部或腰部槍套區域內按下 Right Grip：拔出或收起武器。
* 在其他位置按下 Right Grip：送出 **RB**。
* 在車內：停用槍套手勢。

### 駕駛時交換 Triggers / Grips

開啟此選項後，駕駛時仍可使用 Right Trigger 射擊：

| VR 輸入             | 模擬的手把輸入 |
| ----------------- | ------- |
| Left Trigger      | LB      |
| Right Trigger     | RB      |
| Left analog Grip  | LT      |
| Right analog Grip | RT      |

類比 Grip 會分別作為煞車與油門。只有 VR 控制器輸入會被交換，實體手把完全不受影響。此選項 **預設關閉**。

### 其他選項

| 設定                             | 行為                                      |
| ------------------------------ | --------------------------------------- |
| **Disable Mouse Y (Pitch)**    | 開啟：垂直視角只跟隨 HMD。關閉：啟用滑鼠與右搖桿的垂直視角控制。      |
| **Full-stick Sprint / Crouch** | 左搖桿完全向前推時 Sprint，右搖桿完全向下推時 Crouch。預設開啟。 |


## 原作者文件

請看英文版readme，我懶得翻譯了。
