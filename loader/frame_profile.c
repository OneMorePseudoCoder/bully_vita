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
  // Charge the game's file reads made underneath this function to it. Only for
  // ones called rarely: the attribution costs a sceKernelGetThreadId on entry,
  // which is nothing a few hundred times a load and real thousands of times a
  // second.
  uint8_t watch_io;
} ProfileTarget;

static const ProfileTarget targets[] = {
    // The frame, top down. CWorld::Process is called only from CGame::Process,
    // so these nest and their times are inclusive.
    // The loop above CGame::Process, which nothing had ever measured.
    //
    // GameMain is 74% busy on a 35.4 ms frame -- 26.2 ms of work -- and
    // CGame::Process is only 15.0 ms of it. The other 11.2 ms is up here, and
    // one of these is a frame limiter: Application::ClampFPS reads a target rate
    // off the Application object and, when it is set, burns or sleeps until the
    // frame has taken long enough.
    //
    // Which decides whether any of the rest is worth doing. If ClampFPS is
    // sleeping nine milliseconds a frame then the game is capped rather than
    // limited, GameMain is not the bottleneck, and taking 4.17 ms off the ped
    // loop buys exactly nothing -- it would sleep 4.17 ms longer instead. If
    // ClampFPS returns immediately then the frame really is full and the ped
    // arithmetic stands. Seven hooks settle a question worth months.
    {"_Z10MainThreadPv", "MainThread"},
    {"_ZN11Application4TickEf", "app tick"},
    {"_ZN11Application8ClampFPSEd", "clampfps"},
    {"_ZN11Application17PerformFullUpdateEf", "full update"},
    {"_ZN9UserInput6UpdateEf", "userinput"},
    {"_ZN8UISystem8RenderUIEv", "render ui"},
    {"_Z15LIB_InputUpdatei", "lib input"},

    {"_ZN5CGame7ProcessEv", "CGame"},
    {"_ZN6CWorld7ProcessEv", "CWorld"},
    {"_ZN14WorldSceneView6RenderEv", "scene draw"},
    {"_ZN14WorldSceneView6UpdateEf", "sceneview"},
    {"_ZN10CStreaming6UpdateEv", "streaming"},
    {"_ZN13ScriptManager6UpdateEb", "script"},
    {"_ZN9LuaScript6UpdateEb", "lua"},
    {"_ZN13CameraManager6UpdateEv", "camera"},
    {"_ZN11CPopulation6UpdateEb", "population"},
    {"_ZN11CMissionMgr6UpdateEv", "mission"},
    {"_ZN9CTxdStore14GarbageCollectEv", "txd gc"},

    // The per entity loop, which is where CWorld::Process's time turned out to
    // be: CPed::ProcessControl was 6.73 ms of a 35.4 ms frame, 51% of
    // CWorld::Process, across 15.6 peds at 432 us each. The vehicle entries have
    // never been called -- there were no cars where the last session was played,
    // and CAutomobile::ProcessControl is the largest function in this table at
    // 15388 bytes, so a session on a bike or in a car is still unmeasured.
    {"_ZN4CPed14ProcessControlEv", "ped ctrl"},
    {"_ZN4CPed9PreRenderEv", "ped pre"},
    {"_ZN10CPlayerPed14ProcessControlEv", "player ctrl"},
    {"_ZN9CPhysical14ProcessControlEv", "phys ctrl"},
    {"_ZN7CEntity19UpdateAnimPreRenderEv", "ent anim"},
    {"_ZN11CAutomobile14ProcessControlEv", "car ctrl"},
    {"_ZN11CAutomobile9PreRenderEv", "car pre"},
    {"_ZN5CBike14ProcessControlEv", "bike ctrl"},

    // The thirteen second freeze, and what it is made of.
    //
    // One call to AreaTransitionManager::LoadArea took 13132 ms -- 99.7% of the
    // 13169 ms the whole transition cost, and it happened five times in a 150
    // second session (13132, 3077, 6201, 1021, 270 ms). CGame::TidyUpMemory was
    // six milliseconds of it, so it is not memory being tidied.
    //
    // Under LoadArea is CStreaming::LoadScene, which loads every model and every
    // collision file the new area needs, synchronously, off the memory card --
    // CColStore::EnsureCollisionIsInMemory even wraps its work in CTimer::
    // Suspend and Resume, so the engine knows it is about to block. The whole
    // tree is here so the next run says which part of it the thirteen seconds
    // were: seeking for files, reading them, converting them, or waiting on
    // sound banks.
    {"_ZN21AreaTransitionManager6UpdateEv", "area", 1},
    {"_ZN21AreaTransitionManager32UpdateAreaTransitionStateMachineEv", "area sm", 1},
    {"_ZN21AreaTransitionManager40UpdateBlockingAreaTransitionStateMachineEv", "area block"},
    {"_ZN21AreaTransitionManager8LoadAreaERK7CVector", "area load", 1},
    {"_ZN21AreaTransitionManager14ClearAreaPropsERK15VisibleAreaEnum", "area props", 1},
    {"_ZN21AreaTransitionManager21HandlePropActionTreesEv", "area trees", 1},
    {"_ZN5CGame12TidyUpMemoryEbb", "tidy mem", 1},
    {"_ZN13ScriptManager15StopAreaScriptsEv", "script stop", 1},
    {"_ZN11CPedManager12ShutDownPedsEv", "peds shut", 1},
    {"_ZN11CPopulation32UpdatePopulationOnAreaTransitionEv", "pop area", 1},
    {"_ZN18cSCREAMBankManager14AreaTransitionEv", "sound area", 1},

    {"_ZN10CStreaming9LoadSceneERK7CVector", "load scene", 1},
    {"_ZN10CStreaming22LoadAllRequestedModelsEb", "load all", 1},
    {"_ZN10CStreaming15GetNextFileOnCdEib", "next file", 1},
    {"_ZN10CStreaming21ConvertBufferToObjectEPcib", "convert", 1},
    {"_ZN10CStreaming22AddModelsToRequestListERK7CVectorj", "add reqs", 1},
    {"_ZN10CStreaming20InstanceLoadedModelsERK7CVector", "instance", 1},
    {"_ZN10CStreaming24PostInstanceLoadedModelsERK7CVector", "post inst", 1},
    {"_ZN10CStreaming13FlushChannelsEv", "flush ch", 1},
    {"_ZN9CColStore13LoadCollisionERK9CVector2D", "col load", 1},
    {"_ZN9CColStore7LoadColEiPhi", "col file", 1},
    {"_ZN9CColStore25EnsureCollisionIsInMemoryERK9CVector2D", "col ensure", 1},

    // One level inside ConvertBufferToObject, which was 96.7% of a 13510 ms area
    // load on its own. It both reads and computes -- it opens a stream, loads a
    // texture dictionary, loads collision, converts a mesh -- so splitting it is
    // what says whether the freeze is the card or the CPU, and the io figures
    // above say the same thing a second way.
    {"_ZN10CStreaming18ConvertMeshToModelEP4MeshiP14CStreamingInfoP14CBaseModelInfo", "convert mesh", 1},
    {"_Z17MadNoRwStreamOpen12RwStreamType18RwStreamAccessTypePv", "stream open", 1},
    {"_ZN9CTxdStore7LoadTxdEiP13MadNoRwStream", "load txd", 1},
    {"_ZN11LipSyncData11LoadInitialEiPc", "lipsync", 1},
    {"_ZN10CModelInfo19SetupPropActionTreeEi", "prop tree", 1},
    {"_ZN10ActionNode14LoadFromMemoryEiPKhPS_", "action node", 1},
};



