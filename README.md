# 🥽 HLX-DLSS5 : NVIDIA DLSS 5 (Neural Reconstruction) for Half-Life: Alyx

Universal non-intrusive OpenVR proxy injecting **NVIDIA DLSS 5 (Neural Reconstruction / DLSS-NR)** into *Half-Life: Alyx* (Source 2 engine).

Combines **dual-instance NGX temporal isolation**, **Cheeky Dynamic Foveated Rendering (DFR)** with native eye tracking, and an **interactive in-game VR settings overlay** (`IVROverlay`).

---

## 📢 Real-World Testing & Honest Performance Expectations

* **Hardware tested on**:
  * **Meta Quest 3** (via Virtual Desktop / Steam Link).
  * **Pimax Dream Air / Crystal / Super** (with eye tracking / DFR).
* **Performance Reality Check**:
  * Let's be completely candid: running full-resolution stereoscopic neural inference at 72-90 FPS per eye is **extremely demanding on hardware**.
  * **It's not magic free performance**, and full-screen inference can be heavy. However, **it works surprisingly well**, especially with **Dynamic Foveated Rendering (DFR)** enabled.
  * When your headset supports eye tracking (e.g. Pimax Crystal), the foveated zone tracks your gaze in real time. Combined with `WorkingScale=0.50`, it cuts GPU neural inference time by ~4x while maintaining crisp native pixel resolution in the sweetspot!

---

## 🛡️ Security & VirusTotal Recommendation

