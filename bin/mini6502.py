#!/usr/bin/env python3
"""
mini6502.py - a small 6502 emulator, just enough to execute cc64/c64asm
output and verify it behaves correctly. Not a full C64 emulator: no VIC,
no CIA, no real KERNAL. $FFD2 (CHROUT) is trapped in software: it prints
the PETSCII byte in A (mapped back to ASCII for readability) and returns.

Usage: python3 mini6502.py program.prg program.lst
"""
import sys

MEM = bytearray(65536)

def load_prg(path):
    with open(path, "rb") as f:
        data = f.read()
    addr = data[0] | (data[1] << 8)
    for i, b in enumerate(data[2:]):
        MEM[addr + i] = b
    return addr

def entry_point(lst_path):
    with open(lst_path) as f:
        for line in f:
            line = line.rstrip("\n")
            if len(line) >= 4 and all(c in "0123456789ABCDEF" for c in line[:4]):
                return int(line[:4], 16)
    raise RuntimeError("could not find entry point in listing")

def petscii_to_ascii(b, lowercase_mode):
    if lowercase_mode:
        # charset 2 ("lower/upper"): $41-$5A render lowercase, $C1-$DA render uppercase
        if 0x41 <= b <= 0x5A:
            return chr(b - 0x41 + ord('a'))
        if 0xC1 <= b <= 0xDA:
            return chr(b - 0xC1 + ord('A'))
    else:
        # charset 1 (default, "uppercase/graphics"): $41-$5A render uppercase;
        # $C1-$DA is the GRAPHICS range in this mode, not letters at all.
        if 0x41 <= b <= 0x5A:
            return chr(b)
        if 0xC1 <= b <= 0xDA:
            return "#"  # stand-in for an untypeable graphics glyph
    if b == 13:
        return "\n"
    if 32 <= b < 65:
        return chr(b)
    return "?"

