/* test_prologue.c -- moving a function's first instructions somewhere else
 *
 * This is the part of the frame profiler that can break the game rather than
 * merely fail to measure it: a prologue copied to a trampoline has to mean the
 * same thing at its new address. So it is tested twice over -- on instruction
 * sequences chosen to sit either side of every rule, and, when BULLY_SO points
 * at the real libBully.so, on the actual prologues of the actual functions the
 * profiler hooks.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "../loader/prologue.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define NEED 8

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  uint8_t code[64], out[64];
  int len;

  // A plain Thumb prologue: push, mov, sub sp. All of it moves.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0xB5F0); // push {r4-r7, lr}
  put16(code + 2, 0x4604); // mov  r4, r0
  put16(code + 4, 0xB086); // sub  sp, #24
  put16(code + 6, 0x460D); // mov  r5, r1
  put16(code + 8, 0x4616); // mov  r6, r2
  int took = prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len);
  assert(took == 8 && "four two-byte instructions cover the eight bytes hook_addr wants");
  assert(len == 16 && "eight bytes copied, then the eight byte jump back");
  assert(memcmp(out, code, 8) == 0 && "movable instructions are copied verbatim");
  assert(*(uint32_t *)(out + 8) == 0xf000f8df && "and then LDR PC, [PC]");
  assert(*(uint32_t *)(out + 12) == (((uint32_t)(uintptr_t)code + 8) | 1) &&
         "which lands back in the original, in Thumb");
  printf("thumb plain  : %d bytes taken, %d written, jump back set  OK\n", took, len);

  // A branch in the prologue is refused rather than guessed at. This is the
  // shape that stops CPatrolManager::Update and CPedSocial::Update being
  // hooked, and it must stay refused: relocating it would send the game to an
  // address computed from the wrong PC.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0xB580); // push {r7, lr}
  put16(code + 2, 0xB193); // cbz  r3, +x
  assert(prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len) == 0 &&
         "a conditional branch in the prologue must be refused");
  put16(code + 2, 0xF7FC); // bl
  put16(code + 4, 0xFFFE);
  assert(prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len) == 0 &&
         "and so must a call");
  put16(code + 2, 0xA001); // adr r0, +4
  assert(prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len) == 0 &&
         "and an ADR, which is a PC read wearing a hat");
  printf("thumb branch : branches, calls and ADR all refused      OK\n");

  // A literal load is not refused: it is replaced by the value it would have
  // read. Anything else would leave the busiest functions in the engine
  // unmeasurable, since position independent code reaches its globals this way.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0xB580); // push {r7, lr}
  put16(code + 2, 0x4802); // ldr  r0, [pc, #8]
  put16(code + 4, 0x4604); // mov  r4, r0
  put16(code + 6, 0x4608); // mov  r0, r1
  // pc for a Thumb instruction at +2 is +4 from it, rounded down to four: +4.
  *(uint32_t *)(code + 4 + 8) = 0xDEADBEEF;
  took = prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len);
  assert(took == 8 && "still eight source bytes");
  // push 2 + literal 8 + mov 2 + mov 2 = 14, padded to 16 so the jump's target
  // sits on a four byte boundary, then the eight byte jump itself.
  assert(len == 24 && "the two byte load became eight bytes, and the jump stayed aligned");
  assert(out[0] == 0x80 && out[1] == 0xB5 && "the push is still first");
  // MOVW r0, #0xBEEF ; MOVT r0, #0xDEAD
  uint16_t w0 = (uint16_t)(out[2] | (out[3] << 8)), w1 = (uint16_t)(out[4] | (out[5] << 8));
  uint16_t w2 = (uint16_t)(out[6] | (out[7] << 8)), w3 = (uint16_t)(out[8] | (out[9] << 8));
  uint32_t lo = (uint32_t)((w1 & 0xFF) | ((w1 >> 12) & 7) << 8 | ((w0 & 0xF) << 12) |
                           (((w0 >> 10) & 1) << 11));
  uint32_t hi = (uint32_t)((w3 & 0xFF) | ((w3 >> 12) & 7) << 8 | ((w2 & 0xF) << 12) |
                           (((w2 >> 10) & 1) << 11));
  assert((w0 & 0xFBF0) == 0xF240 && "MOVW");
  assert((w2 & 0xFBF0) == 0xF2C0 && "MOVT");
  assert(((w1 >> 8) & 0xF) == 0 && ((w3 >> 8) & 0xF) == 0 && "into the register the load used");
  assert(lo == 0xBEEF && hi == 0xDEAD && "carrying the word the load would have read");
  printf("thumb literal: ldr r0,[pc] became movw/movt 0x%04x%04x    OK\n", hi, lo);

  // The pair that reaches a global in position independent code: a literal load
  // of an offset, then ADD Rd, PC. Neither half survives being moved on its own
  // -- the load reads the wrong pool, the add reads the wrong PC -- but between
  // them they compute a constant, and a constant moves. This is the first thing
  // ScriptManager::Update, hal::Audio::Update and CClothingManager::Update do,
  // and without folding it none of the three can be measured.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0x4B02); // ldr  r3, [pc, #8]
  put16(code + 2, 0xB580); // push {r7, lr}      -- writes no register, so r3 survives
  put16(code + 4, 0x447B); // add  r3, pc
  put16(code + 6, 0x4604); // mov  r4, r0
  *(uint32_t *)(code + 12) = 0x00001000; // pc for the ldr is (code+4)&~3 = code+4, +8
  took = prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len);
  assert(took == 8 && "four instructions, eight source bytes");
  {
    uint16_t a0 = (uint16_t)(out[0] | (out[1] << 8)), a1 = (uint16_t)(out[2] | (out[3] << 8));
    uint16_t b0 = (uint16_t)(out[10] | (out[11] << 8)), b1 = (uint16_t)(out[12] | (out[13] << 8));
    uint16_t b2 = (uint16_t)(out[14] | (out[15] << 8)), b3 = (uint16_t)(out[16] | (out[17] << 8));
    assert((a0 & 0xFBF0) == 0xF240 && ((a1 >> 8) & 0xF) == 3 && "the load became MOVW r3");
    assert(out[8] == 0x80 && out[9] == 0xB5 && "the push stayed where it was, after it");
    assert((b0 & 0xFBF0) == 0xF240 && ((b1 >> 8) & 0xF) == 3 && "the add became MOVW r3 too");
    uint32_t lo2 = (uint32_t)((b1 & 0xFF) | (((b1 >> 12) & 7) << 8) | ((b0 & 0xF) << 12) |
                              (((b0 >> 10) & 1) << 11));
    uint32_t hi2 = (uint32_t)((b3 & 0xFF) | (((b3 >> 12) & 7) << 8) | ((b2 & 0xF) << 12) |
                              (((b2 >> 10) & 1) << 11));
    uint32_t want = 0x1000 + (uint32_t)(uintptr_t)(code + 4) + 4;
    assert((lo2 | (hi2 << 16)) == want && "carrying offset + the PC the add would have read");
  }
  printf("thumb pic    : ldr+add pc folded to the address it computes  OK\n");

  // Only when the register's value is known. An ADD Rd, PC out of nowhere is a
  // PC read like any other and has to be refused.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0xB580); // push {r7, lr}
  put16(code + 2, 0x447B); // add  r3, pc
  assert(prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len) == 0 &&
         "ADD Rd, PC with nothing tracked into Rd must be refused");
  // And the tracking has to be dropped by anything that could overwrite it.
  memset(code, 0, sizeof(code));
  put16(code + 0, 0x4B02); // ldr r3, [pc, #8]
  put16(code + 2, 0x1C1B); // adds r3, r3, #0   -- writes r3
  put16(code + 4, 0x447B); // add r3, pc
  *(uint32_t *)(code + 12) = 0x1000;
  assert(prologue_relocate((uintptr_t)code, code, 1, NEED, out, sizeof(out), &len) == 0 &&
         "a write to the tracked register must invalidate the fold");
  printf("thumb pic    : unfoldable ADD Rd, PC still refused        OK\n");

  // An entry two bytes off a four byte boundary: hook_addr writes a NOP first,
  // so ten bytes have to come back, and the jump has to stay four byte aligned
  // or LDR PC reads its target from the wrong place.
  memset(code, 0, sizeof(code));
  for (int i = 0; i < 6; i++)
    put16(code + i * 2, 0x4604); // mov r4, r0
  uintptr_t odd = (uintptr_t)code + 2;
  took = prologue_relocate(odd, code + 2, 1, 8 + 2, out, sizeof(out), &len);
  assert(took == 10 && "ten bytes when the entry is not four byte aligned");
  assert((len - 8) % 4 == 0 && "the jump back starts on a four byte boundary");
  printf("thumb offset : %d bytes taken, jump aligned at %d        OK\n", took, len - 8);

  // ARM, for the handful of functions that are not Thumb.
  memset(code, 0, sizeof(code));
  *(uint32_t *)(code + 0) = 0xE92D4010; // push {r4, lr}
  *(uint32_t *)(code + 4) = 0xE24DD010; // sub  sp, sp, #16
  took = prologue_relocate((uintptr_t)code, code, 0, NEED, out, sizeof(out), &len);
  assert(took == 8 && len == 16);
  assert(*(uint32_t *)(out + 8) == 0xe51ff004 && "LDR PC, [PC, #-4]");
  assert(*(uint32_t *)(out + 12) == (uint32_t)(uintptr_t)code + 8 && "no Thumb bit in ARM");
  *(uint32_t *)(code + 4) = 0xE59F0010; // ldr r0, [pc, #16]
  assert(prologue_relocate((uintptr_t)code, code, 0, NEED, out, sizeof(out), &len) == 0 &&
         "an ARM literal load is refused, not rewritten");
  printf("arm          : push and sub move, a literal load is refused  OK\n");

  // The buffer is never overrun, however little room it is given.
  memset(code, 0, sizeof(code));
  for (int i = 0; i < 8; i++)
    put16(code + i * 2, 0x4604);
  for (int room = 0; room < 24; room++) {
    uint8_t small[32];
    memset(small, 0xAB, sizeof(small));
    prologue_relocate((uintptr_t)code, code, 1, NEED, small, room, &len);
    for (int i = room; i < (int)sizeof(small); i++)
      assert(small[i] == 0xAB && "wrote past the end of the output buffer");
  }
  printf("bounds       : nothing written past the buffer end       OK\n");

  // And against the real thing, if it is to hand.
  const char *so = getenv("BULLY_SO");
  if (so) {
    FILE *f = fopen(so, "rb");
    assert(f && "BULLY_SO does not open");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *D = malloc(n);
    assert(fread(D, 1, n, f) == (size_t)n);
    fclose(f);
    uint32_t shoff = *(uint32_t *)(D + 0x20);
    uint16_t shent = *(uint16_t *)(D + 0x2E), shnum = *(uint16_t *)(D + 0x30);
    uint32_t dsym = 0, dsz = 0, dstr = 0, taddr = 0, toff = 0, tsz = 0;
    for (int i = 0; i < shnum; i++) {
      uint8_t *sh = D + shoff + i * shent;
      uint32_t type = *(uint32_t *)(sh + 4), addr = *(uint32_t *)(sh + 12);
      uint32_t off = *(uint32_t *)(sh + 16), size = *(uint32_t *)(sh + 20);
      uint32_t link = *(uint32_t *)(sh + 24), flags = *(uint32_t *)(sh + 8);
      if (type == 11) {
        dsym = off;
        dsz = size;
        dstr = *(uint32_t *)(D + shoff + link * shent + 16);
      }
      if (type == 1 && (flags & 4) && size > tsz) { // the biggest executable section
        taddr = addr;
        toff = off;
        tsz = size;
      }
    }
    int total = 0, movable = 0, refused = 0;
    for (uint32_t o = 0; o < dsz; o += 16) {
      uint8_t *s = D + dsym + o;
      uint32_t value = *(uint32_t *)(s + 4);
      if ((s[12] & 0xF) != 2 || !value)
        continue;
      uint32_t addr = value & ~1u;
      if (addr < taddr || addr + 16 > taddr + tsz)
        continue;
      total++;
      const uint8_t *code_at = D + toff + (addr - taddr);
      int need = 8 + ((addr & 2) ? 2 : 0);
      // Relocate against the file image: a literal load reads the word out of
      // the file rather than out of a live module, which is the wrong value but
      // exactly the right number of bytes and the right shape.
      uint8_t buf[64];
      int wrote = 0;
      if (prologue_relocate((uintptr_t)code_at, code_at, value & 1, need, buf, sizeof(buf), &wrote))
        movable++;
      else
        refused++;
    }
    printf("real binary  : %d of %d functions relocatable, %d refused\n", movable, total, refused);
    assert(total > 10000 && "did not find the symbol table");
    assert(movable * 2 > total && "most of a real binary should be wrappable");
    free(D);
  } else {
    printf("real binary  : skipped, BULLY_SO not set\n");
  }

  printf("\nall prologue assertions held\n");
  return 0;
}