#define PROFILE_SLOTS ((int)(sizeof(targets) / sizeof(targets[0])))
// One thunk per slot, and the thunks are written out by hand because each has to
// know which slot it stands for. Adding a target past this needs another one.
#define PROFILE_THUNKS 64
_Static_assert(PROFILE_SLOTS <= PROFILE_THUNKS, "more targets than thunks to reach them with");
#define TRAMPOLINE_BYTES 64

typedef struct {
  uintptr_t trampoline; // relocated prologue, then a jump back. Thumb bit set.
  uint32_t calls;
  uint32_t total_us;
  uint32_t worst_us;
  uint32_t entered_us;
  int depth;
  uint32_t io_us, io_reads;   // of the above, spent inside the game's fread
  uint32_t open_us, opens;    // and inside its fopen, which is not the same cost
  uint32_t io_kb;
  uint32_t last_calls, last_total_us, last_io_us, last_io_reads;
  uint32_t last_open_us, last_opens, last_io_kb;
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
        "FPTHUNK 0\n FPTHUNK 1\n FPTHUNK 2\n FPTHUNK 3\n FPTHUNK 4\n FPTHUNK 5\n"
        "FPTHUNK 6\n FPTHUNK 7\n FPTHUNK 8\n FPTHUNK 9\n FPTHUNK 10\n FPTHUNK 11\n"
        "FPTHUNK 12\n FPTHUNK 13\n FPTHUNK 14\n FPTHUNK 15\n FPTHUNK 16\n FPTHUNK 17\n"
        "FPTHUNK 18\n FPTHUNK 19\n FPTHUNK 20\n FPTHUNK 21\n FPTHUNK 22\n FPTHUNK 23\n"
        "FPTHUNK 24\n FPTHUNK 25\n FPTHUNK 26\n FPTHUNK 27\n FPTHUNK 28\n FPTHUNK 29\n"
        "FPTHUNK 30\n FPTHUNK 31\n FPTHUNK 32\n FPTHUNK 33\n FPTHUNK 34\n FPTHUNK 35\n"
        "FPTHUNK 36\n FPTHUNK 37\n FPTHUNK 38\n FPTHUNK 39\n FPTHUNK 40\n FPTHUNK 41\n"
        "FPTHUNK 42\n FPTHUNK 43\n FPTHUNK 44\n FPTHUNK 45\n FPTHUNK 46\n FPTHUNK 47\n"
        "FPTHUNK 48\n FPTHUNK 49\n FPTHUNK 50\n FPTHUNK 51\n FPTHUNK 52\n FPTHUNK 53\n"
        "FPTHUNK 54\n FPTHUNK 55\n FPTHUNK 56\n FPTHUNK 57\n FPTHUNK 58\n FPTHUNK 59\n"
        "FPTHUNK 60\n FPTHUNK 61\n FPTHUNK 62\n FPTHUNK 63\n");

