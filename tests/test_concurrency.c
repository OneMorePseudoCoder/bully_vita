/* test_concurrency.c -- the cache and the game in vitaGL at the same time.
 *
 * A core dump taken on hardware (CLEAN21) has the game's CDStreamThread inside
 * glCompressedTexImage2D, through this loader's hook, holding a pthread mutex
 * that the game's RenderThread was parked on -- and the loader's own
 * texture_cache_tick running evict_textures on the main thread, which held no
 * mutex at all. vitaGL keeps one global binding table and one allocator per
 * pool, so the eviction's glBindTexture and glTexImage2D landed in the middle of
 * the game's bind-then-upload pair. What that leaves is a freed block with
 * texture pixels written into it, and a freed block is where the allocator keeps
 * its own list pointers: the next allocation walked its tree into pixel data and
 * dereferenced it. That was the crash.
 *
 * The allocator itself was ruled out from the same dump -- vitaGL's CDRAM mspace
 * has a kernel mutex of its own and the uploading thread was holding it. What is
 * unguarded is which texture is bound and when its storage goes away.
 *
 * So: run the game's upload traffic on one thread and the cache's tick on
 * another, and require that no two threads are ever inside the driver at once.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#include <pthread.h>

#define TEX_BYTES (512 * 1024)
// Enough uploads to put the cache well over its byte budget, so that every tick
// on the main thread has real eviction work to do rather than returning early.
#define OVER_BUDGET_MB ((size_t)TEXTURE_BUDGET_MB + TEXTURE_BUDGET_MB / 2)

// The game's own buffer. Sharing harness's source_bytes between two threads
// would be a race in the test rather than in the thing under test.
static unsigned char game_source[TEX_BYTES];

static GLuint game_ids[64];
static int game_id_count;
static volatile int game_running;
static unsigned game_uploads;

// CDStreamThread, as the dump found it: bind a texture, upload into it, repeat.
static void *game_thread(void *arg) {
  (void)arg;
  while (game_running) {
    for (int i = 0; i < game_id_count && game_running; i++) {
      GLuint id = game_ids[i];
      glBindTextureHook(GL_TEXTURE_2D, id);
      glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 512, 512, 0,
                                 TEX_BYTES, game_source);
      game_uploads++;
    }
  }
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  harness_start_empty(768 * MB);

  for (size_t i = 0; i < sizeof(game_source); i++)
    game_source[i] = (unsigned char)(i * 7);

  // A world's worth of textures, then time enough that they are all idle and
  // the tick will want them gone.
  int uploaded = 0;
  while (tracked_bytes < OVER_BUDGET_MB * MB) {
    GLuint id = tex_upload(0x9000u + uploaded, 512, 512, TEX_BYTES);
    if (game_id_count < (int)(sizeof(game_ids) / sizeof(game_ids[0])))
      game_ids[game_id_count++] = id;
    uploaded++;
  }
  frames(TEXTURE_IDLE_FRAMES + 2);
  printf("staged       : %d textures, %zu MB tracked, %u evicted so far\n", uploaded,
         tracked_bytes / MB, evicted_count);

  // Widen every driver entry point into a window wide enough that an overlap
  // cannot be missed by luck. Without the lock this test fails in the first
  // handful of iterations; it has been run that way to check that it can.
  fake_gl_delay_us = 20;
  fake_gl_overlaps = 0;

  game_running = 1;
  pthread_t game;
  assert(pthread_create(&game, NULL, game_thread, NULL) == 0);

  unsigned evicted_before = evicted_count;
  for (int f = 0; f < 400; f++)
    tick();
  game_running = 0;
  pthread_join(game, NULL);
  fake_gl_delay_us = 0;

  printf("ran          : %u game uploads against %u evictions\n", game_uploads,
         evicted_count - evicted_before);
  // The test is only worth anything if both sides actually did work while the
  // other one was going. A run that evicted nothing proves nothing.
  assert(game_uploads > 0 && "the game thread must have uploaded something");
  assert(evicted_count > evicted_before && "the tick must have evicted something");
  assert(fake_gl_overlaps == 0 && "two threads were inside vitaGL at once");
  printf("exclusive    : no two threads inside the driver at once  OK\n");

  printf("\nall concurrency assertions held\n");
  return 0;
}
