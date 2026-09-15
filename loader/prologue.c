/* prologue.c -- moving the first few instructions of a function somewhere else
 *
 * Timing a function means getting control before and after it, which means
 * jumping away from its first instruction and then running that instruction
 * somewhere else. so_util's hook_addr only does the first half: it overwrites
 * the first eight bytes and never gives them back, which is right for a
 * function the loader replaces outright and useless for one it wants to wrap.
 *
 * So the bytes it would destroy are copied to a scratch buffer, followed by a
 * jump back to what comes after them. Copying only works if the instructions
 * mean the same thing at the new address, so this walks a whitelist of prologue
 * shapes and refuses everything it does not positively recognise. A function
 * that cannot be relocated is simply not hooked, which costs a measurement; a
 * function relocated wrongly costs the game.
 *
 * The one exception is a literal load, which is both unsafe to move and far too
 * common to refuse -- position independent code reaches its globals that way,
 * and it is the first instruction of CGame::Process, CWorld::Process and
 * CStreaming::Update among others. Those are relocated by value instead: the
 * word is read once, here, after the module is relocated and its contents are
 * final, and the load becomes the two instructions that build that word out of
 * nothing.
 *
 * In its own file, with no Vita headers in it, so that it can be run over the
 * real libBully.so on a build machine rather than found out on the console.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "prologue.h"

static int thumb_len(uint16_t hw) {
  uint16_t top5 = hw & 0xF800;
  return (top5 == 0xE800 || top5 == 0xF000 || top5 == 0xF800) ? 4 : 2;
}

// A whitelist of what compilers put at the top of a function, not an attempt to
// enumerate what is unsafe. Everything that reads PC is absent by construction:
// no ADR (0xA000-0xA7FF), no branches (0xD000-0xDFFF, 0xE000-0xE7FF), no
// CBZ/CBNZ, no BX/BLX. Literal loads are handled separately, by value, below.
static int thumb16_movable(uint16_t hw) {
  if (hw >= 0xB400 && hw <= 0xB5FF)
    return 1; // PUSH
  if (hw >= 0xB000 && hw <= 0xB0FF)
    return 1; // ADD/SUB SP, #imm
  if (hw >= 0xA800 && hw <= 0xAFFF)
    return 1; // ADD Rd, SP, #imm
  if (hw <= 0x1FFF)
    return 1; // shifts, ADD/SUB, low registers only
  if (hw >= 0x2000 && hw <= 0x2FFF)
    return 1; // MOV/CMP/ADD/SUB Rd, #imm
  if (hw >= 0x4000 && hw <= 0x43FF)
    return 1; // data processing, low registers only
  if (hw >= 0x4400 && hw <= 0x46FF) {
    int rm = (hw >> 3) & 0xF;
    int rd = (hw & 7) | ((hw >> 4) & 8);
    return rm != 15 && rd != 15; // ADD/CMP/MOV high registers, but never PC
  }
  if (hw >= 0x6000 && hw <= 0x9FFF)
    return 1; // LDR/STR/LDRB/STRB/LDRH/STRH immediate, low registers or SP
  return 0;
}

static int thumb32_movable(uint16_t hw1) {
  if ((hw1 & 0xFFE0) == 0xE920)
    return (hw1 & 0xF) == 13; // STMDB sp!, {...}
  if ((hw1 & 0xFFBF) == 0xED2D)
    return 1; // VPUSH
  if ((hw1 & 0xFBF0) == 0xF1A0 || (hw1 & 0xFBF0) == 0xF2A0 || (hw1 & 0xFBF0) == 0xF100 ||
      (hw1 & 0xFBF0) == 0xF200)
    return (hw1 & 0xF) != 15; // ADD/SUB Rd, Rn, #imm -- Rn 15 is ADR
  if ((hw1 & 0xFBF0) == 0xF240 || (hw1 & 0xFBF0) == 0xF2C0)
    return 1; // MOVW / MOVT, absolute
  if ((hw1 & 0xFF00) == 0xF800 || (hw1 & 0xFF00) == 0xF900)
    return (hw1 & 0xF) != 15; // LDR/STR family with a register base, never PC
  return 0;
}

static int arm_movable(uint32_t w) {
  if ((w & 0x0FFF0000) == 0x092D0000)
    return 1; // STMDB sp!, {...}
  if ((w & 0x0FEF0000) == 0x024D0000)
    return 1; // ADD/SUB sp, sp, #imm
  if ((w & 0x0C000000) == 0x00000000) {
    int rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rm = w & 0xF;
    int imm = (w >> 25) & 1;
    if (rn != 15 && rd != 15 && (imm || rm != 15))
      return 1; // data processing that never touches PC
  }
  return 0;
}

// A Thumb literal load reads a word out of the pool that follows the function.
// Moving the instruction would move what it reads, so it is not moved: the word
// is read here, once, after the module is relocated and its contents are final.
// Returns 0 if this is not a literal load.
static int literal_load(uintptr_t addr, const uint8_t *src, int *rt, uint32_t *value) {
  uint16_t hw1 = (uint16_t)(src[0] | (src[1] << 8));
  int imm;
  if ((hw1 & 0xF800) == 0x4800) { // LDR Rt, [PC, #imm8*4]
    *rt = (hw1 >> 8) & 7;
    imm = (hw1 & 0xFF) * 4;
  } else if ((hw1 & 0xFF7F) == 0xF85F) { // LDR.W Rt, [PC, #+/-imm12]
    uint16_t hw2 = (uint16_t)(src[2] | (src[3] << 8));
    *rt = (hw2 >> 12) & 0xF;
    imm = hw2 & 0xFFF;
    if (!(hw1 & 0x0080))
      imm = -imm;
  } else {
    return 0;
  }
  // uintptr_t rather than uint32_t all the way through: on the console these
  // are the same and on a build machine they are not, and this has to be
  // runnable on a build machine or it can only be tested by shipping it.
  uintptr_t base = (addr + 4) & ~(uintptr_t)3;
  *value = *(const uint32_t *)(base + (intptr_t)imm);
  return 1;
}

// Emits MOVW/MOVT to put `value` in `rt`. Eight bytes.
static void emit_constant(int rt, uint32_t value, uint8_t *out) {
  uint32_t lo = value & 0xFFFF, hi = value >> 16;
  uint16_t w[4];
  w[0] = (uint16_t)(0xF240 | ((lo >> 12) & 0xF) | (((lo >> 11) & 1) << 10));
  w[1] = (uint16_t)((rt << 8) | (lo & 0xFF) | (((lo >> 8) & 7) << 12));
  w[2] = (uint16_t)(0xF2C0 | ((hi >> 12) & 0xF) | (((hi >> 11) & 1) << 10));
  w[3] = (uint16_t)((rt << 8) | (hi & 0xFF) | (((hi >> 8) & 7) << 12));
  for (int i = 0; i < 4; i++) {
    out[i * 2] = (uint8_t)w[i];
    out[i * 2 + 1] = (uint8_t)(w[i] >> 8);
  }
}

// An instruction that cannot write a general register, so a value tracked into
// one survives it. Deliberately short: pushes and stack adjustment, nothing else.
static int leaves_registers_alone(uint16_t hw1, int len) {
  if (len == 2)
    return (hw1 >= 0xB400 && hw1 <= 0xB5FF) || (hw1 >= 0xB000 && hw1 <= 0xB0FF);
  return (hw1 & 0xFFE0) == 0xE920 || (hw1 & 0xFFBF) == 0xED2D;
}

int prologue_relocate(uintptr_t addr, const uint8_t *src, int thumb, int need, uint8_t *out,
                      int out_size, int *out_len) {
  int taken = 0, written = 0;
  // What a literal load put in each register, for the sequence that follows one
  // with ADD Rd, PC. That pair is how position independent code turns an offset
  // in the pool into the address of a global, and it is the first thing
  // ScriptManager::Update, hal::Audio::Update and CClothingManager::Update do.
  // Neither half can be moved on its own; together they are a constant, which
  // can. Anything that could write a register clears the table.
  uint32_t lit_val[16];
  uint8_t lit_ok[16];
  memset(lit_ok, 0, sizeof(lit_ok));
  while (taken < need) {
    if (written + 16 > out_size)
      return 0;
    if (thumb) {
      uint16_t hw1 = (uint16_t)(src[taken] | (src[taken + 1] << 8));
      int n = thumb_len(hw1);
      int emitted = 0, rt = -1;
      uint32_t value = 0;
      if (literal_load(addr + taken, src + taken, &rt, &value)) {
        emit_constant(rt, value, out + written);
        lit_val[rt] = value;
        lit_ok[rt] = 1;
        written += 8;
      } else if (n == 2 && (hw1 & 0xFF78) == 0x4478) {
        // ADD Rd, PC, with Rd a low register. Only foldable when we know what
        // is already in Rd; otherwise it is a PC read like any other.
        int rd = (hw1 & 7) | ((hw1 >> 4) & 8);
        if (!lit_ok[rd])
          return 0;
        emit_constant(rd, lit_val[rd] + (uint32_t)(addr + taken) + 4, out + written);
        lit_ok[rd] = 0;
        written += 8;
      } else if (n == 2 ? thumb16_movable(hw1) : thumb32_movable(hw1)) {
        memcpy(out + written, src + taken, n);
        written += n;
        if (!leaves_registers_alone(hw1, n))
          memset(lit_ok, 0, sizeof(lit_ok));
      } else {
        return 0;
      }
      (void)emitted;
      taken += n;
    } else {
      uint32_t w = (uint32_t)(src[taken] | (src[taken + 1] << 8) | (src[taken + 2] << 16) |
                              (src[taken + 3] << 24));
      if (!arm_movable(w))
        return 0;
      memcpy(out + written, src + taken, 4);
      written += 4;
      taken += 4;
    }
  }
  // Then back to whatever followed the bytes we took. LDR PC reads its literal
  // from a four byte boundary, so pad first if the copy left us halfway.
  if (thumb && (written & 2)) {
    out[written] = 0x00;
    out[written + 1] = 0xbf; // NOP
    written += 2;
  }
  if (written + 8 > out_size)
    return 0;
  uint32_t jump = thumb ? 0xf000f8df : 0xe51ff004; // LDR PC, [PC] / LDR PC, [PC, #-4]
  uint32_t back = (uint32_t)(addr + taken) | (thumb ? 1u : 0u);
  memcpy(out + written, &jump, 4);
  memcpy(out + written + 4, &back, 4);
  written += 8;
  *out_len = written;
  return taken;
}

int prologue_relocatable(const uint8_t *code, int thumb, int need) {
  // The value-reading path needs a live module, so the survey form answers the
  // narrower question: would every instruction here move as it stands?
  int taken = 0;
  while (taken < need) {
    if (thumb) {
      uint16_t hw1 = (uint16_t)(code[taken] | (code[taken + 1] << 8));
      int n = thumb_len(hw1);
      if (!(n == 2 ? thumb16_movable(hw1) : thumb32_movable(hw1)))
        return 0;
      taken += n;
    } else {
      uint32_t w = (uint32_t)(code[taken] | (code[taken + 1] << 8) | (code[taken + 2] << 16) |
                              (code[taken + 3] << 24));
      if (!arm_movable(w))
        return 0;
      taken += 4;
    }
  }
  return taken;
}

