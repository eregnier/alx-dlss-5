#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace hlx {

// Attributes the GPU cost of our pipeline on the game's D3D11 timeline:
//   [0] CopyIn  : native + fovea CopySubresourceRegion
//   [1] Neural  : NGX evaluate + the D3D11<->D3D12 fence wait in front of it
//   [2] Compose : foveated composite dispatch + copy back into the game texture
// Results are read back two frames later with DONOTFLUSH: no CPU stall, and the
// CPU-side timings in [CADENCE-1SEC] stay untouched.
class GpuTimers {
public:
    static constexpr int RINGS = 3;

    static GpuTimers& Instance() {
        static GpuTimers instance;
        return instance;
    }

    bool Init(ID3D11Device* device) {
        if (initialized_ || !device) return initialized_;
        for (int eye = 0; eye < 2; ++eye) {
            for (int r = 0; r < RINGS; ++r) {
                D3D11_QUERY_DESC d = {};
                d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                if (FAILED(device->CreateQuery(&d, &disjoint_[eye][r]))) return false;
            }
            for (int r = 0; r < RINGS; ++r) {
                for (int i = 0; i < 4; ++i) {
                    D3D11_QUERY_DESC d = {};
                    d.Query = D3D11_QUERY_TIMESTAMP;
                    if (FAILED(device->CreateQuery(&d, &stamp_[eye][r][i]))) return false;
                }
            }
            ring_[eye] = 0;
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] bool IsReady() const noexcept { return initialized_; }

    void Begin(ID3D11DeviceContext* ctx, int eye) {
        if (!initialized_ || !ctx) return;
        const int r = ring_[eye];
        ctx->Begin(disjoint_[eye][r]);
        ctx->End(stamp_[eye][r][0]);
    }

    void Mark(ID3D11DeviceContext* ctx, int eye, int index) {
        if (!initialized_ || !ctx) return;
        ctx->End(stamp_[eye][ring_[eye]][index]);
    }

    void End(ID3D11DeviceContext* ctx, int eye) {
        if (!initialized_ || !ctx) return;
        const int r = ring_[eye];
        ctx->End(stamp_[eye][r][3]);
        ctx->End(disjoint_[eye][r]);
        ring_[eye] = (ring_[eye] + 1) % RINGS;
    }

    // Reads the ring slot from two frames ago; false while the GPU has not finished it.
    bool TryCollect(ID3D11DeviceContext* ctx, int eye) {
        if (!initialized_ || !ctx) return false;
        const int slot = (ring_[eye] + 1) % RINGS;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (ctx->GetData(disjoint_[eye][slot], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
        if (dj.Disjoint || dj.Frequency == 0) return false;

        UINT64 ts[4] = {};
        for (int i = 0; i < 4; ++i) {
            if (ctx->GetData(stamp_[eye][slot][i], &ts[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
        }

        sums_[0] += static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(dj.Frequency);
        sums_[1] += static_cast<double>(ts[2] - ts[1]) * 1000.0 / static_cast<double>(dj.Frequency);
        sums_[2] += static_cast<double>(ts[3] - ts[2]) * 1000.0 / static_cast<double>(dj.Frequency);
        samples_++;
        return true;
    }

    // Averages in ms per eye since the previous call, then resets.
    void Consume(double out[3]) {
        for (int i = 0; i < 3; ++i) {
            out[i] = samples_ ? sums_[i] / static_cast<double>(samples_) : 0.0;
            sums_[i] = 0.0;
        }
        samples_ = 0;
    }

private:
    bool initialized_ = false;
    ID3D11Query* disjoint_[2][RINGS] = {};
    ID3D11Query* stamp_[2][RINGS][4] = {};
    int ring_[2] = {};
    double sums_[3] = {};
    uint32_t samples_ = 0;
};

} // namespace hlx
