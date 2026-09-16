/* fps_cap.c -- the frame limiter the game runs itself
 *
 * Measured, three builds running, on settled gameplay:
 *
 *   27.8 fps   frame 35.97 ms = app tick 30.77 + clampfps 4.75  (+0.45 over)
 *   27.8 fps   frame 35.97 ms = app tick 29.44 + clampfps 6.21  (+0.33 over)
 *
 * The frame adds up to within half a millisecond, and between four and six of
 * those milliseconds are Application::ClampFPS deliberately not running. It
 * reads a target rate from the Application object at +0x1c and, while that is
 * above zero, loops on GetCPUTime and calls SleepThread until the frame has
 * taken long enough. The work in a frame is 29-31 ms; the frame is 36.
 *
 * So the game is capped, and it is capped below what it can actually do. Turn
 * the limiter off and the same work should present at about 33 fps rather than
 * 27.8.
 *
 * Off unless ux0:data/Bully has a no_fps_cap file in it. Uncapping a game
 * written to a frame budget is not obviously safe -- it runs hotter, it uses
 * more battery, and anything in the engine that is frame rate dependent rather
 * than delta driven will move faster. ClampFPS's other job, clamping the delta
 * it returns so a long frame cannot explode the physics, is left exactly as it
 * was: this writes zero to the target field and lets the game's own code take
 * its own uncapped path.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include <stdint.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "prologue.h"
#include "fps_cap.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

// Application::ClampFPS(double) reads its target from this offset as a float.
#define APPLICATION_TARGET_FPS 0x1c

int fps_cap_lifted;
static uintptr_t clampfps_trampoline;

// Called with the Application the game is about to clamp against. Every call,
// because it costs one store and the game is free to put the cap back.
void *fps_cap_apply(void *self) {
  if (self)
    *(volatile float *)((char *)self + APPLICATION_TARGET_FPS) = 0.0f;
  return (void *)clampfps_trampoline;
}

// ClampFPS takes three words -- this, and a double in r2:r3 -- so nothing
// arrives on the stack and the eight bytes this pushes are invisible to it.
__asm__(".syntax unified\n"
        ".thumb\n"
        ".thumb_func\n"
        ".global fps_cap_thunk\n"
        "fps_cap_thunk:\n"
        "  push  {r4, lr}\n"
        "  push  {r0-r3}\n"
        "  bl    fps_cap_apply\n"
        "  mov   r4, r0\n"
        "  pop   {r0-r3}\n"
        "  blx   r4\n"
        "  pop   {r4, pc}\n");

extern void fps_cap_thunk(void);

void fps_cap_init(void) {
  SceIoStat stat;
  if (sceIoGetstat(FPS_CAP_DISABLE_PATH, &stat) < 0)
    return;

  uintptr_t entry = so_symbol(&bully_mod, "_ZN11Application8ClampFPSEd");
  if (!entry) {
    traceLog("fps cap: Application::ClampFPS is not in this build\n");
    return;
  }
  int thumb = entry & 1;
  uintptr_t addr = entry & ~(uintptr_t)1;
  int need = 8 + ((addr & 2) ? 2 : 0);
  uint8_t copy[64];
  int written = 0;
  int taken = prologue_relocate(addr, (const uint8_t *)addr, thumb, need, copy, sizeof(copy),
                                &written);
  if (!taken) {
    traceLog("fps cap: ClampFPS starts with something that cannot be moved, left alone\n");
    return;
  }

  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  void *base = NULL;
  SceUID block = kuKernelAllocMemBlock("bully_fpscap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000,
                                       &opt);
  if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 || !base) {
    traceLog("fps cap: no executable memory for the trampoline\n");
    return;
  }
  kuKernelCpuUnrestrictedMemcpy(base, copy, written);
  kuKernelFlushCaches(base, 0x1000);
  clampfps_trampoline = (uintptr_t)base | (thumb ? 1u : 0u);

  hook_addr(entry, (uintptr_t)fps_cap_thunk);
  fps_cap_lifted = 1;
  traceLog("fps cap: the game's frame limiter is off -- it was sleeping 4-6 ms of every 36\n");
}
