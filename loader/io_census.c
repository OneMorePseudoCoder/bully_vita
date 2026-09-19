/* io_census.c -- which files the game keeps opening
 *
 * Every log this port has produced says the same thing and it took a long
 * while to hear it. In a twenty second heartbeat of ordinary play, with no
 * area transition in it at all:
 *
 *   frame io: 2820 reads 41988 KB in 13735 ms | 2581 opens in 4155 ms
 *   frame io: 2924 reads 42810 KB in 15520 ms | 2352 opens in 3538 ms
 *
 * Thirteen of every twenty seconds inside fread, and another three and a half
 * inside fopen. Two and a half thousand opens, at about one and a half
 * milliseconds each, sustained, while the player is just walking around. The
 * biggest single stalls in the same session are an area load at 13.6 seconds
 * and one Lua call at 1.87, and both are full of the same reads.
 *
 * What none of it says is WHICH files. The counters are totals, so a thousand
 * opens of one archive and a thousand opens of a thousand different files look
 * identical -- and they want opposite fixes. This answers that, and nothing
 * else: no caching, no skipping, no reordering. It is a census.
 *
 * It matters because a handle cache has been tried twice here and taken out
 * twice, both times because a first open measured about three times more
 * expensive with the cache in front of it. That is a real result and it is not
 * being argued with. But nobody checked whether the opens are repeats, and if
 * two thousand of them are the same few archives reopened, then the thing to
 * fix is the reopening rather than the open.
 *
 * Paths are kept by their tail rather than whole: the interesting part of
 * ux0:data/Bully/BullyOrig/Scripts/whatever.lur is the end of it, and a fixed
 * tail keeps the table small enough to sit in the heartbeat.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "main.h"
#include "config.h"
#include "io_census.h"

#define CENSUS_SLOTS 192
#define CENSUS_TAIL 44
#define CENSUS_SHOWN 8

typedef struct {
  char tail[CENSUS_TAIL];
  uint32_t opens;
  uint32_t fails; // opens that returned NULL -- a path the game tries first and does not have
  uint32_t open_us;
  uint32_t reads;
  uint32_t read_us;
  uint32_t read_kb;
  uint32_t hash;
} CensusEntry;

static CensusEntry census[CENSUS_SLOTS];
static uint32_t census_overflow;   // opens that found no free slot
static SceKernelLwMutexWork census_lock;
static int census_ready;

// The file each thread opened last, so a read can be charged to it. Reads do
// not carry a path and threading the FILE* through would mean a second table;
// a thread opens a file and then reads it, so last-open is the right guess and
// a wrong guess only misattributes bytes, never miscounts them.
#define CENSUS_THREADS 8
static struct { SceUID thid; int slot; } last_open[CENSUS_THREADS];

void io_census_init(void) {
  // Leaving census_ready at zero is what makes every io_census_* call below a
  // single compare and return. A lock and a table update on every one of half
  // a million reads, for a report the log will never carry, is not worth even
  // a microsecond each.
  if (!log_is_enabled())
    return;
  if (sceKernelCreateLwMutex(&census_lock, "bully io census", 0x2000, 0, NULL) >= 0)
    census_ready = 1;
}

static uint32_t tail_hash(const char *tail) {
  uint32_t h = 0x811c9dc5;
  for (const char *p = tail; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 0x01000193;
  }
  return h;
}

// The last CENSUS_TAIL-1 characters, which is the part that identifies the file.
static void path_tail(const char *path, char *out) {
  size_t len = path ? strlen(path) : 0;
  const char *from = path ? path : "";
  if (len > CENSUS_TAIL - 1)
    from += len - (CENSUS_TAIL - 1);
  snprintf(out, CENSUS_TAIL, "%s", from);
}

static int find_slot(const char *tail, uint32_t hash) {
  int free_slot = -1;
  for (int i = 0; i < CENSUS_SLOTS; i++) {
    if (census[i].opens == 0 && census[i].hash == 0) {
      if (free_slot < 0)
        free_slot = i;
      continue;
    }
    if (census[i].hash == hash && strcmp(census[i].tail, tail) == 0)
      return i;
  }
  if (free_slot < 0)
    return -1;
  snprintf(census[free_slot].tail, CENSUS_TAIL, "%s", tail);
  census[free_slot].hash = hash ? hash : 1;
  return free_slot;
}

static void remember_open(int slot) {
  SceUID me = sceKernelGetThreadId();
  for (int i = 0; i < CENSUS_THREADS; i++) {
    if (last_open[i].thid == me || last_open[i].thid == 0) {
      last_open[i].thid = me;
      last_open[i].slot = slot;
      return;
    }
  }
}

static int recall_open(void) {
  SceUID me = sceKernelGetThreadId();
  for (int i = 0; i < CENSUS_THREADS; i++)
    if (last_open[i].thid == me)
      return last_open[i].slot;
  return -1;
}

void io_census_open(const char *path, unsigned us, int failed) {
  if (!census_ready)
    return;
  char tail[CENSUS_TAIL];
  path_tail(path, tail);
  uint32_t hash = tail_hash(tail);
  sceKernelLockLwMutex(&census_lock, 1, NULL);
  int slot = find_slot(tail, hash);
  if (slot < 0) {
    census_overflow++;
  } else {
    census[slot].opens++;
    census[slot].fails += failed ? 1 : 0;
    census[slot].open_us += us;
    remember_open(slot);
  }
  sceKernelUnlockLwMutex(&census_lock, 1);
}

void io_census_read(unsigned us, unsigned bytes) {
  if (!census_ready)
    return;
  sceKernelLockLwMutex(&census_lock, 1, NULL);
  int slot = recall_open();
  if (slot >= 0) {
    census[slot].reads++;
    census[slot].read_us += us;
    census[slot].read_kb += bytes / 1024;
  }
  sceKernelUnlockLwMutex(&census_lock, 1);
}

// Cumulative rather than per interval. The question is which files are reopened
// across the whole session, and a file opened four hundred times spread over
// twenty heartbeats would look unremarkable in each one of them.
void io_census_report(void) {
  if (!census_ready)
    return;
  sceKernelLockLwMutex(&census_lock, 1, NULL);
  CensusEntry copy[CENSUS_SLOTS];
  memcpy(copy, census, sizeof(copy));
  uint32_t overflow = census_overflow;
  sceKernelUnlockLwMutex(&census_lock, 1);

  uint32_t distinct = 0, total_opens = 0, total_open_us = 0;
  for (int i = 0; i < CENSUS_SLOTS; i++) {
    if (!copy[i].opens)
      continue;
    distinct++;
    total_opens += copy[i].opens;
    total_open_us += copy[i].open_us;
  }
  if (!distinct)
    return;

  traceLog("io census: %u distinct paths, %u opens, %u ms in them%s\n",
           (unsigned)distinct, (unsigned)total_opens, (unsigned)(total_open_us / 1000),
           overflow ? " (table full, some paths not counted)" : "");

  // Most reopened first, because that is the question: repeats or not.
  // Selection sort over the copy, taking the largest and clearing it. Eight
  // passes over a hundred and ninety-two slots, once every twenty seconds.
  for (int shown = 0; shown < CENSUS_SHOWN; shown++) {
    int best = -1;
    for (int i = 0; i < CENSUS_SLOTS; i++) {
      if (!copy[i].opens)
        continue;
      if (best < 0 || copy[i].opens > copy[best].opens)
        best = i;
    }
    if (best < 0)
      break;
    traceLog("io census:   %5u opens (%4u failed) %5u ms | %4u reads %5u ms %6u KB | %s\n",
             (unsigned)copy[best].opens, (unsigned)copy[best].fails,
             (unsigned)(copy[best].open_us / 1000),
             (unsigned)copy[best].reads, (unsigned)(copy[best].read_us / 1000),
             (unsigned)copy[best].read_kb, copy[best].tail);
    copy[best].opens = 0;
  }
}
