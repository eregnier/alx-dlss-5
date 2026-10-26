# HLX-DLSS5 : NVIDIA DLSS 5 (Neural Reconstruction) for Half-Life: Alyx

Universal non-intrusive OpenVR stereoscopic proxy injecting **NVIDIA DLSS 5 Neural Reconstruction** into *Half-Life: Alyx* (Source 2).

## Key Features
- **Zero-touch drop-in DLL** (`openvr_api.dll`): Simply place in `Half-Life Alyx/game/bin/win64/`.
- **Stereo-Isolated Dual NGX Engine**: Dedicated Neural Reconstruction instances for Left and Right eye, ensuring zero temporal cross-talk or stereo shimmering.
- **DirectX 12 Private Device**: Interoperability bridge importing game textures directly into the NVIDIA NGX DLSS-NR pipeline.
- **Fully Automated Test Suite**: Built with verifiable, decoupled unit tests for robust stability.

For comprehensive architectural documentation, design decisions, and tracking, see [PROJECT_TRACKING.md](PROJECT_TRACKING.md).