extern void frame_profile_thunk_0(void);
extern void frame_profile_thunk_1(void);
extern void frame_profile_thunk_2(void);
extern void frame_profile_thunk_3(void);
extern void frame_profile_thunk_4(void);
extern void frame_profile_thunk_5(void);
extern void frame_profile_thunk_6(void);
extern void frame_profile_thunk_7(void);
extern void frame_profile_thunk_8(void);
extern void frame_profile_thunk_9(void);
extern void frame_profile_thunk_10(void);
extern void frame_profile_thunk_11(void);
extern void frame_profile_thunk_12(void);
extern void frame_profile_thunk_13(void);
extern void frame_profile_thunk_14(void);
extern void frame_profile_thunk_15(void);
extern void frame_profile_thunk_16(void);
extern void frame_profile_thunk_17(void);
extern void frame_profile_thunk_18(void);
extern void frame_profile_thunk_19(void);
extern void frame_profile_thunk_20(void);
extern void frame_profile_thunk_21(void);
extern void frame_profile_thunk_22(void);
extern void frame_profile_thunk_23(void);
extern void frame_profile_thunk_24(void);
extern void frame_profile_thunk_25(void);
extern void frame_profile_thunk_26(void);
extern void frame_profile_thunk_27(void);
extern void frame_profile_thunk_28(void);
extern void frame_profile_thunk_29(void);
extern void frame_profile_thunk_30(void);
extern void frame_profile_thunk_31(void);
extern void frame_profile_thunk_32(void);
extern void frame_profile_thunk_33(void);
extern void frame_profile_thunk_34(void);
extern void frame_profile_thunk_35(void);
extern void frame_profile_thunk_36(void);
extern void frame_profile_thunk_37(void);
extern void frame_profile_thunk_38(void);
extern void frame_profile_thunk_39(void);
extern void frame_profile_thunk_40(void);
extern void frame_profile_thunk_41(void);
extern void frame_profile_thunk_42(void);
extern void frame_profile_thunk_43(void);
extern void frame_profile_thunk_44(void);
extern void frame_profile_thunk_45(void);
extern void frame_profile_thunk_46(void);
extern void frame_profile_thunk_47(void);
extern void frame_profile_thunk_48(void);
extern void frame_profile_thunk_49(void);
extern void frame_profile_thunk_50(void);
extern void frame_profile_thunk_51(void);
extern void frame_profile_thunk_52(void);
extern void frame_profile_thunk_53(void);
extern void frame_profile_thunk_54(void);
extern void frame_profile_thunk_55(void);
extern void frame_profile_thunk_56(void);
extern void frame_profile_thunk_57(void);
extern void frame_profile_thunk_58(void);
extern void frame_profile_thunk_59(void);
extern void frame_profile_thunk_60(void);
extern void frame_profile_thunk_61(void);
extern void frame_profile_thunk_62(void);
extern void frame_profile_thunk_63(void);

