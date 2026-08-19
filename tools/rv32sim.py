#!/usr/bin/env python3
"""An RV32IM interpreter, so firmware C can be RUN on this machine.

WHY THIS EXISTS: there is no host C compiler here and no RISC-V simulator, so
every check of firmware logic had to be a Python re-implementation of what the
C was believed to do. That failed twice in a row on the FLAC post-seek clock,
both times convincingly -- one model ignored seektables and measured a code
path the test files never take, the other passed 130 assertions on a rule that
broke seeking outright on hardware. A model of the wrong thing cannot fail the
way the real thing fails.

This runs the actual fw/flac.c, compiled by the actual toolchain, so there is
no model left to be wrong. rv32im only: fw/build.sh targets it, so there are
no compressed instructions to decode.

Flat memory plus a small MMIO block, which is enough to give a freestanding
harness a console and a way to read a real .flac from the host filesystem
without embedding 30 MB in the binary:

    0xF0000000  w  putchar
    0xF0000004  w  exit with this code
    0xF0000010  w  DMA destination address
    0xF0000014  w  DMA source offset within the file
    0xF0000018  w  DMA length -- performs the copy; r returns bytes copied
    0xF000001C  r  file size
"""
import struct
import sys

MMIO = 0xF0000000
M32 = 0xFFFFFFFF


