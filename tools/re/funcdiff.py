"""For each function of selected modules in the new DLL (from llvm-pdbutil symbols), checks whether
identical code (rel32 / rip-relative displacements masked) exists in the old DLL's .text.

usage: funcdiff.py syms.txt new.dll old.dll module-substring...
"""
import re
import struct
import sys

import capstone

syms_path, new_path, old_path, *mods = sys.argv[1:]


def text_section(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + optsize
    for _ in range(nsec):
        name = data[sec:sec + 8].rstrip(b"\0").decode()
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
        if name == ".text":
            return data[rawptr:rawptr + vsize], va
        sec += 40
    raise SystemExit("no .text")


new_text, new_va = text_section(new_path)
old_text, old_va = text_section(old_path)

funcs = []
mod = None
for line in open(syms_path, encoding="utf-8", errors="replace"):
    m = re.match(r"\s*Mod \d+ \| `(.*)`", line)
    if m:
        mod = m.group(1)
        continue
    m = re.search(r"S_[GL]PROC32 \[size = \d+\] `(.*)`$", line.rstrip())
    if m:
        name = m.group(1)
        continue
    m = re.search(r"addr = 0001:(\d+), code size = (\d+)", line)
    if m and mod and any(s in mod for s in mods):
        funcs.append((mod.split("\\")[-1], name, int(m.group(1)), int(m.group(2))))

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def masked(code):
    """Bytes as a regex with displacement/immediate fields of rip-relative and relative branches masked."""
    out = bytearray(code)
    mask = [False] * len(code)
    for ins in md.disasm(code, 0):
        b = ins.address
        # rip-relative memory operand
        if ins.disp_offset and any(
                op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in ins.operands):
            for i in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                mask[b + i] = True
        # relative call/jmp/jcc
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
            if ins.imm_offset and ins.imm_size == 4:
                for i in range(ins.imm_offset, ins.imm_offset + 4):
                    mask[b + i] = True
        # 64-bit immediates (absolute addresses)
        if ins.mnemonic == "movabs":
            for i in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                mask[b + i] = True
    pat = b"".join(b"." if mask[i] else re.escape(bytes([out[i]])) for i in range(len(code)))
    return re.compile(pat, re.S)


same = diff = 0
for mod, name, off, size in funcs:
    code = new_text[off:off + size]
    if size < 8:
        continue
    if masked(code).search(old_text):
        same += 1
    else:
        diff += 1
        print(f"DIFF {mod:24} {size:6} {name}")
print(f"identical {same}, different {diff}")
