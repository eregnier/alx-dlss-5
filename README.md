# 🥽 HLX-DLSS5 : NVIDIA DLSS 5 (Neural Reconstruction) for Half-Life: Alyx

Universal non-intrusive OpenVR proxy injecting **NVIDIA DLSS 5 (Neural Reconstruction / DLSS-NR)** into *Half-Life: Alyx* (Source 2 engine).

Combines **dual-instance NGX temporal isolation**, **Cheeky Dynamic Foveated Rendering (DFR)** with native eye tracking, and an **interactive in-game VR settings overlay** (`IVROverlay`).

---

## 🌟 Key Features

* **Stereo-Isolated Dual NGX Engine**: Dedicated Neural Reconstruction instances for Left and Right eye, eliminating temporal interocular cross-talk, shimmering, and eye desynchronization.
* **Cheeky Dynamic Foveated Rendering (DFR)**: Evaluates neural inference exclusively in the high-acuity fovea with smooth cosine feathering. Supports hardware eye tracking (Pimax Crystal, SteamVR Eye Tracking) to follow your gaze in real time.
* **In-Game VR Menu & Real-Time Tuning**: Seamless overlay projected in front of the headset. Adjust sharpness, detail boost, foveation scale, and inference passes on the fly with your VR controllers.
* **Matched Residual Composition**: Merges high-frequency neural reconstruction with native resolution pixels, delivering up to 4x GPU inference speedup at `WorkingScale=0.50` with zero blur.
* **Zero-Intrusion OpenVR Proxy**: Drop-in `openvr_api.dll` proxy requiring zero modifications or patches to the game binary. Instant, reversible vanilla restore.
* **Native C++ Standalone Installer**: Simple, lightweight GUI installer with automated Steam library discovery and whole-disk DLSS 5 model detection.

---

## 🖥️ System Requirements

* **GPU**: NVIDIA GeForce RTX (RTX 40 or 50 series recommended for optimal DLSS-NR performance).
* **OS**: Windows 10 / 11 (64-bit).
* **Game**: *Half-Life: Alyx* on Steam.
* **VR Headset**: SteamVR-compatible headset (Pimax Crystal / Dream Air, Valve Index, Meta Quest via Virtual Desktop / Steam Link).

---

## 🚀 Quick Start & Installation

### Option 1: Automatic Setup via Installer (Recommended)

1. Download the latest release archive (`HLX-DLSS5-v1.0.0.zip`).
2. Extract the folder anywhere and run **`HLX-DLSS5-Installer.exe`**.
3. The installer will automatically locate your *Half-Life: Alyx* installation (`game/bin/win64`).
4. **DLSS 5 Model Acquisition (`nvngx_dlssnr.dll`)**:
   * NVIDIA's neural model (`nvngx_dlssnr.dll`, ~165 MB) is proprietary and not bundled in the download.
   * Click **`Search on my PC`**: The installer will search your Steam game installations (e.g. Cyberpunk 2077) and local drives to locate and verify compatible versions.
   * Or click **`Select File...`** to pick an existing `nvngx_dlssnr.dll` file.
5. Click **`Install / Update Mod`**.
6. Launch *Half-Life: Alyx* via Steam.

### Option 2: Manual Installation

1. Copy `openvr_api.dll` (from `dist/` or release) into `<GameDir>\game\bin\win64\`.
   *(Before copying, rename the game's original `openvr_api.dll` to `openvr_api_real.dll`)*.
2. Copy `nvngx.dll_dlssnr.dll` and `hlx_dlss5.ini` into `<GameDir>\game\bin\win64\`.
3. Copy a compatible NVIDIA `nvngx_dlssnr.dll` into `<GameDir>\game\bin\win64\`.

---

## 🎮 In-Game VR Controls & Menu

| Action | Controller Shortcut | Description |
| :--- | :--- | :--- |
| **Open VR Menu** | **Left D-Pad** (thumb on flat area) + **Right Trigger** | Toggles the in-headset settings overlay (70 cm in front of gaze). |
| **Alternative Menu Trigger** | **Right Stick Up** ($Y > 0.65$) | Instant fallback trigger to open the menu. |
| **Quick DLSS ON/OFF** | **Left D-Pad** + **Right Grip** | Instantly toggles DLSS 5 Neural Reconstruction ON/OFF for A/B comparison without opening the menu. |
| **Navigate Parameters** | **Left Joystick Up / Down** | Selects previous or next parameter. |
| **Adjust Values** | **Left Joystick Left / Right** | Increases or decreases selected setting in real time. |

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
* 18/18 automated CTest unit and integration tests passing.

---

## ⚖️ Legal & Disclaimer

* *Half-Life: Alyx* is a registered trademark of Valve Corporation.
* *NVIDIA DLSS*, *DLSS-NR*, and *NGX* are trademarks of NVIDIA Corporation.
* This project is an independent open-source research and modding effort and is not affiliated with, endorsed by, or sponsored by Valve Corporation or NVIDIA Corporation.