class CPU:
    def __init__(self):
        self.a = 0; self.x = 0; self.y = 0
        self.sp = 0xFD
        self.pc = 0
        self.c = 0; self.z = 0; self.n = 0; self.v = 0
        self.output = []
        self.lowercase_mode = False  # C64 boots into charset 1 (uppercase/graphics)
        self.steps = 0
        self.max_steps = 20_000_000

    def push(self, v):
        MEM[0x100 + self.sp] = v & 0xFF
        self.sp = (self.sp - 1) & 0xFF
    def pop(self):
        self.sp = (self.sp + 1) & 0xFF
        return MEM[0x100 + self.sp]

    def setzn(self, v):
        v &= 0xFF
        self.z = 1 if v == 0 else 0
        self.n = 1 if v & 0x80 else 0
        return v

    def read(self, addr): return MEM[addr & 0xFFFF]
    def write(self, addr, v): MEM[addr & 0xFFFF] = v & 0xFF

    def imm(self):
        v = self.read(self.pc); self.pc += 1; return v
    def zp(self):
        return self.imm()
    def abs_(self):
        lo = self.imm(); hi = self.imm()
        return lo | (hi << 8)
    def indy_addr(self):
        zp = self.imm()
        lo = self.read(zp); hi = self.read((zp + 1) & 0xFF)
        base = lo | (hi << 8)
        return (base + self.y) & 0xFFFF

    def run(self, entry):
        # sentinel return address 0x0000 (via pushing 0xFFFF) so the
        # top-level RTS halts the emulator.
        self.push(0xFF); self.push(0xFF)
        self.pc = entry
        trace = []
        while True:
            self.steps += 1
            if self.steps > self.max_steps:
                raise RuntimeError("step limit exceeded (infinite loop?)")
            pcstart = self.pc
            op = self.read(self.pc); self.pc += 1
            trace.append((pcstart, op))
            if len(trace) > 20: trace.pop(0)
            try:
                self.exec1(op)
            except Exception as e:
                sys.stderr.write("CRASH near PC trace (addr,opcode): %s\n" % trace)
                raise
            if self.pc == 0x0000:
                break

    def branch(self, cond):
        off = self.imm()
        if off >= 0x80: off -= 0x100
        if cond:
            self.pc = (self.pc + off) & 0xFFFF

    def exec1(self, op):
        if op == 0xA9: self.a = self.setzn(self.imm())               # LDA #imm
        elif op == 0xA5: self.a = self.setzn(self.read(self.zp()))   # LDA zp
        elif op == 0xAD: self.a = self.setzn(self.read(self.abs_())) # LDA abs
        elif op == 0xB1: self.a = self.setzn(self.read(self.indy_addr()))  # LDA (zp),Y
        elif op == 0x85: self.write(self.zp(), self.a)               # STA zp
        elif op == 0x8D: self.write(self.abs_(), self.a)             # STA abs
        elif op == 0x91: self.write(self.indy_addr(), self.a)        # STA (zp),Y
        elif op == 0xA2: self.x = self.setzn(self.imm())             # LDX #imm
        elif op == 0xA6: self.x = self.setzn(self.read(self.zp()))   # LDX zp
        elif op == 0xAE: self.x = self.setzn(self.read(self.abs_())) # LDX abs
        elif op == 0x86: self.write(self.zp(), self.x)               # STX zp
        elif op == 0x8E: self.write(self.abs_(), self.x)             # STX abs
        elif op == 0xBD:                                              # LDA abs,X
            self.a = self.setzn(self.read(self.abs_() + self.x))
        elif op == 0x9D:                                              # STA abs,X
            self.write(self.abs_() + self.x, self.a)
        elif op == 0xA0: self.y = self.setzn(self.imm())             # LDY #imm
        elif op == 0xA4: self.y = self.setzn(self.read(self.zp()))   # LDY zp
        elif op == 0xAC: self.y = self.setzn(self.read(self.abs_())) # LDY abs
        elif op == 0x84: self.write(self.zp(), self.y)               # STY zp
        elif op == 0x8C: self.write(self.abs_(), self.y)             # STY abs
        elif op == 0xAA: self.x = self.setzn(self.a)                 # TAX
        elif op == 0xA8: self.y = self.setzn(self.a)                 # TAY
        elif op == 0x8A: self.a = self.setzn(self.x)                 # TXA
        elif op == 0x98: self.a = self.setzn(self.y)                 # TYA
        elif op == 0x18: self.c = 0                                   # CLC
        elif op == 0x38: self.c = 1                                   # SEC
        elif op == 0x69:                                              # ADC #imm
            v = self.imm(); self._adc(v)
        elif op == 0x65:
            v = self.read(self.zp()); self._adc(v)
        elif op == 0x6D:                                              # ADC abs
            v = self.read(self.abs_()); self._adc(v)
        elif op == 0xE9:                                              # SBC #imm
            v = self.imm(); self._sbc(v)
        elif op == 0xE5:
            v = self.read(self.zp()); self._sbc(v)
        elif op == 0xED:                                              # SBC abs
            v = self.read(self.abs_()); self._sbc(v)
        elif op == 0x29:                                              # AND #imm
            self.a = self.setzn(self.a & self.imm())
        elif op == 0x25:
            self.a = self.setzn(self.a & self.read(self.zp()))
        elif op == 0x2D:                                              # AND abs
            self.a = self.setzn(self.a & self.read(self.abs_()))
        elif op == 0x09:                                              # ORA #imm
            self.a = self.setzn(self.a | self.imm())
        elif op == 0x05:
            self.a = self.setzn(self.a | self.read(self.zp()))
        elif op == 0x0D:                                              # ORA abs
            self.a = self.setzn(self.a | self.read(self.abs_()))
        elif op == 0x49:                                              # EOR #imm
            self.a = self.setzn(self.a ^ self.imm())
        elif op == 0x45:
            self.a = self.setzn(self.a ^ self.read(self.zp()))
        elif op == 0x4D:                                              # EOR abs
            self.a = self.setzn(self.a ^ self.read(self.abs_()))
        elif op == 0x0A:                                              # ASL A
            self.c = (self.a >> 7) & 1
            self.a = self.setzn((self.a << 1) & 0xFF)
        elif op == 0x06:                                              # ASL zp
            addr = self.zp(); v = self.read(addr)
            self.c = (v >> 7) & 1
            self.write(addr, self.setzn((v << 1) & 0xFF))
        elif op == 0x0E:                                              # ASL abs
            addr = self.abs_(); v = self.read(addr)
            self.c = (v >> 7) & 1
            self.write(addr, self.setzn((v << 1) & 0xFF))
        elif op == 0x4A:                                              # LSR A
            self.c = self.a & 1
            self.a = self.setzn(self.a >> 1)
        elif op == 0x46:                                              # LSR zp
            addr = self.zp(); v = self.read(addr)
            self.c = v & 1
            self.write(addr, self.setzn(v >> 1))
        elif op == 0x4E:                                              # LSR abs
            addr = self.abs_(); v = self.read(addr)
            self.c = v & 1
            self.write(addr, self.setzn(v >> 1))
        elif op == 0x2A:                                              # ROL A
            newc = (self.a >> 7) & 1
            self.a = self.setzn(((self.a << 1) | self.c) & 0xFF)
            self.c = newc
        elif op == 0x26:                                              # ROL zp
            addr = self.zp(); v = self.read(addr)
            newc = (v >> 7) & 1
            self.write(addr, self.setzn(((v << 1) | self.c) & 0xFF))
            self.c = newc
        elif op == 0x2E:                                              # ROL abs
            addr = self.abs_(); v = self.read(addr)
            newc = (v >> 7) & 1
            self.write(addr, self.setzn(((v << 1) | self.c) & 0xFF))
            self.c = newc
        elif op == 0x6A:                                              # ROR A
            newc = self.a & 1
            self.a = self.setzn((self.a >> 1) | (self.c << 7))
            self.c = newc
        elif op == 0x66:                                              # ROR zp
            addr = self.zp(); v = self.read(addr)
            newc = v & 1
            self.write(addr, self.setzn((v >> 1) | (self.c << 7)))
            self.c = newc
        elif op == 0x6E:                                              # ROR abs
            addr = self.abs_(); v = self.read(addr)
            newc = v & 1
            self.write(addr, self.setzn((v >> 1) | (self.c << 7)))
            self.c = newc
        elif op == 0xE6:                                              # INC zp
            addr = self.zp(); self.write(addr, self.setzn(self.read(addr) + 1))
        elif op == 0xEE:                                              # INC abs
            addr = self.abs_(); self.write(addr, self.setzn(self.read(addr) + 1))
        elif op == 0xC6:                                              # DEC zp
            addr = self.zp(); self.write(addr, self.setzn(self.read(addr) - 1))
        elif op == 0xCE:                                              # DEC abs
            addr = self.abs_(); self.write(addr, self.setzn(self.read(addr) - 1))
        elif op == 0xE8: self.x = self.setzn(self.x + 1)              # INX
        elif op == 0xC8: self.y = self.setzn(self.y + 1)              # INY
        elif op == 0xCA: self.x = self.setzn(self.x - 1)              # DEX
        elif op == 0x88: self.y = self.setzn(self.y - 1)              # DEY
        elif op == 0xC9:                                              # CMP #imm
            v = self.imm(); self._cmp(self.a, v)
        elif op == 0xC5:
            v = self.read(self.zp()); self._cmp(self.a, v)
        elif op == 0xCD:                                              # CMP abs
            v = self.read(self.abs_()); self._cmp(self.a, v)
        elif op == 0xE0:                                              # CPX #imm
            v = self.imm(); self._cmp(self.x, v)
        elif op == 0xEC:                                              # CPX abs
            v = self.read(self.abs_()); self._cmp(self.x, v)
        elif op == 0xC0:                                              # CPY #imm
            v = self.imm(); self._cmp(self.y, v)
        elif op == 0xCC:                                              # CPY abs
            v = self.read(self.abs_()); self._cmp(self.y, v)
        elif op == 0x4C: self.pc = self.abs_()                        # JMP abs
        elif op == 0x20:                                              # JSR abs
            target = self.abs_()
            ret = self.pc - 1
            self.push((ret >> 8) & 0xFF); self.push(ret & 0xFF)
            self.pc = target
        elif op == 0x60:                                              # RTS
            lo = self.pop(); hi = self.pop()
            self.pc = (((hi << 8) | lo) + 1) & 0xFFFF
        elif op == 0xF0: self.branch(self.z == 1)                     # BEQ
        elif op == 0xD0: self.branch(self.z == 0)                     # BNE
        elif op == 0x30: self.branch(self.n == 1)                     # BMI
        elif op == 0x10: self.branch(self.n == 0)                     # BPL
        elif op == 0x90: self.branch(self.c == 0)                     # BCC
        elif op == 0xB0: self.branch(self.c == 1)                     # BCS
        elif op == 0x50: self.branch(self.v == 0)                     # BVC
        elif op == 0x70: self.branch(self.v == 1)                     # BVS
        elif op == 0xEA: pass                                         # NOP
        elif op == 0x00:                                              # BRK (unused, but just in case)
            raise RuntimeError("BRK executed")
        else:
            raise RuntimeError("unimplemented opcode $%02X at $%04X" % (op, self.pc-1))

        # Trap CHROUT: if PC lands on $FFD2 treat as a call
        if self.pc == 0xFFD2:
            # Real CHROUT (and the background keyboard-scan IRQ) touch a
            # documented set of zero-page bytes: $F3/$F4 (current line in
            # color RAM), $F5/$F6 (keyboard-matrix-to-PETSCII table
            # pointer), $F7-$FA (RS232 buffer pointers). A compiler that
            # depends on any of these surviving a CHROUT call has a real
            # bug that won't show up here unless we simulate the clobber -
            # this is exactly the class of bug that shipped once already.
            for addr in (0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA):
                MEM[addr] = 0xEE  # arbitrary poison byte, not 0x00
            if self.a == 14:
                self.lowercase_mode = True
            elif self.a == 142:
                self.lowercase_mode = False
            else:
                self.output.append(petscii_to_ascii(self.a, self.lowercase_mode))
            # perform an RTS immediately
            lo = self.pop(); hi = self.pop()
            self.pc = (((hi << 8) | lo) + 1) & 0xFFFF

    def _adc(self, v):
        total = self.a + v + self.c
        result = total & 0xFF
        self.v = 1 if (~(self.a ^ v) & (self.a ^ result) & 0x80) else 0
        self.c = 1 if total > 0xFF else 0
        self.a = self.setzn(result)

    def _sbc(self, v):
        total = self.a - v - (1 - self.c)
        result = total & 0xFF
        self.v = 1 if ((self.a ^ v) & (self.a ^ result) & 0x80) else 0
        self.c = 1 if total >= 0 else 0
        self.a = self.setzn(result)

    def _cmp(self, reg, v):
        total = reg - v
        self.c = 1 if reg >= v else 0
        self.setzn(total & 0xFF)


def main():
    prg, lst = sys.argv[1], sys.argv[2]
    load_prg(prg)
    entry = entry_point(lst)
    cpu = CPU()
    cpu.run(entry)
    sys.stdout.write("".join(cpu.output))
    sys.stdout.write("\n")
    sys.stderr.write("[ok: %d instructions executed]\n" % cpu.steps)

if __name__ == "__main__":
    main()