> [!IMPORTANT]
> **Always verify your mods!**  
> Whether you download binaries from this project, NexusMods, Discord, or anywhere else on the web, **you should always upload and check the executable and DLL files on [VirusTotal.com](https://www.virustotal.com/)** before running them.  
> The installer is compiled with standard MSVC from open-source code in this repository, but verifying files independently is simply good security hygiene.

---

## 🎮 Controls & Shortcuts (Controllers & Keyboard)

You can adjust all settings and toggle DLSS 5 either with your VR motion controllers or directly from your keyboard!

### VR Motion Controllers (Quest 3 Touch Plus / Pimax / Index)

| Action | Controller Shortcut | Description |
| :--- | :--- | :--- |
| **Open / Close VR Menu** | **Left D-Pad** (thumb flat on touch area) + **Right Trigger** | Toggles the in-headset settings overlay (70 cm in front of gaze). |
| **Fallback Menu Open** | **Right Stick Up** ($Y > 0.65$) | Alternative single-hand shortcut to open the menu. |
| **Quick DLSS ON/OFF** | **Left D-Pad** + **Right Grip** | Instantly toggles DLSS 5 Neural Reconstruction ON/OFF for immediate A/B visual comparison (silent, no menu). |
| **Navigate Settings** | **Left Stick Up / Down** | Selects previous or next parameter in the list. |
| **Adjust Values** | **Left Stick Left / Right** | Increases or decreases the selected parameter in real time. |

### Keyboard Shortcuts (Desktop / Wireless Keyboard)

| Action | Key / Shortcut | Description |
| :--- | :--- | :--- |
| **Quick DLSS ON/OFF** | `F7` or `NumPad -` or `-` | Instant DLSS 5 toggle ON / Bypass. |
| **Open / Close VR Menu** | `F8` or `F11` or `Ctrl + Alt + D` or `NumPad *` | Toggles the floating in-game VR configuration overlay. |
| **Navigate Settings** | `Arrow Up` / `Arrow Down` | Move cursor between parameters. |
| **Adjust Values** | `Arrow Left` / `Arrow Right` | Decrease or increase selected value. |
| **Toggle Checkboxes** | `Enter` or `Space` | Toggle booleans (e.g., Foveated, EyeTracking, Border). |
| **Close Menu** | `Escape` | Dismisses the overlay. |
| **Toggle Fovea Border** | `F9` | Shows/hides the green foveated circle for optical alignment. |

---

## 🌟 Key Features

* **Stereo-Isolated Dual NGX Engine**: Dedicated Neural Reconstruction instances for Left and Right eye, eliminating temporal interocular cross-talk, shimmering, and eye desynchronization.
* **Cheeky Dynamic Foveated Rendering (DFR)**: Evaluates neural inference exclusively in the high-acuity fovea with smooth cosine feathering. Supports hardware eye tracking (Pimax Crystal, SteamVR Eye Tracking) to follow your gaze in real time.
* **In-Game VR Menu & Real-Time Tuning**: Seamless overlay projected in front of the headset. Adjust sharpness, detail boost, foveation scale, and inference passes on the fly with your VR controllers or keyboard.
* **Matched Residual Composition**: Merges high-frequency neural reconstruction with native resolution pixels, delivering up to 4x GPU inference speedup at `WorkingScale=0.50` with zero blur.
* **Zero-Intrusion OpenVR Proxy**: Drop-in `openvr_api.dll` proxy requiring zero modifications or patches to the game binary. Instant, reversible vanilla restore.
* **Native C++ Standalone Installer**: Simple, lightweight GUI installer with automated Steam library discovery and whole-disk DLSS 5 model detection.

---

## 🖥️ System Requirements

* **GPU**: NVIDIA GeForce RTX (RTX 40 or 50 series recommended for optimal DLSS-NR performance).
* **OS**: Windows 10 / 11 (64-bit).
* **Game**: *Half-Life: Alyx* on Steam.
* **VR Headset**: SteamVR-compatible headset (Meta Quest 3, Pimax Crystal / Dream Air, Valve Index, etc.).

---

## 🚀 Quick Start & Installation

> [!TIP]
> **Play Normally**: Once installed, **you do NOT need to keep this installer open or re-run it before playing!** The mod automatically hooks in via `openvr_api.dll` whenever *Half-Life: Alyx* launches from Steam, SteamVR, or your headset. You only need this installer again if you want to update or uninstall (restore vanilla).

### Option 1: Automatic Setup via Installer (Recommended)

1. Download the release archive (`HLX-DLSS5-v1.0.0.zip`).
2. Extract the folder anywhere and run **`HLX-DLSS5-Installer.exe`**.
3. **Target Directory**: The installer auto-detects your game installation. You can also click **`Auto-Find Alyx`** to scan your Steam library folders, or **`Browse...`** to pick manually.
4. **DLSS 5 Model Acquisition (`nvngx_dlssnr.dll`)**:
   * > [!WARNING]
     > **Do NOT confuse with DLSS 4 / 4.5**: Standard DLSS 4/4.5 Super Resolution (`nvngx_dlss.dll`, ~58 MB) and Ray Reconstruction (`nvngx_dlssd.dll`, ~48 MB) **will NOT work**. This mod requires the dedicated **DLSS 5 Neural Reconstruction model** (`nvngx_dlssnr.dll`, ~165 MB, version **310.8+**).
   * Click **`Search on my PC`**: The installer will search your Steam game libraries (e.g. Cyberpunk 2077) and local drives to locate and strictly verify genuine DLSS 5 models.
   * Or click **`Select File...`** to select an existing `nvngx_dlssnr.dll` file.
5. Click **`Install / Update Mod`**.
6. Close the installer and launch *Half-Life: Alyx* as usual!

### Option 2: Manual Installation

1. Copy `openvr_api.dll` (from `dist/` or release) into `<GameDir>\game\bin\win64\`.
   *(Before copying, rename the game's original `openvr_api.dll` to `openvr_api_real.dll`)*.
2. Copy `nvngx.dll_dlssnr.dll` and `hlx_dlss5.ini` into `<GameDir>\game\bin\win64\`.
3. Copy a compatible NVIDIA `nvngx_dlssnr.dll` into `<GameDir>\game\bin\win64\`.

---

## ⚙️ Configuration (`hlx_dlss5.ini`)

Settings can be edited in `hlx_dlss5.ini` or adjusted directly inside the VR menu:

* `Enabled` (`1` / `0`): Enable or bypass DLSS 5 Neural Reconstruction.
* `Foveated` (`1` / `0`): Enables foveated neural evaluation.
* `FoveatedWidth` / `FoveatedHeight` (`0.25` - `1.00`): Dimensions of the foveated zone.
* `EyeTracking` (`1` / `0`): Real-time pupil gaze tracking for Dynamic Foveated Rendering.
* `WorkingScale` (`0.35` - `1.00`): Internal resolution scale of neural inference (`0.50` yields ~4x faster GPU passes).
* `Sharpness` (`0.0` - `1.0`): Neural edge enhancement filter.
* `DetailBoost` (`0.0` - `2.0`): High-frequency neural texture synthesis strength.
* `Passes` (`1` - `3`): Number of sequential neural inference passes (`1` is optimal for 72-90 FPS).

---

## 🔄 Uninstallation (Vanilla Restore)

* **Via Installer**: Open `HLX-DLSS5-Installer.exe` and click **`Uninstall (Vanilla Restore)`**.
* **Manually**: In `<GameDir>\game\bin\win64\`, delete `openvr_api.dll` and rename `openvr_api_real.dll` back to `openvr_api.dll`. Delete `nvngx.dll_dlssnr.dll` and `hlx_dlss5.ini`.

---

## 🧪 Scientific Validation & Testing

HLX-DLSS5 includes an extensive test suite verifying:
* Zero-touch VTable interception without compositor rejection.
* Inter-API D3D11 <-> D3D12 hardware synchronization and shared textures.
* Single-pass stereo atlas reuse dividing compositor latency in half.
* 19/19 automated CTest unit and integration tests passing.

---

## ⚖️ Legal & Disclaimer

* *Half-Life: Alyx* is a registered trademark of Valve Corporation.
* *NVIDIA DLSS*, *DLSS-NR*, and *NGX* are trademarks of NVIDIA Corporation.
* This project is an independent open-source research and modding effort and is not affiliated with, endorsed by, or sponsored by Valve Corporation or NVIDIA Corporation.
