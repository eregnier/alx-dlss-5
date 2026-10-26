import ctypes
from ctypes import wintypes
import struct

d3d11 = ctypes.windll.d3d11
d3dcompiler = ctypes.windll.d3dcompiler_47

pDevice = ctypes.c_void_p()
pContext = ctypes.c_void_p()
featureLevel = ctypes.c_uint()

hr = d3d11.D3D11CreateDevice(
    None, 1, None, 0, None, 0, 7,
    ctypes.byref(pDevice),
    ctypes.byref(featureLevel),
    ctypes.byref(pContext)
)

vtable_dev = ctypes.cast(pDevice, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
vtable_ctx = ctypes.cast(pContext, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

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

ptr = GetBufferPointer(blob)
sz = GetBufferSize(blob)

CreateComputeShader = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[18])
pCS = ctypes.c_void_p()
hr = CreateComputeShader(pDevice, ptr, sz, None, ctypes.byref(pCS))
print("CreateComputeShader:", hex(hr & 0xFFFFFFFF), "pCS:", pCS.value)

class D3D11_TEXTURE2D_DESC(ctypes.Structure):
    _fields_ = [
        ('Width', wintypes.UINT),
        ('Height', wintypes.UINT),
        ('MipLevels', wintypes.UINT),
        ('ArraySize', wintypes.UINT),
        ('Format', wintypes.UINT),
        ('SampleDesc_Count', wintypes.UINT),
        ('SampleDesc_Quality', wintypes.UINT),
        ('Usage', wintypes.UINT),
        ('BindFlags', wintypes.UINT),
        ('CPUAccessFlags', wintypes.UINT),
        ('MiscFlags', wintypes.UINT),
    ]

class D3D11_UNORDERED_ACCESS_VIEW_DESC(ctypes.Structure):
    _fields_ = [
        ('Format', wintypes.UINT),
        ('ViewDimension', wintypes.UINT),
        ('MipSlice', wintypes.UINT),
    ]

class D3D11_SHADER_RESOURCE_VIEW_DESC(ctypes.Structure):
    _fields_ = [
        ('Format', wintypes.UINT),
        ('ViewDimension', wintypes.UINT),
        ('MostDetailedMip', wintypes.UINT),
        ('MipLevels', wintypes.UINT),
    ]

class D3D11_BUFFER_DESC(ctypes.Structure):
    _fields_ = [
        ('ByteWidth', wintypes.UINT),
        ('Usage', wintypes.UINT),
        ('BindFlags', wintypes.UINT),
        ('CPUAccessFlags', wintypes.UINT),
        ('MiscFlags', wintypes.UINT),
        ('StructureByteStride', wintypes.UINT),
    ]

class D3D11_MAPPED_SUBRESOURCE(ctypes.Structure):
    _fields_ = [
        ('pData', ctypes.c_void_p),
        ('RowPitch', wintypes.UINT),
        ('DepthPitch', wintypes.UINT),
    ]

CreateTexture2D = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3D11_TEXTURE2D_DESC), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[5])
CreateSRV = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(D3D11_SHADER_RESOURCE_VIEW_DESC), ctypes.POINTER(ctypes.c_void_p))(vtable_dev[7])
CreateUAV = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(D3D11_UNORDERED_ACCESS_VIEW_DESC), ctypes.POINTER(ctypes.c_void_p))(vtable_dev[8])
CreateBuffer = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3D11_BUFFER_DESC), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[3])

unormFormat = 28
srvDesc = D3D11_SHADER_RESOURCE_VIEW_DESC(unormFormat, 4, 0, 1)
uavDesc = D3D11_UNORDERED_ACCESS_VIEW_DESC(unormFormat, 4, 0)

# Native (4040x2696)
tdNative = D3D11_TEXTURE2D_DESC(4040, 2696, 1, 1, unormFormat, 1, 0, 0, 0x8 | 0x20, 0, 0)
pNative = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdNative), None, ctypes.byref(pNative))
pNativeSRV = ctypes.c_void_p()
CreateSRV(pDevice, pNative, ctypes.byref(srvDesc), ctypes.byref(pNativeSRV))

# Neural (2224x1488)
tdNeural = D3D11_TEXTURE2D_DESC(2224, 1488, 1, 1, unormFormat, 1, 0, 0, 0x8 | 0x20 | 0x80, 0, 0x2)
pNeural = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdNeural), None, ctypes.byref(pNeural))
pNeuralSRV = ctypes.c_void_p()
CreateSRV(pDevice, pNeural, ctypes.byref(srvDesc), ctypes.byref(pNeuralSRV))

# Output (4040x2696)
tdComp = D3D11_TEXTURE2D_DESC(4040, 2696, 1, 1, unormFormat, 1, 0, 0, 0x8 | 0x20 | 0x80, 0, 0)
pComp = ctypes.c_void_p()
CreateTexture2D(pDevice, ctypes.byref(tdComp), None, ctypes.byref(pComp))
pCompUAV = ctypes.c_void_p()
CreateUAV(pDevice, pComp, ctypes.byref(uavDesc), ctypes.byref(pCompUAV))

# CB
bd = D3D11_BUFFER_DESC(64, 2, 4, 0x10000, 0, 0) # D3D11_USAGE_DYNAMIC = 2, D3D11_BIND_CONSTANT_BUFFER = 4, D3D11_CPU_ACCESS_WRITE = 0x10000
pCB = ctypes.c_void_p()
CreateBuffer(pDevice, ctypes.byref(bd), None, ctypes.byref(pCB))

# Map CB
Map_fn = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, wintypes.UINT, ctypes.POINTER(D3D11_MAPPED_SUBRESOURCE))(vtable_ctx[14])
Unmap_fn = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)(vtable_ctx[15])

mapped = D3D11_MAPPED_SUBRESOURCE()
# D3D11_MAP_WRITE_DISCARD = 4
hr = Map_fn(pContext, pCB, 0, 4, 0, ctypes.byref(mapped))
print("Map CB hr:", hex(hr & 0xFFFFFFFF))

# Pack 64 bytes
# uint2 OutputSize; uint2 FoveaBase; uint2 FoveaSize; uint2 NeuralSize;
# float FoveaRoundness; float FeatherWidth; uint FoveatedEnabled; uint ShowBorder; uint4 Padding;
cb_data = struct.pack(
    "IIIIIIIIffIIIIII",
    4040, 2696,  # OutputSize
    908, 604,    # FoveaBase
    2224, 1488,  # FoveaSize
    2224, 1488,  # NeuralSize
    0.5, 0.12,   # Roundness, Feather
    1, 0,        # FoveatedEnabled, ShowBorder
    0, 0, 0, 0   # Padding
)
ctypes.memmove(mapped.pData, cb_data, 64)
Unmap_fn(pContext, pCB, 0)

# Bind and Dispatch
CSSetShader = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT)(vtable_ctx[69])
CSSetCB = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_ctx[70])
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

dispatchX = (4040 + 15) // 16
dispatchY = (2696 + 15) // 16
print(f"Dispatching compute shader: {dispatchX} x {dispatchY} groups...")
Dispatch(pContext, dispatchX, dispatchY, 1)
print("Compute dispatch finished successfully!")
