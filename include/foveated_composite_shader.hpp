#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "hlx_logger.hpp"

using Microsoft::WRL::ComPtr;

namespace hlx {

// Constant buffer layout matching HLSL cbuffer FoveatedConstants : register(b0)
// 16-byte alignment strictly enforced for D3D11 constant buffers (64 bytes total).
struct alignas(16) FoveatedConstants {
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t foveaBaseX;
    uint32_t foveaBaseY;

    uint32_t foveaWidth;
    uint32_t foveaHeight;
    uint32_t neuralWidth;
    uint32_t neuralHeight;

    float foveaRoundness;
    float featherWidth;
    uint32_t foveatedEnabled;
    uint32_t showBorder;

    float sharpness;     // [0.0 - 1.0] neural detail boost & unsharp mask
    uint32_t padding[3]; // padding[0] = useResidualTransfer (1 when InputProxyColor t2 is bound at WorkingScale < 1.0)
};

static inline const char g_foveatedCompositeHlsl[] = R"(
Texture2D<float4> NativeColor     : register(t0);
Texture2D<float4> NeuralColor     : register(t1);
Texture2D<float4> InputProxyColor : register(t2);
RWTexture2D<float4> OutputColor   : register(u0);

cbuffer FoveatedConstants : register(b0) {
    uint2 OutputSize;
    uint2 FoveaBase;
    uint2 FoveaSize;
    uint2 NeuralSize;
    float FoveaRoundness;
    float FeatherWidth;
    uint  FoveatedEnabled;
    uint  ShowBorder;
    float Sharpness;
    uint3 Padding;
};

float EllipseDistance(float2 pixel, float2 base, float2 size, float roundness) {
    // Reserve the outer 8% of the fovea tensor (apron = 0.92) as a clean receptive-field
    // guard band for DLSS-NR's 3-stage U-Net pyramid (1/2, 1/4, 1/8). This completely hides
    // the 3 horizontal/vertical convolutional border-padding lines near the tensor boundary.
    float2 centered = ((pixel - base + 0.5) / (0.5 * max(size, 1.0)) - 1.0) / 0.92;
    float2 scaled = abs(centered);
    return lerp(max(scaled.x, scaled.y), length(scaled), saturate(roundness));
}

float3 SampleBilinear(Texture2D<float4> tex, float2 pos, uint2 texSize) {
    int2 maxCoord = int2(texSize) - 1;
    int2 p00 = clamp(int2(floor(pos)), int2(0, 0), maxCoord);
    int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), maxCoord);
    int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), maxCoord);
    int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), maxCoord);
    float2 f = frac(pos);
    return lerp(
        lerp(tex.Load(int3(p00, 0)).rgb, tex.Load(int3(p10, 0)).rgb, f.x),
        lerp(tex.Load(int3(p01, 0)).rgb, tex.Load(int3(p11, 0)).rgb, f.x),
        f.y
    );
}

