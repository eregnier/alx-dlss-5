import ctypes
from ctypes import wintypes
import struct
from PIL import Image
import numpy as np

d3d11 = ctypes.windll.d3d11
d3dcompiler = ctypes.windll.d3dcompiler_47

pDevice = ctypes.c_void_p()
pContext = ctypes.c_void_p()
featureLevel = ctypes.c_uint()

d3d11.D3D11CreateDevice(None, 1, None, 0, None, 0, 7, ctypes.byref(pDevice), ctypes.byref(featureLevel), ctypes.byref(pContext))

vtable_dev = ctypes.cast(pDevice, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
vtable_ctx = ctypes.cast(pContext, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

# Load an image
in_img = Image.open(r"C:\Users\utopm\.gemini\antigravity-cli\brain\2482a0fd-83ce-48d3-b4f0-0435f464dd35\preview_input.png").convert("RGBA")
W, H = in_img.size
print(f"Loaded image {W}x{H}")

# Compile shader
with open(r"C:\code\hlx-dlss5\include\foveated_composite_shader.hpp", "r", encoding="utf-8") as f:
    text = f.read()

s_start = text.find("Texture2D<float4> NativeColor")
s_end = text.find(')";', s_start)
hlsl = text[s_start:s_end].encode("utf-8")

blob = ctypes.c_void_p()
err = ctypes.c_void_p()
d3dcompiler.D3DCompile(hlsl, len(hlsl), None, None, None, b"CSMain", b"cs_5_0", 0, 0, ctypes.byref(blob), ctypes.byref(err))

vtable_blob = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable_blob[3])
GetBufferSize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable_blob[4])

CreateComputeShader = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[18])
pCS = ctypes.c_void_p()
CreateComputeShader(pDevice, GetBufferPointer(blob), GetBufferSize(blob), None, ctypes.byref(pCS))

class D3D11_TEXTURE2D_DESC(ctypes.Structure):
    _fields_ = [
        ('Width', wintypes.UINT), ('Height', wintypes.UINT), ('MipLevels', wintypes.UINT),
        ('ArraySize', wintypes.UINT), ('Format', wintypes.UINT), ('SampleDesc_Count', wintypes.UINT),
        ('SampleDesc_Quality', wintypes.UINT), ('Usage', wintypes.UINT), ('BindFlags', wintypes.UINT),
        ('CPUAccessFlags', wintypes.UINT), ('MiscFlags', wintypes.UINT)
    ]
class D3D11_SUBRESOURCE_DATA(ctypes.Structure):
    _fields_ = [('pSysMem', ctypes.c_void_p), ('SysMemPitch', wintypes.UINT), ('SysMemSlicePitch', wintypes.UINT)]

class D3D11_UNORDERED_ACCESS_VIEW_DESC(ctypes.Structure):
    _fields_ = [('Format', wintypes.UINT), ('ViewDimension', wintypes.UINT), ('MipSlice', wintypes.UINT)]

class D3D11_SHADER_RESOURCE_VIEW_DESC(ctypes.Structure):
    _fields_ = [('Format', wintypes.UINT), ('ViewDimension', wintypes.UINT), ('MostDetailedMip', wintypes.UINT), ('MipLevels', wintypes.UINT)]

class D3D11_BUFFER_DESC(ctypes.Structure):
    _fields_ = [('ByteWidth', wintypes.UINT), ('Usage', wintypes.UINT), ('BindFlags', wintypes.UINT), ('CPUAccessFlags', wintypes.UINT), ('MiscFlags', wintypes.UINT), ('StructureByteStride', wintypes.UINT)]

class D3D11_MAPPED_SUBRESOURCE(ctypes.Structure):
    _fields_ = [('pData', ctypes.c_void_p), ('RowPitch', wintypes.UINT), ('DepthPitch', wintypes.UINT)]

CreateTexture2D = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3D11_TEXTURE2D_DESC), ctypes.POINTER(D3D11_SUBRESOURCE_DATA), ctypes.POINTER(ctypes.c_void_p))(vtable_dev[5])
CreateSRV = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(D3D11_SHADER_RESOURCE_VIEW_DESC), ctypes.POINTER(ctypes.c_void_p))(vtable_dev[7])
CreateUAV = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(D3D11_UNORDERED_ACCESS_VIEW_DESC), ctypes.POINTER(ctypes.c_void_p))(vtable_dev[8])
CreateBuffer = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3D11_BUFFER_DESC), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[3])

raw_bytes = in_img.tobytes()
sub_data = D3D11_SUBRESOURCE_DATA(ctypes.cast(raw_bytes, ctypes.c_void_p), W * 4, 0)

