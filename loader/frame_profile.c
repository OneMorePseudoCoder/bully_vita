/* frame_profile.c -- where a frame of the game's own code goes
 *
 * The port can place the game's four threads on cores and it can keep the
 * loader's own work off them, but it cannot split the game's work up: the
 * engine has no job system, its physics is btSequentialImpulseConstraintSolver
 * and its btDiscreteDynamicsWorld::setNumTasks is a twelve byte stub. Before
 * anyone spends a month finding that out the expensive way, this answers the
 * cheap question first -- is there anything in a frame big enough to be worth
 * splitting, and what is it.
 *
 * So: hook the engine's frame level Update/Process methods, time them, and say
 * what each one cost over the last heartbeat. Off unless ux0:data/Bully has a
 * frame_profile file in it, because this is instrumentation and the shipping
 * build has no business carrying its risk.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "frame_profile.h"
#include "prologue.h"

// kubridge takes kernel memblock types and vitasdk keeps this one to itself.
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

/*
 * What gets hooked
 *
 * Frame level systems only -- the ones called a handful of times a frame, where
 * two clock reads are noise. Per entity methods are deliberately absent: there
 * are 1229 Update/Process/Tick/Step symbols in this binary and timing the ones
 * called two hundred times a frame would measure this file rather than the game.
 *
 * Every entry must take four words of arguments or fewer. The thunk pushes two
 * registers before calling the original, so anything reading an argument off
 * the stack would read it from the wrong place. All of these take none, a
 * float, or a bool.
 */
typedef struct {
  const char *symbol;
  const char *shown; // what to call it in the trace
} ProfileTarget;

static const ProfileTarget targets[] = {
    {"_ZN5CGame7ProcessEv", "CGame"},
    {"_ZN6CWorld7ProcessEv", "CWorld"},
    {"_ZN5World6UpdateEf", "World"},
    {"_ZN10CStreaming6UpdateEv", "streaming"},
    {"_ZN13ScriptManager6UpdateEb", "script"},
    {"_ZN9LuaScript6UpdateEb", "lua"},
    {"_ZN10RendererES6UpdateEf", "rendererES"},
    {"_ZN14WorldSceneView6UpdateEf", "sceneview"},
    {"_ZN14WorldSceneView6RenderEv", "scene draw"},
    {"_ZN3hal5Audio6UpdateEv", "audio"},
    {"_ZN13AnimationTree6UpdateEf", "anim tree"},
    {"_ZN11CPedManager6UpdateEv", "peds"},
    {"_ZN13CSpawnManager6UpdateEv", "spawn"},
    {"_ZN13EffectManager6UpdateEv", "effects"},
    {"_ZN13CameraManager6UpdateEv", "camera"},
    {"_ZN12CFireManager6UpdateEv", "fire"},
    {"_ZN10POIManager6UpdateEv", "poi"},
    {"_ZN12CoverManager6UpdateEv", "cover"},
    {"_ZN21AreaTransitionManager6UpdateEv", "area"},
    {"_ZN19ScriptEffectManager6UpdateEv", "script fx"},
    {"_ZN16CClothingManager6UpdateEv", "clothing"},
    {"_ZN14CPatrolManager6UpdateEv", "patrol"},
    {"_ZN10CPedSocial6UpdateEv", "social"},
    {"_ZN18EffectLightManager6UpdateEv", "fx lights"},
};

#define PROFILE_SLOTS ((int)(sizeof(targets) / sizeof(targets[0])))
#define TRAMPOLINE_BYTES 64

typedef struct {
  uintptr_t trampoline; // relocated prologue, then a jump back. Thumb bit set.
  uint32_t calls;
  uint32_t total_us;
  uint32_t worst_us;
  uint32_t entered_us;
  int depth;
  uint32_t last_calls, last_total_us; // what the previous heartbeat had seen
  uint8_t live;
} ProfileSlot;

static ProfileSlot slots[PROFILE_SLOTS];
static int profiling;