static void (*const thunks[])(void) = {
    frame_profile_thunk_0, frame_profile_thunk_1, frame_profile_thunk_2, frame_profile_thunk_3,
    frame_profile_thunk_4, frame_profile_thunk_5, frame_profile_thunk_6, frame_profile_thunk_7,
    frame_profile_thunk_8, frame_profile_thunk_9, frame_profile_thunk_10, frame_profile_thunk_11,
    frame_profile_thunk_12, frame_profile_thunk_13, frame_profile_thunk_14, frame_profile_thunk_15,
    frame_profile_thunk_16, frame_profile_thunk_17, frame_profile_thunk_18, frame_profile_thunk_19,
    frame_profile_thunk_20, frame_profile_thunk_21, frame_profile_thunk_22, frame_profile_thunk_23,
    frame_profile_thunk_24, frame_profile_thunk_25, frame_profile_thunk_26, frame_profile_thunk_27,
    frame_profile_thunk_28, frame_profile_thunk_29, frame_profile_thunk_30, frame_profile_thunk_31,
    frame_profile_thunk_32, frame_profile_thunk_33, frame_profile_thunk_34, frame_profile_thunk_35,
    frame_profile_thunk_36, frame_profile_thunk_37, frame_profile_thunk_38, frame_profile_thunk_39,
    frame_profile_thunk_40, frame_profile_thunk_41, frame_profile_thunk_42, frame_profile_thunk_43,
    frame_profile_thunk_44, frame_profile_thunk_45, frame_profile_thunk_46, frame_profile_thunk_47,
    frame_profile_thunk_48, frame_profile_thunk_49, frame_profile_thunk_50, frame_profile_thunk_51,
    frame_profile_thunk_52, frame_profile_thunk_53, frame_profile_thunk_54, frame_profile_thunk_55,
    frame_profile_thunk_56, frame_profile_thunk_57, frame_profile_thunk_58, frame_profile_thunk_59,
    frame_profile_thunk_60, frame_profile_thunk_61, frame_profile_thunk_62, frame_profile_thunk_63,
};

