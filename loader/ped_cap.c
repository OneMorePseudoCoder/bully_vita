/* ped_cap.c -- a ceiling on the ambient ped count
 *
 * The stutter is peds, and the measurement is unusually clean. Over sixty
 * settled heartbeats, with every other cost on core 1 named and accounted:
 *
 *   peds/frame   CWorld::Process   app tick   fps
 *        2.0          3.53 ms      14.31 ms   29.2
 *       15.0         14.58 ms      32.10 ms   27.5
 *       16.2         15.91 ms      44.59 ms   21.0
 *
 * That is about 0.9 ms of core 1 per ped, and nothing else in the frame moves
 * like it -- script, streaming, population, the hud and the whole render side
 * all stay under 2.6 ms whatever is happening. A busy street costs thirty
 * milliseconds more than an empty one and the frame has about six to spare.
 *
 * The game already has the control. RoomForAnotherAmbientPed reads a limit out
 * of the current area's population info at [[this+0x76f0]+4], compares it with
 * GetPedTypeTotal, and gates every ambient spawn path on the answer:
 * SpawnAmbientPed, AddToPopulation, AddToPOIGroups, CreatePedForBike,
 * RequestPedForVehicle and the vehicle generator. So a second, lower ceiling
 * put in front of it needs no new state and can only ever refuse -- there is
 * no path by which this creates a ped, takes a lock, or touches anything the
 * other cores can see. The worst it can do is empty the school.
 *
 * It is off unless ux0:data/Bully/ped_cap exists with a number in it, and it
 * should stay off. Bully is a game about a school full of people; a version of
 * it with an empty school has given up the thing it was porting. This is here
 * to price the ped loop -- to answer "what would moving this to another core
 * actually be worth" in one run, without writing the hard version first -- and
 * not as an answer in itself. The answer is to move the work, not delete it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "prologue.h"
#include "ped_cap.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

static uintptr_t room_trampoline;
static int (*ped_type_total)(void *self);
static int ped_cap_limit;
static uint32_t ped_cap_blocks;
static uint32_t ped_cap_last_blocks;
static int ped_cap_seen_worst;

// Called with the CPopulation the game is asking. Returning NULL means "no
// room", which is a thing the original returns all the time -- every caller
// already handles it, because the area's own limit produces it.
void *ped_cap_gate(void *self) {
  if (ped_cap_limit > 0 && self && ped_type_total) {
    int now = ped_type_total(self);
    if (now > ped_cap_seen_worst)
      ped_cap_seen_worst = now;
    if (now >= ped_cap_limit) {
      ped_cap_blocks++;
      return NULL;
    }
  }
  return (void *)room_trampoline;
}

// RoomForAnotherAmbientPed takes one word, this, so the eight bytes pushed here
// are invisible to it. The zero returned on the blocked path is the same false
// the original returns when the area is already full.
__asm__(".syntax unified\n"
        ".thumb\n"
        ".thumb_func\n"
        ".global ped_cap_thunk\n"
        "ped_cap_thunk:\n"
        "  push  {r4, lr}\n"
        "  push  {r0-r3}\n"
        "  bl    ped_cap_gate\n"
        "  mov   r4, r0\n"
        "  pop   {r0-r3}\n"
        "  cmp   r4, #0\n"
        "  beq   1f\n"
        "  blx   r4\n"
        "  pop   {r4, pc}\n"
        "1:\n"
        "  movs  r0, #0\n"
        "  pop   {r4, pc}\n");

extern void ped_cap_thunk(void);

// A number on the first line of ux0:data/Bully/ped_cap, if there is one.
static int read_configured_cap(void) {
  SceUID fd = sceIoOpen(PED_CAP_PATH, SCE_O_RDONLY, 0);
  if (fd < 0)
    return PED_CAP_DEFAULT;
  char buf[16];
  int got = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (got <= 0)
    return PED_CAP_DEFAULT;
  buf[got] = 0;
  int value = atoi(buf);
  // A cap of zero would empty the world and a negative one is a typo. Anything
  // at or above the game's own limits is the same as not being here at all,
  // which is a legitimate way to ask for the original behaviour.
  if (value <= 0)
    return PED_CAP_DEFAULT;
  return value;
}

void ped_cap_init(void) {
  // Opt in, not opt out. A cap trades the school being full for frame rate,
  // and a port that quietly empties the school has broken the thing it was
  // meant to run. It is here to answer "what would it be worth", not to ship.
  SceIoStat stat;
  if (sceIoGetstat(PED_CAP_PATH, &stat) < 0)
    return;

  uintptr_t entry = so_symbol(&bully_mod, "_ZN11CPopulation24RoomForAnotherAmbientPedEv");
  uintptr_t total = so_symbol(&bully_mod, "_ZN11CPopulation15GetPedTypeTotalEv");
  if (!entry || !total) {
    traceLog("ped cap: CPopulation is not shaped the way this expects, left alone\n");
    return;
  }
  ped_type_total = (int (*)(void *))total;

  int thumb = entry & 1;
  uintptr_t addr = entry & ~(uintptr_t)1;
  int need = 8 + ((addr & 2) ? 2 : 0);
  uint8_t copy[64];
  int written = 0;
  if (!prologue_relocate(addr, (const uint8_t *)addr, thumb, need, copy, sizeof(copy), &written)) {
    traceLog("ped cap: RoomForAnotherAmbientPed starts with something that cannot be moved\n");
    return;
  }

  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  void *base = NULL;
  SceUID block = kuKernelAllocMemBlock("bully_pedcap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000,
                                       &opt);
  if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 || !base) {
    traceLog("ped cap: no executable memory for the trampoline\n");
    return;
  }
  kuKernelCpuUnrestrictedMemcpy(base, copy, written);
  kuKernelFlushCaches(base, 0x1000);
  room_trampoline = (uintptr_t)base | (thumb ? 1u : 0u);

  ped_cap_limit = read_configured_cap();
  hook_addr(entry, (uintptr_t)ped_cap_thunk);
  traceLog("ped cap: ambient peds held at %d -- put a number in " PED_CAP_PATH
           ", or an empty no_pedcap file to turn this off\n", ped_cap_limit);
}

void ped_cap_report(void) {
  if (!ped_cap_limit)
    return;
  traceLog("ped cap: %d, %u spawns refused this interval (%u all told), most peds seen %d\n",
           ped_cap_limit, (unsigned)(ped_cap_blocks - ped_cap_last_blocks),
           (unsigned)ped_cap_blocks, ped_cap_seen_worst);
  ped_cap_last_blocks = ped_cap_blocks;
}
