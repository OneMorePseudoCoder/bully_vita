/* prologue.h -- see prologue.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __PROLOGUE_H__
#define __PROLOGUE_H__

#include <stdint.h>

// Copies whole instructions from `src` (which really lives at `addr`) until at
// least `need` bytes of the original are covered, rewriting a literal load into
// the value it would have loaded, and appends a jump back to addr + consumed.
// Writes at most out_size bytes and sets *out_len to how many it wrote.
// Returns the number of source bytes consumed, or 0 if it refused.
int prologue_relocate(uintptr_t addr, const uint8_t *src, int thumb, int need, uint8_t *out,
                      int out_size, int *out_len);

// The same walk without moving anything or reading any literal: would every
// instruction covering `need` bytes move as it stands? Returns the byte count
// or 0. For surveying a binary that is not loaded.
int prologue_relocatable(const uint8_t *code, int thumb, int need);

#endif
