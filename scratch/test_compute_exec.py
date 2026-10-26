import ctypes
from ctypes import wintypes

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
print("CreateDevice:", hex(hr & 0xFFFFFFFF))

with open(r"C:\code\hlx-dlss5\include\foveated_composite_shader.hpp", "r", encoding="utf-8") as f:
    text = f.read()

s_start = text.find("Texture2D<float4> NativeColor")
s_end = text.find(')";', s_start)
hlsl = text[s_start:s_end].encode("utf-8")

blob = ctypes.c_void_p()
err = ctypes.c_void_p()
hr = d3dcompiler.D3DCompile(
    hlsl, len(hlsl), None, None, None, b"CSMain", b"cs_5_0", 0, 0,
    ctypes.byref(blob), ctypes.byref(err)
)
print("Compile hr:", hex(hr & 0xFFFFFFFF))
if hr != 0 and err.value:
    msg = ctypes.string_at(vtable_blob_get_buffer(err))
    print("Err:", msg)

# Create Compute Shader
vtable_dev = ctypes.cast(pDevice, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
vtable_blob = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents

# ID3D10Blob: GetBufferPointer = 3, GetBufferSize = 4
GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable_blob[3])
GetBufferSize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable_blob[4])

ptr = GetBufferPointer(blob)
sz = GetBufferSize(blob)
print("Blob size:", sz)

# CreateComputeShader: slot 12
CreateCS = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[12])
pCS = ctypes.c_void_p()
hr = CreateCS(pDevice, ptr, sz, None, ctypes.byref(pCS))
print("CreateCS hr:", hex(hr & 0xFFFFFFFF), "pCS:", pCS.value)