static uint32_t profile_now_us(void) {
  SceKernelSysClock now;
  sceKernelGetProcessTime(&now);
  return (uint32_t)now;
}

/*
 * What the card is doing while a frame method is slow
 *
 * The frame profiler says which function spent thirteen seconds. It cannot say
 * whether that function spent them waiting on the memory card or working, and
 * for CStreaming::LoadAllRequestedModels -- which reads every model an area
 * needs, synchronously -- that is the whole question. So count the game's own
 * reads and time them.
 *
 * On only when the profiler is, and a branch when it is not. The game's fread
 * is not a hot path in the ordinary sense: a whole session made 65000 of them.
 */
unsigned io_opens, io_seeks, io_reads, io_bytes_kb, io_us, open_us;
static unsigned io_bytes_part;

// Which hooked functions the reading thread is currently inside.
//
// The process wide totals cannot answer the question that matters. The last run
// had ConvertBufferToObject at 13059 ms of a 13510 ms area load and the io line
// at 14646 ms of reading in the same twenty seconds -- more than the load itself,
// because the game's CDStreamThread reads continuously in the background and
// the counter could not tell the two apart. So a read is charged to whichever
// watched functions are on the stack of the thread that made it, and to none if
// that is a different thread.
#define IO_STACK 8
static SceUID io_thread;
static int io_stack[IO_STACK];
static int io_depth;

static void io_enter(int slot) {
  if (!io_depth)
    io_thread = sceKernelGetThreadId();
  if (io_depth < IO_STACK)
    io_stack[io_depth] = slot;
  io_depth++;
}

static void io_leave(int slot) {
  if (io_depth > 0 && (io_depth > IO_STACK || io_stack[io_depth - 1] == slot))
    io_depth--;
}

// Charge one file operation to every watched function the calling thread is
// inside. kind 0 is a read, 1 is an open.
static void io_charge(unsigned spent, unsigned bytes, int kind) {
  if (!io_depth || sceKernelGetThreadId() != io_thread)
    return;
  int n = io_depth < IO_STACK ? io_depth : IO_STACK;
  for (int i = 0; i < n; i++) {
    ProfileSlot *s = &slots[io_stack[i]];
    if (kind) {
      s->open_us += spent;
      s->opens++;
    } else {
      s->io_us += spent;
      s->io_reads++;
      s->io_kb += bytes >> 10;
    }
  }
}

void frame_profile_io_open(unsigned started) {
  if (!profiling)
    return;
  unsigned spent = profile_now_us() - started;
  open_us += spent;
  io_opens++;
  io_charge(spent, 0, 1);
}

void frame_profile_io_seek(void) {
  if (profiling)
    io_seeks++;
}

unsigned frame_profile_io_begin(void) {
  return profiling ? profile_now_us() : 0;
}

void frame_profile_io_end(unsigned started, unsigned bytes) {
  if (!profiling)
    return;
  unsigned spent = profile_now_us() - started;
  io_us += spent;
  io_reads++;
  io_charge(spent, bytes, 0);
  io_bytes_part += bytes;
  io_bytes_kb += io_bytes_part >> 10;
  io_bytes_part &= 1023;
}


void *frame_profile_enter(int slot) {
  ProfileSlot *s = &slots[slot];
  // Only the outermost entry is timed. These call each other -- CGame::Process
  // reaches most of the rest -- and charging an inner call to the outer one as
  // well would make the column add up to several times the frame.
  if (__atomic_fetch_add(&s->depth, 1, __ATOMIC_SEQ_CST) == 0)
    s->entered_us = profile_now_us();
  if (targets[slot].watch_io)
    io_enter(slot);
  return (void *)s->trampoline;
}