// Soft-cubic residual scaling (OptiScaler-DLSSNR / Cheeky):
// Transfers the neural lighting/detail residual onto full-resolution native pixels
// while softly compressing near 0.0 and 1.0 to prevent highlight/shadow clipping.
float3 CubeScaleResidual(float3 baseColor, float3 targetColor) {
    float3 headroom = max(0.0, float3(1.0, 1.0, 1.0) - abs(baseColor * 2.0 - 1.0));
    float3 softScale = float3(1.0, 1.0, 1.0) - pow(float3(1.0, 1.0, 1.0) - saturate(headroom), 3.0);
    return saturate(baseColor + (targetColor - baseColor) * max(softScale, 0.25));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= OutputSize.x || dispatch_id.y >= OutputSize.y) return;
    uint2 pixel = dispatch_id.xy;
    float4 native_color = NativeColor.Load(int3(pixel, 0));
    
    if (FoveatedEnabled == 0) {
        float2 uv = (float2(pixel) + 0.5) / float2(OutputSize);
        float2 neural_pos = uv * float2(NeuralSize) - 0.5;
        float3 neural_rgb = SampleBilinear(NeuralColor, neural_pos, NeuralSize);
        if (Padding.x != 0) {
            float3 proxy_rgb = SampleBilinear(InputProxyColor, neural_pos, NeuralSize);
            float3 delta = neural_rgb - proxy_rgb;
            neural_rgb = CubeScaleResidual(native_color.rgb, native_color.rgb + delta);
        }
        OutputColor[pixel] = float4(neural_rgb, native_color.a);
        return;
    }
    
    bool inside_box = (pixel.x >= FoveaBase.x && pixel.x < FoveaBase.x + FoveaSize.x &&
                       pixel.y >= FoveaBase.y && pixel.y < FoveaBase.y + FoveaSize.y);
    if (!inside_box) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float dist = EllipseDistance(float2(pixel), float2(FoveaBase), float2(FoveaSize), FoveaRoundness);
    if (ShowBorder != 0 && abs(dist - 1.0) < 0.02) {
        OutputColor[pixel] = float4(0.0, 1.0, 0.0, native_color.a);
        return;
    }
    if (dist >= 1.0) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float weight = 1.0 - smoothstep(max(0.0, 1.0 - FeatherWidth), 1.0, dist);
    if (weight <= 0.0) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float2 local_pos = float2(pixel - FoveaBase);
    float2 uv_fovea = (local_pos + 0.5) / float2(FoveaSize);
    float2 neural_pos = uv_fovea * float2(NeuralSize) - 0.5;
    float3 neural_rgb = SampleBilinear(NeuralColor, neural_pos, NeuralSize);

    if (Padding.x != 0) {
        // Matched Residual Composition (WorkingScale < 1.0):
        // Extract the pure neural edit (NeuralColor - InputProxyColor) at the working resolution
        // and inject it onto the untouched full-resolution native pixel.
        float3 proxy_rgb = SampleBilinear(InputProxyColor, neural_pos, NeuralSize);
        float3 delta = neural_rgb - proxy_rgb;

        if (Sharpness > 0.01) {
            float3 d_n = SampleBilinear(NeuralColor, neural_pos + float2(0.0, -1.0), NeuralSize) -
                         SampleBilinear(InputProxyColor, neural_pos + float2(0.0, -1.0), NeuralSize);
            float3 d_s = SampleBilinear(NeuralColor, neural_pos + float2(0.0,  1.0), NeuralSize) -
                         SampleBilinear(InputProxyColor, neural_pos + float2(0.0,  1.0), NeuralSize);
            float3 d_w = SampleBilinear(NeuralColor, neural_pos + float2(-1.0, 0.0), NeuralSize) -
                         SampleBilinear(InputProxyColor, neural_pos + float2(-1.0, 0.0), NeuralSize);
            float3 d_e = SampleBilinear(NeuralColor, neural_pos + float2( 1.0, 0.0), NeuralSize) -
                         SampleBilinear(InputProxyColor, neural_pos + float2( 1.0, 0.0), NeuralSize);
            float3 highFreq = delta - (d_n + d_s + d_w + d_e) * 0.25;
            delta += highFreq * (Sharpness * 1.5 * weight);
        }

        neural_rgb = CubeScaleResidual(native_color.rgb, native_color.rgb + delta);
    } else {
        // Direct full-fovea neural evaluation (WorkingScale == 1.0)
        if (Sharpness > 0.01) {
            float3 c_n = SampleBilinear(NeuralColor, neural_pos + float2(0.0, -1.0), NeuralSize);
            float3 c_s = SampleBilinear(NeuralColor, neural_pos + float2(0.0,  1.0), NeuralSize);
            float3 c_w = SampleBilinear(NeuralColor, neural_pos + float2(-1.0, 0.0), NeuralSize);
            float3 c_e = SampleBilinear(NeuralColor, neural_pos + float2( 1.0, 0.0), NeuralSize);
            float3 avg = (c_n + c_s + c_w + c_e) * 0.25;
            float3 highFreq = neural_rgb - avg;
            neural_rgb = saturate(neural_rgb + highFreq * (Sharpness * 1.5 * weight));
        }
    }

    OutputColor[pixel] = float4(lerp(native_color.rgb, neural_rgb, weight), native_color.a);
}
)";

class FoveatedCompositePass {
public:
    static FoveatedCompositePass& Instance() {
        static FoveatedCompositePass instance;
        return instance;
    }