/*
 * The thunks
 *
 * One per slot, because the hook target is reached by a jump and has no other
 * way of knowing which function it is standing in for. Each saves the incoming
 * arguments, charges the clock, runs the relocated prologue (which jumps back
 * into the original), charges it again and returns what the original returned.
 *
 * Floats are in core registers: the loader and the Android library it loads are
 * both built softfp, so there is nothing in d0-d7 to preserve.
 */
void *frame_profile_enter(int slot);
void frame_profile_exit(int slot);

__asm__(".syntax unified\n"
        ".thumb\n"
        ".macro FPTHUNK idx\n"
        ".thumb_func\n"
        ".global frame_profile_thunk_\\idx\n"
        "frame_profile_thunk_\\idx:\n"
        "  push  {r4, lr}\n"
        "  push  {r0-r3}\n"
        "  movs  r0, #\\idx\n"
        "  bl    frame_profile_enter\n"
        "  mov   r4, r0\n"
        "  pop   {r0-r3}\n"
        "  blx   r4\n"
        "  push  {r0, r1}\n"
        "  movs  r0, #\\idx\n"
        "  bl    frame_profile_exit\n"
        "  pop   {r0, r1}\n"
        "  pop   {r4, pc}\n"
        ".endm\n"
        "FPTHUNK 0\n  FPTHUNK 1\n  FPTHUNK 2\n  FPTHUNK 3\n"
        "FPTHUNK 4\n  FPTHUNK 5\n  FPTHUNK 6\n  FPTHUNK 7\n"
        "FPTHUNK 8\n  FPTHUNK 9\n  FPTHUNK 10\n FPTHUNK 11\n"
        "FPTHUNK 12\n FPTHUNK 13\n FPTHUNK 14\n FPTHUNK 15\n"
        "FPTHUNK 16\n FPTHUNK 17\n FPTHUNK 18\n FPTHUNK 19\n"
        "FPTHUNK 20\n FPTHUNK 21\n FPTHUNK 22\n FPTHUNK 23\n");

extern void frame_profile_thunk_0(void), frame_profile_thunk_1(void);
extern void frame_profile_thunk_2(void), frame_profile_thunk_3(void);
extern void frame_profile_thunk_4(void), frame_profile_thunk_5(void);
extern void frame_profile_thunk_6(void), frame_profile_thunk_7(void);
extern void frame_profile_thunk_8(void), frame_profile_thunk_9(void);
extern void frame_profile_thunk_10(void), frame_profile_thunk_11(void);
extern void frame_profile_thunk_12(void), frame_profile_thunk_13(void);
extern void frame_profile_thunk_14(void), frame_profile_thunk_15(void);
extern void frame_profile_thunk_16(void), frame_profile_thunk_17(void);
extern void frame_profile_thunk_18(void), frame_profile_thunk_19(void);
extern void frame_profile_thunk_20(void), frame_profile_thunk_21(void);
extern void frame_profile_thunk_22(void), frame_profile_thunk_23(void);

static void (*const thunks[])(void) = {
    frame_profile_thunk_0,  frame_profile_thunk_1,  frame_profile_thunk_2,  frame_profile_thunk_3,
    frame_profile_thunk_4,  frame_profile_thunk_5,  frame_profile_thunk_6,  frame_profile_thunk_7,
    frame_profile_thunk_8,  frame_profile_thunk_9,  frame_profile_thunk_10, frame_profile_thunk_11,
    frame_profile_thunk_12, frame_profile_thunk_13, frame_profile_thunk_14, frame_profile_thunk_15,
    frame_profile_thunk_16, frame_profile_thunk_17, frame_profile_thunk_18, frame_profile_thunk_19,
    frame_profile_thunk_20, frame_profile_thunk_21, frame_profile_thunk_22, frame_profile_thunk_23,
};

static uint32_t profile_now_us(void) {
  SceKernelSysClock now;
  sceKernelGetProcessTime(&now);
  return (uint32_t)now;
}

void *frame_profile_enter(int slot) {
  ProfileSlot *s = &slots[slot];
  // Only the outermost entry is timed. These call each other -- CGame::Process
  // reaches most of the rest -- and charging an inner call to the outer one as
  // well would make the column add up to several times the frame.
  if (__atomic_fetch_add(&s->depth, 1, __ATOMIC_SEQ_CST) == 0)
    s->entered_us = profile_now_us();
  return (void *)s->trampoline;
}