void frame_profile_exit(int slot) {
  ProfileSlot *s = &slots[slot];
  if (targets[slot].watch_io)
    io_leave(slot);
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

  // Somewhere to put the relocated prologues that the CPU will execute.
  //
  // Two ways, because the first one failed on hardware with a NULL options
  // pointer and there is no way to try again quickly: so_util asks kubridge for
  // a kernel RX block and always hands it a zeroed option struct, so do that,
  // and if it still says no, take an ordinary user RW block -- which needs no
  // kubridge at all -- and have kubridge mark it executable afterwards. The
  // trace says which worked, so the next log settles it.
  const SceSize tramp_size = ALIGN_MEM(PROFILE_SLOTS * TRAMPOLINE_BYTES, 0x1000);
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  void *base = NULL;
  int writable = 0;

  SceUID block = kuKernelAllocMemBlock("bully_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, tramp_size,
                                       &opt);
  int got_base = block >= 0 ? sceKernelGetMemBlockBase(block, &base) : block;
  if (block < 0 || got_base < 0 || !base) {
    traceLog("frame profile: kubridge would not give an RX block (alloc %#x, base %#x), "
             "trying RW and a protect\n",
             (unsigned)block, (unsigned)got_base);
    base = NULL;
    block = sceKernelAllocMemBlock("bully_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, tramp_size,
                                   NULL);
    got_base = block >= 0 ? sceKernelGetMemBlockBase(block, &base) : block;
    if (block < 0 || got_base < 0 || !base) {
      traceLog("frame profile: no memory for the trampolines at all (alloc %#x, base %#x)\n",
               (unsigned)block, (unsigned)got_base);
      return;
    }
    // Write through the ordinary pointer while it is still writable, and turn
    // it executable once every trampoline is in place.
    writable = 1;
  }

  // Two passes, and the hooks go in the second one.
  //
  // hook_addr cannot be undone: it overwrites the first instruction and the
  // bytes it displaced only exist in the trampoline. So nothing is patched
  // until every trampoline is written and the memory holding them is known to
  // be executable. Patching first and failing to protect afterwards would leave
  // the game jumping into memory it is not allowed to run, which is a crash on
  // the first frame rather than a missing measurement.
  uintptr_t entries[PROFILE_SLOTS];
  int ready = 0;
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    entries[i] = 0;
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
    if (writable)
      memcpy(tramp, copy, written);
    else
      kuKernelCpuUnrestrictedMemcpy(tramp, copy, written);
    slots[i].trampoline = (uintptr_t)tramp | (thumb ? 1u : 0u);
    entries[i] = entry;
    ready++;
  }

  if (writable) {
    int prot = kuKernelMemProtect(base, tramp_size, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
    if (prot < 0) {
      // Nothing is patched yet, so this costs the measurement and nothing else.
      traceLog("frame profile: could not make the trampolines executable (%#x), not hooking\n",
               (unsigned)prot);
      sceKernelFreeMemBlock(block);
      return;
    }
  }
  kuKernelFlushCaches(base, tramp_size);

  int hooked = 0;
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    if (!entries[i])
      continue;
    slots[i].live = 1;
    hook_addr(entries[i], (uintptr_t)thunks[i]);
    hooked++;
  }
  (void)ready;

  profiling = hooked > 0;
  traceLog("frame profile: %d of %d frame methods hooked from %s memory, timing on\n", hooked,
           PROFILE_SLOTS, writable ? "protected RW" : "kubridge RX");
}