tdNative = D3D11_TEXTURE2D_DESC(W, H, 1, 1, 28, 1, 0, 0, 0x8 | 0x20, 0, 0)
pNative = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdNative), ctypes.byref(sub_data), ctypes.byref(pNative))
srvDesc = D3D11_SHADER_RESOURCE_VIEW_DESC(28, 4, 0, 1)
pNativeSRV = ctypes.c_void_p()
CreateSRV(pDevice, pNative, ctypes.byref(srvDesc), ctypes.byref(pNativeSRV))

# Fovea subrect: 55%
foveaW = int(W * 0.55)
foveaH = int(H * 0.55)
foveaX = (W - foveaW) // 2
foveaY = (H - foveaH) // 2

# Crop fovea from image as neural stand-in
neural_crop = in_img.crop((foveaX, foveaY, foveaX + foveaW, foveaY + foveaH))
neural_bytes = neural_crop.tobytes()
sub_neural = D3D11_SUBRESOURCE_DATA(ctypes.cast(neural_bytes, ctypes.c_void_p), foveaW * 4, 0)

tdNeural = D3D11_TEXTURE2D_DESC(foveaW, foveaH, 1, 1, 28, 1, 0, 0, 0x8 | 0x20 | 0x80, 0, 0)
pNeural = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdNeural), ctypes.byref(sub_neural), ctypes.byref(pNeural))
srvDescNeural = D3D11_SHADER_RESOURCE_VIEW_DESC(28, 4, 0, 1)
pNeuralSRV = ctypes.c_void_p()
CreateSRV(pDevice, pNeural, ctypes.byref(srvDescNeural), ctypes.byref(pNeuralSRV))

# Output texture with UAV
tdComp = D3D11_TEXTURE2D_DESC(W, H, 1, 1, 28, 1, 0, 0, 0x8 | 0x20 | 0x80, 0, 0)
pComp = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdComp), None, ctypes.byref(pComp))
uavDesc = D3D11_UNORDERED_ACCESS_VIEW_DESC(28, 4, 0)
pCompUAV = ctypes.c_void_p()
CreateUAV(pDevice, pComp, ctypes.byref(uavDesc), ctypes.byref(pCompUAV))

# CB
bd = D3D11_BUFFER_DESC(64, 2, 4, 0x10000, 0, 0)
pCB = ctypes.c_void_p()
CreateBuffer(pDevice, ctypes.byref(bd), None, ctypes.byref(pCB))

Map_fn = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, wintypes.UINT, ctypes.POINTER(D3D11_MAPPED_SUBRESOURCE))(vtable_ctx[14])
Unmap_fn = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)(vtable_ctx[15])

mapped = D3D11_MAPPED_SUBRESOURCE()
Map_fn(pContext, pCB, 0, 4, 0, ctypes.byref(mapped))
cb_data = struct.pack(
    "IIIIIIIIffIIIIII",
    W, H,
    foveaX, foveaY,
    foveaW, foveaH,
    foveaW, foveaH,
    0.5, 0.12,
    1, 0, # Foveated=1, ShowBorder=0
    0, 0, 0, 0
)
ctypes.memmove(mapped.pData, cb_data, 64)
Unmap_fn(pContext, pCB, 0)

CSSetShader = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)(vtable_ctx[69])
CSSetCB = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_ctx[71])
CSSetSRV = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_ctx[67])
CSSetUAV = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT))(vtable_ctx[68])
Dispatch = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, wintypes.UINT)(vtable_ctx[41])

CSSetShader(pContext, pCS, None, 0)
cbs = (ctypes.c_void_p * 1)(pCB)
CSSetCB(pContext, 0, 1, cbs)
srvs = (ctypes.c_void_p * 2)(pNativeSRV, pNeuralSRV)
CSSetSRV(pContext, 0, 2, srvs)
uavs = (ctypes.c_void_p * 1)(pCompUAV)
CSSetUAV(pContext, 0, 1, uavs, None)

Dispatch(pContext, (W + 15) // 16, (H + 15) // 16, 1)

# Readback output texture
tdStaging = D3D11_TEXTURE2D_DESC(W, H, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0) # D3D11_USAGE_STAGING = 3, CPU_READ = 0x20000
pStaging = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdStaging), None, ctypes.byref(pStaging))

CopyResource = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(vtable_ctx[47])
CopyResource(pContext, pStaging, pComp)

Map_fn(pContext, pStaging, 0, 1, 0, ctypes.byref(mapped)) # D3D11_MAP_READ = 1
out_buf = ctypes.string_at(mapped.pData, mapped.RowPitch * H)
Unmap_fn(pContext, pStaging, 0)

out_arr = np.frombuffer(out_buf, dtype=np.uint8).reshape((H, mapped.RowPitch))[:, :W * 4].reshape((H, W, 4))
out_img = Image.fromarray(out_arr)
out_img.save(r"C:\code\hlx-dlss5\scratch\test_simulated_output.png")
print("Saved output image to scratch/test_simulated_output.png")