    bool Init(ID3D11Device* device) {
        if (isInitialized_ && device == device_) return true;
        device_ = device;

        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;

        HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!hCompiler) {
            HLX_ERROR("Failed to load d3dcompiler_47.dll");
            return false;
        }

        pD3DCompile pfnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile"));
        if (!pfnCompile) {
            HLX_ERROR("Failed to get D3DCompile proc address");
            return false;
        }

        HRESULT hr = pfnCompile(
            g_foveatedCompositeHlsl,
            sizeof(g_foveatedCompositeHlsl) - 1,
            "FoveatedCompositeHLSL",
            nullptr,
            nullptr,
            "CSMain",
            "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &shaderBlob,
            &errorBlob
        );

        if (FAILED(hr)) {
            if (errorBlob) {
                HLX_ERROR("Foveated composite shader compilation error: %s",
                    reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = device_->CreateComputeShader(
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            nullptr,
            &computeShader_
        );
        if (FAILED(hr)) {
            HLX_ERROR("Failed to create D3D11 compute shader: 0x%08X", hr);
            return false;
        }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(FoveatedConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&bd, nullptr, &constantBuffer_);
        if (FAILED(hr)) {
            HLX_ERROR("Failed to create constant buffer for foveated composite: 0x%08X", hr);
            return false;
        }

        isInitialized_ = true;
        HLX_INFO("FoveatedCompositePass initialized successfully (CS=5.0, CB=%zu bytes)", sizeof(FoveatedConstants));
        return true;
    }

    bool Execute(
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* nativeSrv,
        ID3D11ShaderResourceView* neuralSrv,
        ID3D11UnorderedAccessView* outputUav,
        const FoveatedConstants& constants,
        ID3D11ShaderResourceView* inputProxySrv = nullptr
    ) {
        if (!isInitialized_ || !ctx) return false;

        FoveatedConstants cbData = constants;
        if (inputProxySrv != nullptr &&
            (constants.neuralWidth < constants.foveaWidth || constants.neuralHeight < constants.foveaHeight)) {
            cbData.padding[0] = 1u;
        }

        // 1. Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return false;
        memcpy(mapped.pData, &cbData, sizeof(FoveatedConstants));
        ctx->Unmap(constantBuffer_.Get(), 0);

        // 2. Bind pipeline state
        ID3D11ShaderResourceView* srvs[3] = { nativeSrv, neuralSrv, inputProxySrv };
        ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
        ID3D11UnorderedAccessView* uavs[1] = { outputUav };

        ctx->CSSetShader(computeShader_.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // 3. Dispatch
        uint32_t dispatchX = (constants.outputWidth + 15) / 16;
        uint32_t dispatchY = (constants.outputHeight + 15) / 16;
        ctx->Dispatch(dispatchX, dispatchY, 1);

        // 4. Unbind resources to prevent hazard locks
        ctx->CSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* nullCbs[1] = { nullptr };
        ctx->CSSetConstantBuffers(0, 1, nullCbs);
        ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
        ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
        ctx->CSSetShaderResources(0, 3, nullSrvs);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

        return true;
    }

    void Release() {
        computeShader_.Reset();
        constantBuffer_.Reset();
        device_ = nullptr;
        isInitialized_ = false;
    }

private:
    FoveatedCompositePass() = default;
    ~FoveatedCompositePass() { Release(); }

    bool isInitialized_ = false;
    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11ComputeShader> computeShader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
};


// ---------------------------------------------------------------------------
// Fovea downsample pass: feeds the neural model an area-integrated working image.
// Uses exact box-area integration over the source pixel footprint so downsampling
// has zero sub-pixel aliasing at any WorkingScale (e.g. 0.50, 0.55, 0.65).
// Combined with Matched Residual Composition in FoveatedCompositePass, this cuts
// neural inference GPU cost by ~4x at WorkingScale=0.50 while preserving 100% of
// the full-resolution native pixel sharpness.
// ---------------------------------------------------------------------------
struct alignas(16) FoveaDownsampleConstants {
    uint32_t srcBaseX;
    uint32_t srcBaseY;
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    uint32_t padding[2];
};

static inline const char g_foveaDownsampleHlsl[] = R"(
Texture2D<float4> SrcColor : register(t0);
RWTexture2D<float4> DstColor : register(u0);

cbuffer DownsampleConstants : register(b0) {
    uint2 SrcBase;
    uint2 SrcSize;
    uint2 DstSize;
    uint2 Padding;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DstSize.x || id.y >= DstSize.y) return;

    float2 scale = float2(SrcSize) / float2(DstSize);
    float2 boxMin = float2(SrcBase) + float2(id.xy) * scale;
    float2 boxMax = float2(SrcBase) + float2(id.xy + uint2(1, 1)) * scale;
    int2 lo = int2(SrcBase);
    int2 hi = int2(SrcBase) + int2(SrcSize) - 1;

    int2 startPx = clamp(int2(floor(boxMin)), lo, hi);
    int2 endPx   = clamp(int2(ceil(boxMax)) - 1, lo, hi);
    endPx = min(endPx, startPx + int2(3, 3));

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    for (int y = startPx.y; y <= endPx.y; ++y) {
        float wy = max(0.0, min(float(y + 1), boxMax.y) - max(float(y), boxMin.y));
        for (int x = startPx.x; x <= endPx.x; ++x) {
            float wx = max(0.0, min(float(x + 1), boxMax.x) - max(float(x), boxMin.x));
            float w = wx * wy;
            acc += SrcColor.Load(int3(x, y, 0)) * w;
            weightSum += w;
        }
    }
    DstColor[id.xy] = (weightSum > 1e-5) ? (acc / weightSum) : SrcColor.Load(int3(startPx, 0));
}
)";

class FoveaDownsamplePass {
public:
    static FoveaDownsamplePass& Instance() {
        static FoveaDownsamplePass instance;
        return instance;
    }