void frame_profile_exit(int slot) {
  ProfileSlot *s = &slots[slot];
  if (__atomic_sub_fetch(&s->depth, 1, __ATOMIC_SEQ_CST) != 0)
    return;
  uint32_t spent = profile_now_us() - s->entered_us;
  s->calls++;
  s->total_us += spent;
  if (spent > s->worst_us)
    s->worst_us = spent;
}

void frame_profile_init(void) {
  SceIoStat stat;
  if (sceIoGetstat(FRAME_PROFILE_PATH, &stat) < 0)
    return;

  SceUID block = kuKernelAllocMemBlock("bully_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX,
                                       ALIGN_MEM(PROFILE_SLOTS * TRAMPOLINE_BYTES, 0x1000), NULL);
  void *base = NULL;
  if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 || !base) {
    traceLog("frame profile: no executable memory for the trampolines\n");
    return;
  }

  int hooked = 0;
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    uintptr_t entry = so_symbol(&bully_mod, targets[i].symbol);
    if (!entry) {
      traceLog("frame profile: %s is not in this build\n", targets[i].shown);
      continue;
    }
    int thumb = entry & 1;
    uintptr_t addr = entry & ~(uintptr_t)1;
    // hook_addr writes eight bytes, preceded by a NOP when the entry is not on
    // a four byte boundary. Take back exactly what it will overwrite.
    int need = 8 + ((addr & 2) ? 2 : 0);
    uint8_t copy[TRAMPOLINE_BYTES];
    int written = 0;
    int taken = prologue_relocate(addr, (const uint8_t *)addr, thumb, need, copy, sizeof(copy),
                                  &written);
    if (!taken) {
      traceLog("frame profile: %s starts with something that cannot be moved, left alone\n",
               targets[i].shown);
      continue;
    }
    uint8_t *tramp = (uint8_t *)base + i * TRAMPOLINE_BYTES;
    kuKernelCpuUnrestrictedMemcpy(tramp, copy, written);
    slots[i].trampoline = (uintptr_t)tramp | (thumb ? 1u : 0u);
    slots[i].live = 1;
    hook_addr(entry, (uintptr_t)thunks[i]);
    hooked++;
  }
  kuKernelFlushCaches(base, PROFILE_SLOTS * TRAMPOLINE_BYTES);

  profiling = hooked > 0;
  traceLog("frame profile: %d of %d frame methods hooked, timing on\n", hooked, PROFILE_SLOTS);
}

void frame_profile_report(void) {
  if (!profiling)
    return;
  // Busiest first, as deltas over the interval, and only the ones that cost
  // something: a line of two dozen zeroes is not a measurement. The worst is a
  // high water mark and stays cumulative -- the question it answers is whether
  // any single call has ever been long enough to be a stall on its own.
  uint32_t delta[PROFILE_SLOTS];
  uint8_t shown[PROFILE_SLOTS];
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    delta[i] = slots[i].live ? slots[i].total_us - slots[i].last_total_us : 0;
    shown[i] = 0;
  }

  char line[512];
  int at = 0;
  for (int n = 0; n < 8; n++) {
    int best = -1;
    for (int i = 0; i < PROFILE_SLOTS; i++)
      if (!shown[i] && delta[i] && (best < 0 || delta[i] > delta[best]))
        best = i;
    if (best < 0)
      break;
    shown[best] = 1;
    ProfileSlot *s = &slots[best];
    int room = (int)sizeof(line) - at;
    int put = snprintf(line + at, room, "%s%s %d ms/%u (worst %u ms)", at ? " | " : "",
                       targets[best].shown, (int)(delta[best] / 1000), s->calls - s->last_calls,
                       s->worst_us / 1000);
    if (put < 0 || put >= room)
      break;
    at += put;
  }
  // Rolled forward whether or not it made the line, so a slot that is quiet for
  // a heartbeat does not report that quiet interval's work in the next one.
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    slots[i].last_total_us = slots[i].total_us;
    slots[i].last_calls = slots[i].calls;
  }
  if (at)
    traceLog("frame: %s\n", line);
}
