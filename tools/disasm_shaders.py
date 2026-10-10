"""Disassembles DXBC shaders saved by DS3MouseFix (dlaa_capture = 1) with Windows' own d3dcompiler_47.dll.

usage: python disasm_shaders.py <shader dir> <out dir> [id ...]
Writes <out dir>/<id>.<vs|ps|cs>.asm for every file (or only the given IDs).
"""
import ctypes
import os
import sys

d3d = ctypes.WinDLL("d3dcompiler_47.dll")
d3d.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
d3d.D3DDisassemble.restype = ctypes.c_long


def disassemble(code: bytes) -> str:
    blob = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(code, len(code))
    hr = d3d.D3DDisassemble(buf, len(code), 0, None, ctypes.byref(blob))
    if hr < 0:
        return f"; D3DDisassemble failed 0x{hr & 0xffffffff:08x}\n"
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vtbl = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])
    text = ctypes.string_at(get_ptr(blob), get_size(blob)).decode("ascii", "replace").rstrip("\0")
    release(blob)
    return text


def main():
    src, out = sys.argv[1], sys.argv[2]
    wanted = set(a.lower() for a in sys.argv[3:])
    os.makedirs(out, exist_ok=True)
    n = 0
    for name in os.listdir(src):
        if wanted and name.split(".")[0].lower() not in wanted:
            continue
        with open(os.path.join(src, name), "rb") as f:
            code = f.read()
        with open(os.path.join(out, name + ".asm"), "w") as f:
            f.write(disassemble(code))
        n += 1
    print(f"{n} shader(s) disassembled to {out}")


if __name__ == "__main__":
    main()