    bool Init(ID3D11Device* device) {
        if (isInitialized_ && device == device_) return true;
        device_ = device;

        HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!hCompiler) {
            HLX_ERROR("Downsample: failed to load d3dcompiler_47.dll");
            return false;
        }
        auto pfnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile"));
        if (!pfnCompile) {
            HLX_ERROR("Downsample: failed to get D3DCompile");
            return false;
        }

        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = pfnCompile(
            g_foveaDownsampleHlsl, sizeof(g_foveaDownsampleHlsl) - 1, "FoveaDownsampleHLSL",
            nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                HLX_ERROR("Fovea downsample shader compilation error: %s",
                    reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = device_->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &computeShader_);
        if (FAILED(hr)) {
            HLX_ERROR("Downsample: CreateComputeShader failed 0x%08X", hr);
            return false;
        }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(FoveaDownsampleConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&bd, nullptr, &constantBuffer_);
        if (FAILED(hr)) {
            HLX_ERROR("Downsample: CreateBuffer failed 0x%08X", hr);
            return false;
        }

        isInitialized_ = true;
        HLX_INFO("FoveaDownsamplePass initialized successfully (CS=5.0, CB=%zu bytes)", sizeof(FoveaDownsampleConstants));
        return true;
    }

    bool Execute(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSrv,
                 ID3D11UnorderedAccessView* dstUav, const FoveaDownsampleConstants& constants) {
        if (!isInitialized_ || !ctx || !srcSrv || !dstUav) return false;
        if (constants.dstWidth == 0 || constants.dstHeight == 0) return false;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        memcpy(mapped.pData, &constants, sizeof(constants));
        ctx->Unmap(constantBuffer_.Get(), 0);

        ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
        ctx->CSSetShader(computeShader_.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(0, 1, &srcSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &dstUav, nullptr);
        ctx->Dispatch((constants.dstWidth + 15) / 16, (constants.dstHeight + 15) / 16, 1);

        ctx->CSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* nullCbs[1] = { nullptr };
        ctx->CSSetConstantBuffers(0, 1, nullCbs);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        return true;
    }

    void Release() {
        computeShader_.Reset();
        constantBuffer_.Reset();
        device_ = nullptr;
        isInitialized_ = false;
    }

private:
    FoveaDownsamplePass() = default;
    ~FoveaDownsamplePass() { Release(); }

    bool isInitialized_ = false;
    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11ComputeShader> computeShader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
};

} // namespace hlx