void frame_profile_report(void) {
  if (!profiling)
    return;

  // Every slot that ran, busiest first, over as many lines as it takes -- and
  // then the ones that did not.
  //
  // The first round printed the top eight and nothing else, so a target that was
  // never called looked exactly like one that was called and was cheap. That is
  // the difference between "this work is not worth parallelising" and "this
  // function does not run", and the whole point of the exercise is telling them
  // apart. So: no truncation, and a closing line that names what never ran.
  uint32_t ms[PROFILE_SLOTS], calls[PROFILE_SLOTS];
  uint8_t placed[PROFILE_SLOTS];
  uint32_t hooked_calls = 0;
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    ms[i] = slots[i].live ? (slots[i].total_us - slots[i].last_total_us) / 1000 : 0;
    calls[i] = slots[i].live ? slots[i].calls - slots[i].last_calls : 0;
    placed[i] = !slots[i].live;
    hooked_calls += calls[i];
  }

  char line[420];
  int at = 0;
  for (;;) {
    int best = -1;
    for (int i = 0; i < PROFILE_SLOTS; i++)
      if (!placed[i] && calls[i] && (best < 0 || ms[i] > ms[best]))
        best = i;
    if (best < 0)
      break;
    placed[best] = 1;
    int room = (int)sizeof(line) - at;
    unsigned iod = (slots[best].io_us - slots[best].last_io_us) / 1000;
    int put;
    if (targets[best].watch_io)
      put = snprintf(line + at, room,
                     "%s%s %u ms/%u (rd %u ms/%u %u KB, op %u ms/%u, worst %u ms)",
                     at ? " | " : "", targets[best].shown, ms[best], calls[best], iod,
                     slots[best].io_reads - slots[best].last_io_reads,
                     slots[best].io_kb - slots[best].last_io_kb,
                     (slots[best].open_us - slots[best].last_open_us) / 1000,
                     slots[best].opens - slots[best].last_opens, slots[best].worst_us / 1000);
    else
      put = snprintf(line + at, room, "%s%s %u ms/%u (worst %u ms)", at ? " | " : "",
                     targets[best].shown, ms[best], calls[best], slots[best].worst_us / 1000);
    if (put < 0)
      break;
    if (put >= room) { // did not fit: flush what we have and put it on the next line
      line[at] = 0;
      traceLog("frame: %s\n", line);
      at = 0;
      placed[best] = 0;
      continue;
    }
    at += put;
  }
  if (at)
    traceLog("frame: %s\n", line);

  // Hooked, and not called once all session. Cumulative, not this interval: a
  // slot that ran earlier and is quiet now is not the same as one that has never
  // run at all, and only the second is worth saying.
  at = 0;
  int silent = 0;
  for (int i = 0; i < PROFILE_SLOTS; i++) {
    if (!slots[i].live || slots[i].calls)
      continue;
    silent++;
    int room = (int)sizeof(line) - at;
    int put = snprintf(line + at, room, "%s%s", at ? " " : "", targets[i].shown);
    if (put < 0 || put >= room)
      break;
    at += put;
  }
  // The profiler's own load, so its cost is judged from the log rather than
  // assumed: two clock reads per call, and the count is right here.
  static unsigned last_opens, last_seeks, last_reads, last_kb, last_io_us;
  traceLog("frame idle: %d never called (%s) | %u hooked calls this interval\n", silent,
           at ? line : "none", hooked_calls);
  // The game's own reading, over the same interval, so a slow function can be
  // told from a slow card.
  static unsigned last_open_us;
  traceLog("frame io: %u reads %u KB in %u ms | %u opens in %u ms | %u seeks\n",
           io_reads - last_reads, io_bytes_kb - last_kb, (io_us - last_io_us) / 1000,
           io_opens - last_opens, (open_us - last_open_us) / 1000, io_seeks - last_seeks);
  last_open_us = open_us;
  last_opens = io_opens;
  last_seeks = io_seeks;
  last_reads = io_reads;
  last_kb = io_bytes_kb;
  last_io_us = io_us;

  for (int i = 0; i < PROFILE_SLOTS; i++) {
    slots[i].last_total_us = slots[i].total_us;
    slots[i].last_calls = slots[i].calls;
    slots[i].last_io_us = slots[i].io_us;
    slots[i].last_io_reads = slots[i].io_reads;
    slots[i].last_open_us = slots[i].open_us;
    slots[i].last_opens = slots[i].opens;
    slots[i].last_io_kb = slots[i].io_kb;
  }
}