class Machine:
    def __init__(self, mem_size=1 << 23):
        self.mem = bytearray(mem_size)
        self.size = mem_size
        self.reg = [0] * 32
        self.pc = 0
        self.out = bytearray()
        self.blob = b''
        self.dma_dst = self.dma_src = self.dma_len = 0
        self.halted = None
        self.icount = 0

    # ---- ELF ---------------------------------------------------------------
    def load_elf(self, path):
        d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1:
            raise SystemExit('not a 32-bit ELF')
        entry, phoff = struct.unpack_from('<II', d, 24)
        phentsize, phnum = struct.unpack_from('<HH', d, 42)
        for i in range(phnum):
            o = phoff + i * phentsize
            p_type, p_offset, p_vaddr, _pa, p_filesz, p_memsz = \
                struct.unpack_from('<IIIIII', d, o)
            if p_type != 1:
                continue
            if p_vaddr + p_memsz > self.size:
                raise SystemExit('segment past end of RAM: 0x%X' % p_vaddr)
            self.mem[p_vaddr:p_vaddr + p_filesz] = d[p_offset:p_offset + p_filesz]
            for a in range(p_vaddr + p_filesz, p_vaddr + p_memsz):
                self.mem[a] = 0
        self.pc = entry
        return entry

    # ---- MMIO --------------------------------------------------------------
    def flush(self):
        if self.out:
            sys.stdout.write(self.out.decode('latin1'))
            sys.stdout.flush()
            del self.out[:]

    def mmio_store(self, addr, val):
        off = addr - MMIO
        if off == 0:
            self.out.append(val & 0xFF)
            if val == 10:
                self.flush()
        elif off == 4:
            self.halted = val
        elif off == 0x10:
            self.dma_dst = val
        elif off == 0x14:
            self.dma_src = val
        elif off == 0x18:
            chunk = self.blob[self.dma_src:self.dma_src + val]
            self.mem[self.dma_dst:self.dma_dst + len(chunk)] = chunk
            self.dma_len = len(chunk)
        else:
            raise SystemExit('bad MMIO store 0x%X' % addr)

    def mmio_load(self, addr):
        off = addr - MMIO
        if off == 0x18:
            return self.dma_len
        if off == 0x1C:
            return len(self.blob)
        raise SystemExit('bad MMIO load 0x%X' % addr)

    # ---- run ---------------------------------------------------------------
    def run(self, max_instr=4000000000):
        mem, reg = self.mem, self.reg
        pc = self.pc
        n = 0
        while n < max_instr:
            n += 1
            ins = (mem[pc] | (mem[pc + 1] << 8) |
                   (mem[pc + 2] << 16) | (mem[pc + 3] << 24))
            op = ins & 0x7F
            rd = (ins >> 7) & 0x1F
            f3 = (ins >> 12) & 7
            rs1 = (ins >> 15) & 0x1F
            rs2 = (ins >> 20) & 0x1F
            npc = pc + 4

            if op == 0x13:                                   # OP-IMM
                imm = ins >> 20
                if imm & 0x800:
                    imm -= 0x1000
                a = reg[rs1]
                sa = a - 0x100000000 if a & 0x80000000 else a
                if f3 == 0:
                    v = (a + imm) & M32
                elif f3 == 1:
                    v = (a << rs2) & M32
                elif f3 == 2:
                    v = 1 if sa < imm else 0
                elif f3 == 3:
                    v = 1 if a < (imm & M32) else 0
                elif f3 == 4:
                    v = a ^ (imm & M32)
                elif f3 == 5:
                    v = ((sa >> rs2) & M32) if (ins & 0x40000000) else (a >> rs2)
                elif f3 == 6:
                    v = a | (imm & M32)
                else:
                    v = a & (imm & M32)
                if rd:
                    reg[rd] = v
            elif op == 0x33:                                 # OP
                a, b = reg[rs1], reg[rs2]
                sa = a - 0x100000000 if a & 0x80000000 else a
                sb = b - 0x100000000 if b & 0x80000000 else b
                if ins & 0x02000000:                         # M extension
                    if f3 == 0:
                        v = (a * b) & M32
                    elif f3 == 1:
                        v = ((sa * sb) >> 32) & M32
                    elif f3 == 2:
                        v = ((sa * b) >> 32) & M32
                    elif f3 == 3:
                        v = ((a * b) >> 32) & M32
                    elif f3 == 4:                            # DIV
                        if sb == 0:
                            v = M32
                        elif sa == -0x80000000 and sb == -1:
                            v = 0x80000000
                        else:
                            q = abs(sa) // abs(sb)
                            v = (q if (sa < 0) == (sb < 0) else -q) & M32
                    elif f3 == 5:                            # DIVU
                        v = M32 if b == 0 else (a // b) & M32
                    elif f3 == 6:                            # REM
                        if sb == 0:
                            v = a
                        elif sa == -0x80000000 and sb == -1:
                            v = 0
                        else:
                            q = abs(sa) // abs(sb)
                            q = q if (sa < 0) == (sb < 0) else -q
                            v = (sa - q * sb) & M32
                    else:                                    # REMU
                        v = a if b == 0 else (a % b) & M32
                else:
                    alt = ins & 0x40000000
                    if f3 == 0:
                        v = ((a - b) if alt else (a + b)) & M32
                    elif f3 == 1:
                        v = (a << (b & 31)) & M32
                    elif f3 == 2:
                        v = 1 if sa < sb else 0
                    elif f3 == 3:
                        v = 1 if a < b else 0
                    elif f3 == 4:
                        v = a ^ b
                    elif f3 == 5:
                        v = ((sa >> (b & 31)) & M32) if alt else (a >> (b & 31))
                    elif f3 == 6:
                        v = a | b
                    else:
                        v = a & b
                if rd:
                    reg[rd] = v
            elif op == 0x03:                                 # LOAD
                imm = ins >> 20
                if imm & 0x800:
                    imm -= 0x1000
                addr = (reg[rs1] + imm) & M32
                if addr >= MMIO:
                    v = self.mmio_load(addr)
                elif f3 == 2:
                    v = (mem[addr] | (mem[addr + 1] << 8) |
                         (mem[addr + 2] << 16) | (mem[addr + 3] << 24))
                elif f3 == 4:
                    v = mem[addr]
                elif f3 == 0:
                    v = mem[addr]
                    v = (v - 256) & M32 if v & 0x80 else v
                elif f3 == 5:
                    v = mem[addr] | (mem[addr + 1] << 8)
                else:
                    v = mem[addr] | (mem[addr + 1] << 8)
                    v = (v - 0x10000) & M32 if v & 0x8000 else v
                if rd:
                    reg[rd] = v
            elif op == 0x23:                                 # STORE
                imm = ((ins >> 7) & 0x1F) | ((ins >> 20) & 0xFE0)
                if imm & 0x800:
                    imm -= 0x1000
                addr = (reg[rs1] + imm) & M32
                v = reg[rs2]
                if addr >= MMIO:
                    self.mmio_store(addr, v)
                    if self.halted is not None:
                        break
                elif f3 == 2:
                    mem[addr] = v & 0xFF
                    mem[addr + 1] = (v >> 8) & 0xFF
                    mem[addr + 2] = (v >> 16) & 0xFF
                    mem[addr + 3] = (v >> 24) & 0xFF
                elif f3 == 0:
                    mem[addr] = v & 0xFF
                else:
                    mem[addr] = v & 0xFF
                    mem[addr + 1] = (v >> 8) & 0xFF
            elif op == 0x63:                                 # BRANCH
                a, b = reg[rs1], reg[rs2]
                if f3 == 0:
                    t = a == b
                elif f3 == 1:
                    t = a != b
                elif f3 in (4, 5):
                    sa = a - 0x100000000 if a & 0x80000000 else a
                    sb = b - 0x100000000 if b & 0x80000000 else b
                    t = (sa < sb) if f3 == 4 else (sa >= sb)
                elif f3 == 6:
                    t = a < b
                else:
                    t = a >= b
                if t:
                    imm = ((((ins >> 8) & 0xF) << 1) | (((ins >> 25) & 0x3F) << 5) |
                           (((ins >> 7) & 1) << 11) | (((ins >> 31) & 1) << 12))
                    if imm & 0x1000:
                        imm -= 0x2000
                    npc = (pc + imm) & M32
            elif op == 0x6F:                                 # JAL
                imm = ((((ins >> 21) & 0x3FF) << 1) | (((ins >> 20) & 1) << 11) |
                       (((ins >> 12) & 0xFF) << 12) | (((ins >> 31) & 1) << 20))
                if imm & 0x100000:
                    imm -= 0x200000
                if rd:
                    reg[rd] = npc
                npc = (pc + imm) & M32
            elif op == 0x67:                                 # JALR
                imm = ins >> 20
                if imm & 0x800:
                    imm -= 0x1000
                t = (reg[rs1] + imm) & M32 & ~1
                if rd:
                    reg[rd] = npc
                npc = t
            elif op == 0x37:                                 # LUI
                if rd:
                    reg[rd] = ins & 0xFFFFF000
            elif op == 0x17:                                 # AUIPC
                if rd:
                    reg[rd] = (pc + (ins & 0xFFFFF000)) & M32
            elif op == 0x0F:                                 # FENCE
                pass
            elif op == 0x73:                                 # ECALL / EBREAK
                self.halted = reg[10]
                break
            else:
                raise SystemExit('illegal opcode 0x%02X at pc=0x%X' % (op, pc))
            pc = npc
        self.pc = pc
        self.icount = n
        self.flush()
        return self.halted


def main():
    if len(sys.argv) < 2:
        print('usage: rv32sim.py <elf> [datafile]')
        return 2
    m = Machine()
    m.load_elf(sys.argv[1])
    if len(sys.argv) > 2:
        m.blob = open(sys.argv[2], 'rb').read()
    rc = m.run()
    sys.stderr.write('[%s instructions, exit %s]\n' % (format(m.icount, ','), rc))
    return 0 if rc in (0, None) else 1


if __name__ == '__main__':
    sys.exit(main())
