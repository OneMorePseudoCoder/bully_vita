/* test_regress.c -- the specific mistakes this cache has already made once.
 *
 * Each of these was a real bug found by reading vitaGL's source, and each one
 * silently corrupted something rather than failing loudly.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "harness.h"

#define TEX_BYTES (256 * 1024)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  harness_start(512 * MB);

  // vglMemFree cannot be asked for the total. VGL_MEM_ALL is the enum
  // terminator and the wrapper returns 0 for it, so a cache that asked would
  // believe it was permanently out of memory and evict everything it could.
  assert(vglMemFree(VGL_MEM_ALL) == 0 && "the trap this fake exists to preserve");
  size_t pools[VGL_POOLS];
  vitagl_free_per_pool(pools);
  assert(pools[0] + pools[1] + pools[2] > 0 && "asking per pool must see the real figure");
  printf("free memory  : read per pool, not via VGL_MEM_ALL     OK\n");

  // Cube map faces come through the same entry point but are six buffers behind
  // one name. Tracking one as a flat texture meant freeing all six and
  // restoring a single face as a 2D texture.
  GLuint cube;
  glGenTexturesHook(1, &cube);
  glBindTextureHook(GL_TEXTURE_2D, cube);
  fill_source(0xC0DE0001u, TEX_BYTES);
  glCompressedTexImage2DHook(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                             GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 128, 128, 0, 8192, source_bytes);
  assert(!textures[cube].tracked && "a cube map face must not be tracked as a 2D texture");
  assert(tracked_bytes == 0 && "a cube map face must not be accounted");
  // And it must still be a cube map after enough pressure to evict anything the
  // cache thinks it owns.
  for (int i = 0; i < 96; i++) { tex_upload(0x7000u + i, 512, 512, TEX_BYTES); drain(); }
  frames(TEXTURE_IDLE_FRAMES * 4);
  assert(fake_sampled(cube) == 0xCBCBCBCBu && "the cube map must survive eviction untouched");
  printf("cube maps    : not tracked, and survive eviction      OK\n");

  // vitaGL reports a rejected upload by returning having allocated nothing.
  // Counting those left phantom bytes in the budget and let the cache "evict" a
  // texture that was never there, which actually allocated a placeholder.
  size_t before = tracked_bytes;
  uint32_t rejected_before = upload_rejected;
  GLuint bad;
  glGenTexturesHook(1, &bad);
  glBindTextureHook(GL_TEXTURE_2D, bad);
  fake_reject_next_upload = 1;
  glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 130, 130, 0,
                             8192, source_bytes);
  assert(tracked_bytes == before && "a rejected upload must not be accounted");
  assert(!textures[bad].tracked && "a rejected upload must not be tracked");
  // And it has to be counted. A texture the driver would not allocate draws
  // black, exactly like an eviction that never came back, and for six builds
  // the two were indistinguishable in the log because this one was silent.
  assert(upload_rejected == rejected_before + 1 &&
         "an upload the driver refused was not counted");
  printf("bad uploads  : rejected, not accounted, and counted   OK\n");

  // The store used to be one file carved into extents, and re-uploading a
  // texture appended rather than reusing its space, so a long session -- the
  // exact case this feature exists for -- walked the file to its cap and
  // everything after that degraded to white. Uploading writes nothing at all
  // now, so no amount of re-streaming can grow the store.
  GLuint churn[64];
  for (int i = 0; i < 64; i++) {
    churn[i] = tex_upload(0xF00D0000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  uint32_t after_one_round = fake_store_files();
  for (int round = 0; round < 40; round++)
    for (int i = 0; i < 64; i++) {
      tex_reupload(churn[i], 0xF00D0000u + i, 512, 512, TEX_BYTES);
      drain();
    }
  printf("store growth : %u files after 1 round, %u after 41     OK\n", after_one_round,
         fake_store_files());
  assert(fake_store_files() == after_one_round &&
         "re-uploading the same contents must not add files");

  // Uploading a texture is the hot path -- an area transition is thousands of
  // them back to back -- and it must not touch the card. Writing at upload time
  // is what made loading an area take minutes.
  uint32_t writes_before = fake_store_writes();
  for (int i = 0; i < 64; i++) {
    tex_reupload(churn[i], 0xF00D0000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  assert(fake_store_writes() == writes_before && "uploading must never write to the card");
  printf("upload cost  : no card writes on the upload path       OK\n");

  // Evicting must not write either, while the heap still has room for the
  // copies. A card write costs far more than a frame, and doing every eviction
  // through the card turned reclaiming into a visible stall.
  {
    harness_start(512 * MB);
    const int count = (int)(((size_t)TEXTURE_RAM_CACHE_MB * MB / 2) / TEX_BYTES);
    GLuint cold[256];
    assert(count > 0 && count <= 256);
    for (int i = 0; i < count; i++) {
      cold[i] = tex_upload(0xEE000000u + i, 512, 512, TEX_BYTES);
      drain();
    }
    // Enough pressure to force them out, from textures that stay drawn.
    GLuint hot[64];
    for (int i = 0; i < 64; i++) {
      hot[i] = tex_upload(0xEF000000u + i, 512, 512, TEX_BYTES);
      drain();
    }
    while (tracked_bytes < (size_t)TEXTURE_BUDGET_MB * MB + 32 * MB) {
      tex_upload(0xF1000000u + (unsigned)tracked_bytes, 512, 512, TEX_BYTES);
      drain();
    }
    wander(hot, 64, TEXTURE_IDLE_FRAMES * 3);

    int evicted = 0;
    for (int i = 0; i < count; i++)
      if (textures[cold[i]].evicted)
        evicted++;
    printf("heap tier    : %d evicted, %u to heap, %u to card, %zu MB parked  OK\n", evicted,
           ram_evicted_count, card_evicted_count, ram_cache_bytes / MB);
    assert(evicted > count / 2 && "the cold set should have been evicted");
    assert(ram_evicted_count > 0 && "eviction must use the heap");
    // The card is the overflow, not the destination: nothing may be written
    // while the tier still had room for it.
    assert((card_evicted_count == 0 ||
            ram_cache_bytes + TEX_BYTES > (size_t)TEXTURE_RAM_CACHE_MB * MB) &&
           "the card must not be touched until the heap tier is full");

    // And they must come back from the heap just as they would off the card.
    wander(cold, count, 4);
    for (int i = 0; i < count; i++)
      assert(fake_sampled(cold[i]) == fake_fingerprint_of(0xEE000000u + i) &&
             "a texture parked in the heap must restore correctly");
    printf("heap restore : parked textures come back byte for byte OK\n");
  }

  // The store outlives the run, and a texture it already holds costs nothing at
  // all to evict. This is what the persistent store is for: evicting used to
  // mean writing a few hundred KB inside a frame, which is why writes are
  // rationed to emergencies, which is why a session with the card shut evicted
  // 337 textures and then watched the pools drain to nothing.
  {
    // A driver small enough that the RAM pool genuinely runs out -- smaller
    // than the byte budget, so the budget cannot cap the pressure first. The
    // card is only for that case now: past the heap tier, with memory to spare,
    // a texture stays resident rather than costing a write inside a frame.
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);
    GLuint ids[1536];
    assert(count <= (int)(sizeof(ids) / sizeof(ids[0])));

    harness_start_empty(192 * MB);
    assert(store_files == 0 && "this block starts from an empty store");

    for (int i = 0; i < count; i++) {
      tex_upload(0xDE000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned spilled = fake_store_files();
    assert(spilled > 0 && "a pool actually running out has to be able to reach the card");

    // A restart, and then the same assets again. Different texture names, same
    // textures: the store is keyed by what they are, not where they landed.
    fake_reset(192 * MB);
    texture_cache_init();
    assert(fake_store_files() == spilled && "the store must survive a restart");
    assert(store_files == spilled && "and be read back into the index");

    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0xDE000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    printf("store reuse  : %u spilled, %u evictions free on the next run, %u written  OK\n",
           spilled, store_reused, card_evicted_count);
    // Most of what the store holds, evicted for free. Not "at least as many as
    // the first run spilled": that counts how hard the cache was driven rather
    // than whether the store works, and a cache that recovers the pool sooner
    // legitimately evicts fewer textures the second time round. What matters is
    // below -- that nothing already in the store is paid for again.
    assert(store_reused * 10 >= (uint32_t)spilled * 8 &&
           "the store is not being used for the evictions that did happen");
    // Not zero, and it should not be. Freeing an eviction from its ration means
    // more of them succeed, so textures that only ever reached the heap tier on
    // the first run get as far as the card on the second and are written once,
    // for the first time. The store converges; what must not happen is paying
    // again for what it already holds.
    assert(card_evicted_count < spilled / 8 && "but next to nothing may be written again");

    // And a file this run never wrote still restores byte for byte. A sample of
    // them, with a tick between, because restoring six hundred textures into a
    // driver this size would run it out of memory on its own.
    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0xDE000000u + (unsigned)i) &&
             "a texture restored from a previous run's file must be the right one");
      checked++;
      tick();
    }
    assert(checked > 0 && "something had to have been evicted to check this");
    printf("             : %d restored from files written before the restart  OK\n", checked);
  }

  // ...and the key has to actually tell textures apart, because the failure it
  // guards against is silent. A texture whose contents differ must not be
  // handed a stored file just because it is the same size and shape.
  {
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);

    harness_start_empty(192 * MB);
    for (int i = 0; i < count; i++) {
      tex_upload(0xAB000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned first = fake_store_files();
    assert(first > 0);

    // Same dimensions, same format, same sizes -- different pixels.
    fake_reset(192 * MB);
    texture_cache_init();
    GLuint ids[1536];
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0xCD000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    printf("key discrim  : %u stored, %u reused, %u written for different pixels  OK\n", first,
           store_reused, card_evicted_count);
    assert(store_reused == 0 && "different pixels must never match a stored file");
    assert(card_evicted_count > 0 && "they have to be written for themselves");

    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0xCD000000u + (unsigned)i) &&
             "and each must restore as itself, not as the texture it displaced");
      checked++;
      tick();
    }
    assert(checked > 0 && "something had to have been evicted to check this");
  }

  // Reclaiming is bursty, not a trickle. A pool sitting a little under its ideal
  // is left alone, because chasing it evicts textures the game asks straight
  // back for -- and every one of those is a read off the memory card on the
  // drawing thread. A session ran with the RAM pool at 24-25 MB free against a
  // 24 MB threshold and paid 2434 evictions and 1022 restores for it.
  {
    harness_start_empty(0);
    fake_set_pools(81 * MB, 100 * MB, 26 * MB);
    tick(); // takes the starting figures
    assert(pool_start[1] == 100 * MB);

    // Enough resident to have something to evict, and idle enough to qualify.
    GLuint cold[128];
    for (int i = 0; i < 128; i++) {
      cold[i] = tex_upload(0xBB000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
    }
    frames(TEXTURE_IDLE_FRAMES * 2);

    // Derived from the marks rather than written out, so that moving them does
    // not silently turn this into a test of something else -- which is what
    // happened the first time they moved.
    const size_t low_mb = pool_start[1] / 100 * TEXTURE_FREE_HEADROOM_LOW_PERCENT / MB;
    const size_t high_mb = pool_start[1] / 100 * TEXTURE_FREE_HEADROOM_PERCENT / MB;
    assert(high_mb > low_mb + 1 && "the two marks have to leave room between them");

    // Between the marks: under what it would like, above the point at which it
    // should start doing anything about it.
    fake_set_pools(81 * MB, (low_mb + high_mb) / 2 * MB, 26 * MB);
    int before = evicted_count;
    frames(600);
    printf("headroom     : %zu MB free, marks at %zu and %zu, %d evicted over 600 frames  OK\n",
           (low_mb + high_mb) / 2, low_mb, high_mb, evicted_count - before);
    assert(evicted_count == before && "a pool between the marks must be left alone");

    // Below the low mark, and it has to act -- and act past the low mark rather
    // than just back over it, or it lands straight back here next frame.
    fake_set_pools(81 * MB, (low_mb - 2) * MB, 26 * MB);
    frames(TEXTURE_POOL_SAMPLE_FRAMES * 2);
    assert(evicted_count > before && "a pool below the low mark must be reclaimed");
    printf("             : %d evicted once it dropped below the low mark  OK\n",
           evicted_count - before);

    // Hand the pools back healthy, or every block after this one inherits a
    // driver at its floor and tests a policy it did not mean to.
    fake_set_pools(81 * MB, 100 * MB, 26 * MB);
    frames(2);
  }

  // A texture still bound to a unit can be drawn without the game ever binding
  // it again, so there would be no moment at which to restore it.
  GLuint stuck = tex_upload(0x5AFE0001u, 512, 512, TEX_BYTES);
  drain();
  glBindTextureHook(GL_TEXTURE_2D, stuck);
  for (int i = 0; i < 64; i++) {
    tex_upload(0x99000000u + i, 512, 512, TEX_BYTES);
    drain();
  }
  frames(TEXTURE_IDLE_FRAMES * 4);
  assert(!textures[stuck].evicted && "a still-bound texture must not be evicted");
  printf("bound guard  : a bound texture is never evicted       OK\n");

  // A restore can fail -- the card may be gone, or vitaGL may be out of memory.
  // It frees the old data before it tries, so failing without putting a real
  // texture back leaves the game drawing from memory already handed away.
  // From a clean start, so that evicting it by hand is not competing with a
  // tick that has already spent this frame's ration of card writes.
  harness_start(512 * MB);
  GLuint fragile = tex_upload(0x5EED0001u, 512, 512, TEX_BYTES);
  drain();
  frames(1);
  glBindTextureHook(GL_TEXTURE_2D, 0); // so it is not the bound texture
  evict_texture(fragile);
  assert(textures[fragile].evicted);
  fake_reject_next_upload = 1; // the restore's upload will be refused
  glBindTextureHook(GL_TEXTURE_2D, fragile);
  assert(fake_slot_bytes[fragile] > 0 && "a failed restore must leave a real texture behind");
  assert(fake_sampled(fragile) == 0xFFFFFFFFu && "and it must be the placeholder");
  printf("failed restore: leaves a placeholder, not freed memory OK\n");

  // The failure this cache was written to prevent, reproduced exactly as it
  // happened on hardware: CDRAM runs dry while the RAM pool still has plenty,
  // so the total free memory reads healthy and a cache watching the total
  // evicts nothing. vitaGL does not fail the allocation -- it falls back to
  // RAM and then to the newlib heap -- so the game dies later, somewhere else,
  // out of heap. A session that crashed had CDRAM at 0 for 50,000 iterations
  // with 101 MB "free" and ev 0.
  {
    harness_start(0);
    fake_set_pools(81 * MB, 122 * MB, 26 * MB); // the figures off the console
    GLuint hot[64];
    for (int i = 0; i < 64; i++)
      hot[i] = tex_upload(0xC0000000u + i, 512, 512, TEX_BYTES);
    // A fixed number of uploads, chosen to come to less than the byte budget in
    // total, so the byte budget can never be what saves us -- but to more than
    // CDRAM holds, so that pool runs dry. Only the pool rule can catch this.
    const int count = (int)(((size_t)TEXTURE_BUDGET_MB * MB - 12 * MB) / TEX_BYTES);
    assert((size_t)count * TEX_BYTES > 81 * MB && "must be more than CDRAM holds");
    for (int i = 0; i < count; i++) {
      tex_upload(0xC1000000u + i, 512, 512, TEX_BYTES);
      tick();
    }
    wander(hot, 64, TEXTURE_IDLE_FRAMES * 3);
    int gone = 0;
    for (GLuint t = 1; t < MAX_TEXTURES; t++)
      if (textures[t].evicted)
        gone++;
    printf("pool drain   : cdram %zu MB free of 81 (was 0), %d evicted (was 0)   OK\n",
           fake_pool_free[0] / MB, gone);
    assert(tracked_bytes < (size_t)TEXTURE_BUDGET_MB * MB &&
           "the byte budget must not be what triggers this");
    assert(gone > 0 && "a pool running dry must be reclaimed against, however healthy the total looks");
    // Not the full 25% reserve: textures the game keeps drawing are reallocated
    // as they are restored, and with CDRAM preferred they land back in it. What
    // matters is that the pool is held well off the floor instead of sitting at
    // zero while the cache does nothing, which is what happened on hardware.
    assert(fake_pool_free[0] > fake_pool_start[0] / 8 &&
           "the drained pool must be held well clear of empty");
  }

  // Reclaiming that is refused must not be refused forever. On hardware the
  // RAM pool settled at 13% of its start -- above the 12% that opens the memory
  // card, below the 25% the cache aims for -- with the heap tier full because
  // the game had the heap. The cache wanted to evict and was told no 3.6
  // million times while the pools drained under it, and the session ended in
  // malloc. Cheap first is right; cheap forever is not.
  {
    // A driver small enough that the byte budget cannot cap the pressure before
    // the RAM pool gets low, since it is the pool that has to end up wedged.
    harness_start(160 * MB);
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB; // the game has the heap: no parking
    GLuint hot[32];
    for (int i = 0; i < 32; i++)
      hot[i] = tex_upload(0xB0000000u + i, 512, 512, TEX_BYTES);
    // Sit the RAM pool between the two thresholds -- under the 25% the cache
    // aims for, over the 12% that opens the card -- which is where nothing
    // would ever happen if the fixed threshold were the only way out.
    for (int i = 0; i < 4000 && fake_pool_free[1] > fake_pool_start[1] / 100 * 15; i++) {
      tex_upload(0xB1000000u + i, 512, 512, TEX_BYTES);
      tick();
    }
    assert(fake_pool_free[1] <= fake_pool_start[1] / 100 * 15 && "failed to wedge the pool");
    uint32_t spilled_before = card_evicted_count;
    wander(hot, 32, TEXTURE_BLOCKED_FRAMES * 4);
    printf("wedge        : ram %zu%% of start, %u written once cheap ran out  OK\n",
           fake_pool_free[1] * 100 / fake_pool_start[1], card_evicted_count - spilled_before);
    assert(fake_pool_free[1] > fake_pool_start[1] / 100 * TEXTURE_POOL_EMERGENCY_PERCENT &&
           "the fixed threshold must not be what saves this");
    assert(card_evicted_count > spilled_before &&
           "reclaiming blocked for long enough has to escalate, not wait forever");
  }

  // A file the console never finished writing must not be indexed as a good
  // copy. If it is, the next eviction of that texture is told the store already
  // holds it, frees the pixels without writing, and the restore then has
  // nothing to read: the texture is lost for the session, and the store goes on
  // lying about it in every run after this one.
  {
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);

    harness_start_empty(192 * MB);
    for (int i = 0; i < count; i++) {
      tex_upload(0x7C000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned written = fake_store_files();
    assert(written > 1 && "this block needs a store to damage");

    assert(fake_tear_one_store_file((long)sizeof(BackupRecord)) &&
           "the harness had a file to tear");
    fake_reset(192 * MB);
    texture_cache_init();
    assert(store_files == written - 1 &&
           "a file with a header and no data was counted as a usable copy");
    printf("torn file    : %u of %u indexed, the truncated one skipped  OK\n",
           store_files, written);
  }

  // The loader's own size estimate must never be what a copy is measured by.
  //
  // resident_size guesses from the GL enums and rounds rows by a rule of thumb.
  // It says so where it is defined -- close enough for the budget, not meant to
  // match vitaGL to the byte -- and for GL_RGBA4 with GL_UNSIGNED_BYTE it comes
  // out at four bytes a pixel where the driver allocated two. Evicting one of
  // those read twice the buffer out of vitaGL's pool, and restoring it wrote
  // twice the buffer back in, over whatever was next.
  {
    harness_start_empty(192 * MB);
    const int count = 260;
    GLuint ids[260];
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload_narrow(0x5E000000u + (unsigned)i, 512, 512);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);

    unsigned evicted = 0;
    for (int i = 0; i < count; i++)
      if (textures[ids[i]].evicted)
        evicted++;
    assert(evicted > 0 && "nothing was evicted, so nothing was copied");
    assert(!fake_first_overrun() && "eviction read or wrote past a texture buffer");

    int checked = 0;
    for (int i = 0; i < count && checked < 16; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      assert(fake_sampled(ids[i]) == fake_fingerprint_of(0x5E000000u + (unsigned)i) &&
             "a narrow-format texture must come back as itself");
      checked++;
      tick();
    }
    assert(checked > 0);
    assert(!fake_first_overrun() && "restoring wrote past a texture buffer");
    printf("narrow fmt   : %u evicted, %d restored, no buffer overrun  OK\n",
           evicted, checked);
  }

  // A file that belongs to a different texture must never be drawn as this one.
  //
  // The store is keyed by a hash of a sample of the pixels, so two textures can
  // in principle land on the same name -- and not only by chance: two that
  // differ solely in the gaps between sampled slices collide every time. The
  // record used to guard against that by carrying the key back and comparing
  // it, which is circular, since the file was found by that key. The verify
  // hash is the real guard: written only inside the record, over bytes the key
  // never reads. Scrambling it here is exactly what a colliding texture's file
  // looks like -- right name, right length, its own valid checksum, wrong
  // texture.
  {
    harness_start_empty(192 * MB);
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);
    GLuint ids[1536];
    assert(count <= (int)(sizeof(ids) / sizeof(ids[0])));

    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0x33000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    assert(fake_store_files() > 0 && "this block needs a file on the card");

    // Word 3 is verify_lo: magic, key_lo, key_hi, then verify_lo.
    assert(fake_scramble_store_word(3) && "the harness had a file to scramble");

    // Start again so the store is read fresh, then find the texture that file
    // belongs to and ask for it back.
    fake_reset(192 * MB);
    texture_cache_init();
    // Before the evictions, not after them: a file that will not read back is
    // now usually caught by the eviction that would have freed the pixels,
    // rather than by the restore that used to find the pixels already gone.
    uint32_t refused_before = restore_failed_count + store_unsound;
    for (int i = 0; i < count; i++) {
      ids[i] = tex_upload(0x33000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    int wrong = 0, looked = 0;
    for (int i = 0; i < count; i++) {
      if (!textures[ids[i]].evicted)
        continue;
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
      uint32_t got = fake_sampled(ids[i]);
      looked++;
      // Either the right texture, or a refusal. Never another texture's pixels.
      for (int j = 0; j < count; j++)
        if (j != i && got == fake_fingerprint_of(0x33000000u + (unsigned)j))
          wrong++;
      tick();
    }
    assert(looked > 0 && "nothing was evicted, so nothing was checked");
    assert(wrong == 0 && "a texture was restored as a different texture");

    // The scrambled one is only refused if the verify hash is actually
    // compared. Nothing else in the record would notice: the name, the length
    // and the checksum over the data are all still correct, which is precisely
    // the position a colliding texture's file leaves us in. Every other file
    // here is sound, so a refusal in this pass is that one and nothing else.
    //
    // Either end of the eviction catches it now. It used to be caught only at
    // restore, by which time the pixels were already freed and the texture was
    // lost; the eviction reads a carried-in file before believing it, so the
    // usual outcome is that it is thrown away and a good copy written over it.
    assert(restore_failed_count + store_unsound > refused_before &&
           "the mismatched file was accepted rather than refused");
    // And nothing was lost to it. The eviction that would have freed the
    // pixels reads a carried-in file first, so the bad one is thrown away and
    // a good copy written over it -- which is why the path is occupied again
    // afterwards and why no restore in this pass had to fail. A file that
    // fails must never be left indexed as good, whichever end catches it.
    assert(restore_failed_count == 0 &&
           "a texture was lost to a file that would not read back");
    // Off the card and out of the index, which are two separate things. An
    // index still claiming a file that is gone is the same trap by another
    // route: store_has says yes, the eviction frees the pixels without writing,
    // and the restore opens nothing.
    assert(store_files == fake_store_files() &&
           "the index claims files the card does not have");
    printf("wrong file   : %d restores checked, %d drawn as another texture, "
           "%u refused  OK\n", looked, wrong,
           restore_failed_count + store_unsound - refused_before);
  }

  // What the key and the verify hash have to be, before any of the machinery
  // above can mean anything.
  {
    harness_start_empty(192 * MB);
    const GLsizei size = 64 * 1024;

    // Two different textures must not share either hash.
    GLuint a = tex_upload(0x11111111u, 128, 128, size);
    GLuint b = tex_upload(0x22222222u, 128, 128, size);
    assert(textures[a].key != textures[b].key && "different pixels, different key");
    assert(textures[a].verify != textures[b].verify &&
           "the verify hash does not vary, so it can refuse nothing");

    // And the verify hash has to look where the key does not. The key reads the
    // first and last 8 KB in full and a 128-byte slice every 8 KB in between;
    // two textures differing only in the gaps between those slices collide
    // every time, not once in a billion. Build exactly that pair: identical
    // everywhere the key looks, different at a byte only the verify hash's
    // midpoint slices reach.
    memset(source_bytes, 0x5A, size);
    GLuint c;
    glGenTexturesHook(1, &c);
    glBindTextureHook(GL_TEXTURE_2D, c);
    glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,
                               128, 128, 0, size, source_bytes);

    // 12288 is 8 KB + half a stride: past the leading block, past the key's
    // first slice, and precisely where the verify hash starts sampling.
    source_bytes[12288] = 0x5B;
    GLuint d;
    glGenTexturesHook(1, &d);
    glBindTextureHook(GL_TEXTURE_2D, d);
    glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,
                               128, 128, 0, size, source_bytes);

    assert(textures[c].key == textures[d].key &&
           "the pair was meant to collide on the key; the sampling must have moved");
    assert(textures[c].verify != textures[d].verify &&
           "the verify hash reads the same bytes as the key, so it adds nothing");
    printf("hashes       : distinct textures differ; a key collision is caught "
           "by verify  OK\n");
  }

  // A store left by an older loader must be gone by the end of boot, not
  // believed and then found wanting one file at a time during play.
  //
  // The index is built from filenames; nothing opens a file. So when the record
  // layout changed and the name did not, a whole store of unreadable files
  // indexed as good -- and each one then cost a card open, a read, a failed
  // check and a delete at the moment its texture was wanted, on the frame
  // thread, having already told the eviction it was free.
  {
    harness_start_empty(192 * MB);
    const size_t over = ((size_t)TEXTURE_BUDGET_MB + TEXTURE_RAM_CACHE_MB + 96) * MB;
    const int count = (int)(over / TEX_BYTES);
    for (int i = 0; i < count; i++) {
      tex_upload(0x9F000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned written = fake_store_files();
    assert(written > 0 && "this block needs a store to age");

    // Rename every file to the shape the previous format used: the same key and
    // size, without the version. Byte for byte what was on the card before.
    assert(fake_age_store_files() == (int)written && "all of them renamed");
    assert(fake_store_files() == written && "the files are still there");

    fake_reset(192 * MB);
    texture_cache_init();
    assert(store_files == 0 &&
           "a store in the old format was indexed as usable");
    assert(fake_store_files() == 0 &&
           "and it was left on the card rather than cleared out");
    printf("old format   : %u files from a previous format discarded at boot  OK\n",
           written);

    // And the other way a store goes stale: written by a loader that did carry
    // a format, just not this one. Accepting any version is the same bug with
    // an extra step.
    for (int i = 0; i < count; i++) {
      tex_upload(0xA7000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
      tick();
    }
    frames(TEXTURE_IDLE_FRAMES * 8);
    unsigned again = fake_store_files();
    assert(again > 0);
    assert(fake_set_store_version(BACKUP_FORMAT - 1) == (int)again);
    fake_reset(192 * MB);
    texture_cache_init();
    assert(store_files == 0 && "a store from a different format version was indexed");
    printf("old version  : %u files stamped format %d discarded at boot  OK\n",
           again, BACKUP_FORMAT - 1);
  }

  // A restore that cannot get memory must not retire the texture.
  //
  // A stall has to be seen as a stall.
  //
  // vitaGL's allocator, when its pools cannot serve a texture, calls
  // sceGxmFinish and then sceKernelDelayThread for a full second, up to four
  // times. Whether that is happening is a question about the worst upload, and
  // the cache used to time one upload in sixty-four and scale the total up by
  // sixty-four -- which misses a rare stall almost always and reports it as a
  // minute when it does not.
  {
    harness_start_empty(192 * MB);
    TextureCacheStats st;
    for (int i = 0; i < 70; i++) { tex_upload(0x5701A110u + (unsigned)i, 256, 256, 65536); drain(); }
    texture_cache_stats(&st);
    assert(st.upload_slow == 0 && "nothing had stalled yet");
    int worst_before = st.upload_worst_ms;

    // One upload, on the wrong side of sixty-four, spends a second in the
    // driver the way a failed allocation does.
    fake_next_upload_delay_us = 1000000;
    tex_upload(0x5701A199u, 256, 256, 65536);
    drain();
    texture_cache_stats(&st);
    assert(st.upload_slow == 1 && "a one second upload was not counted as a stall");
    assert(st.upload_worst_ms >= 1000 && st.upload_worst_ms > worst_before &&
           "the worst upload was averaged away");
    assert(st.upload_driver_ms >= 1000 && "the second did not reach the total");
    printf("stall        : a one second upload is seen, not sampled over  OK\n");
  }

  // A stored copy that will not read back must be found before the pixels are
  // freed, not after.
  //
  // The scan indexes the store by filename and never opens anything, so a file
  // carried in from a previous run is a name in a directory. Believing it and
  // freeing the texture leaves a 1x1 placeholder with nothing to restore from:
  // a real session lost nineteen textures that way, and a lost texture draws
  // black for the rest of the run.
  {
    harness_start_empty(192 * MB);
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB; // no heap tier: it goes to the card
    GLuint id = tex_upload(0xC0B0A0FEu, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 2);
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(textures[id].evicted && fake_store_files() > 0);
    glBindTextureHook(GL_TEXTURE_2D, id);
    assert(!textures[id].evicted && "it had to come back for the rest of this");

    // Start again, so the store is read the way a new run reads it: from the
    // names alone, with nothing verified.
    unsigned kept = fake_store_files();
    fake_reset(192 * MB);
    texture_cache_init();
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB;
    assert(store_files == kept && "the file had to be carried in");
    id = tex_upload(0xC0B0A0FEu, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 2);
    assert(store_has(textures[id].key, textures[id].resident_size) &&
           "the carried-in file has to match this texture");

    // Damage it the way the console did: full length, right name, wrong bytes.
    assert(fake_scramble_store_word((int)(sizeof(BackupRecord) / 4) + 1) &&
           "the harness had a file to damage");

    uint32_t unsound_was = store_unsound, reused_was = store_reused;
    size_t bytes_was = fake_slot_bytes[id];
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(store_unsound == unsound_was + 1 &&
           "a copy that does not read back was trusted");
    assert(store_reused == reused_was && "and counted as a free eviction");
    // Either it wrote a fresh copy and evicted, or it kept the texture. What it
    // must never do is free the pixels believing a file that is not there.
    if (textures[id].evicted) {
      glBindTextureHook(GL_TEXTURE_2D, id);
      assert(!textures[id].evicted && "the fresh copy did not restore");
      assert(fake_sampled(id) == fake_fingerprint_of(0xC0B0A0FEu) &&
             "it came back as something else");
    } else {
      assert(fake_slot_bytes[id] == bytes_was && "the pixels were freed anyway");
    }
    printf("unsound copy : a carried-in file is read before it is trusted  OK\n");

    // And a file this run wrote is trusted without reading it again.
    uint32_t reads_was = store_unsound;
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    reused_was = store_reused;
    evict_texture(id);
    assert(store_unsound == reads_was && "a sound copy was called unsound");
    printf("             : one this run wrote is taken on trust  OK\n");
    fake_heap_used = 0;
  }

  // A texture vitaGL will not describe is a texture this cache must not drop.
  //
  // Evicting is not "put a copy somewhere and forget the pixels" -- the pixels
  // are freed by vitaGL, from its own allocator, and the allocator reads the
  // block's header to do it. If that header has been overwritten, freeing is
  // the operation that finds out. On hardware it found out with a chunk size of
  // 3.3 GB read out of texture pixels and a walk to an address that wrapped
  // past the end of memory, on the first eviction of the session.
  //
  // The cache cannot repair a broken allocation. What it can do is not be the
  // thing that steps on it: every path that ends in an eviction asks vitaGL how
  // big the buffer is first, and an answer that cannot be true means leave the
  // texture exactly where it is.
  {
    harness_start_empty(192 * MB);
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB; // no heap tier: it goes to the card
    GLuint id = tex_upload(0xDEFEC7EDu, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 2);
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(textures[id].evicted && "the copy had to reach the card");
    assert(fake_store_files() > 0);
    glBindTextureHook(GL_TEXTURE_2D, id); // bring it back; the file stays
    assert(!textures[id].evicted && "it had to come back for the rest of this");

    // Now the same texture is in the store, so the next eviction is the cheap
    // one: the cache knows the bytes are already saved and frees the pixels
    // without reading them. That is the path that has to look anyway.
    fake_slot_usable[id] = (size_t)TEXTURE_BACKUP_MAX_KB * 1024 + 1; // cannot be true
    size_t tracked_was = tracked_bytes;
    size_t bytes_was = fake_slot_bytes[id];
    uint32_t reused_was = store_reused;
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(!textures[id].evicted && "a texture vitaGL cannot size was dropped anyway");
    assert(fake_slot_bytes[id] == bytes_was && "its pixels were freed");
    assert(tracked_bytes == tracked_was && "it was accounted as gone");
    assert(store_reused == reused_was && "it was counted as a free eviction");
    assert(textures[id].unbacked && "and it must not be considered again");
    printf("bad header   : an unsizeable texture is left alone, not freed  OK\n");

    // The same for a texture vitaGL will not hand a pointer to at all.
    fake_slot_usable[id] = 0;
    textures[id].unbacked = 0; // ask it again
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(!textures[id].evicted && "a texture with no usable size was dropped");
    assert(fake_slot_bytes[id] == bytes_was && "its pixels were freed");
    printf("             : and one vitaGL reports as zero bytes  OK\n");

    // And the cheap path still has to be cheap when nothing is wrong with it,
    // or this guard has quietly turned every store hit into a card write.
    fake_slot_usable[id] = bytes_was;
    textures[id].unbacked = 0;
    uint32_t spilled_was = card_evicted_count, parked_was = ram_evicted_count;
    reused_was = store_reused;
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(textures[id].evicted && "a sound texture in the store must still go");
    assert(store_reused == reused_was + 1 && "and go for free");
    assert(card_evicted_count == spilled_was && "it must not be written again");
    assert(ram_evicted_count == parked_was && "nor copied to the heap");
    printf("             : a sound one still evicts for free  OK\n");
    fake_heap_used = 0;
  }

  // Reclaiming has to start before the frame rate has already gone.
  //
  // The measurement this is set from: with the loader idle and the game drawing
  // a world, a 105 MB RAM pool at 18 MB or less ran at a median of 18
  // ProcessEvents a second against 45 to 76 once it was 19 or more. The cache
  // cannot see what a nearly empty pool costs -- it is not spending that time
  // itself -- so the only thing it can do is not let the pool get there.
  {
    harness_start_empty(0);
    fake_set_pools(81 * MB, 105 * MB, 26 * MB); // the figures off the console
    tick();
    assert(pool_start[1] == 105 * MB);

    GLuint held[192];
    for (int i = 0; i < 192; i++) {
      held[i] = tex_upload(0xDEC1DE00u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
    }
    (void)held;
    frames(TEXTURE_IDLE_FRAMES * 2);

    // Walk the pool down the way a game loading an area does, and note where
    // the cache first does something about it.
    uint32_t before = evicted_count;
    size_t acted_at = 0;
    for (size_t free_mb = 40; free_mb >= 8 && !acted_at; free_mb -= 2) {
      fake_set_pools(81 * MB, free_mb * MB, 26 * MB);
      frames(TEXTURE_POOL_SAMPLE_FRAMES * 2);
      if (evicted_count > before)
        acted_at = free_mb;
    }
    assert(acted_at && "the cache never reclaimed at all");
    printf("headroom     : first eviction with %zu MB of 105 still free  OK\n", acted_at);
    assert(acted_at > 18 &&
           "reclaiming starts inside the band where the frame rate has collapsed");
  }

  // The intro movie is not a texture shortage.
  //
  // Phycont is not a vitaGL pool in this build. PHYCONT_ON_DEMAND makes
  // vglMemFree(PHYCONT) the kernel's answer for the whole process, and
  // SceAvPlayer takes 23 of the 26 MB for as long as a movie is playing. The
  // cache samples that pool from the tick, the tick runs from ProcessEvents,
  // and the movie draws and swaps from ProcessEvents too -- so every session
  // opened with the cache reading 3 MB of 26, calling it a shortage, and
  // emptying itself of everything the game had uploaded and was not drawing at
  // that instant: the menu font, the HUD, the first street. Nineteen textures,
  // at the same point, in three sessions running.
  {
    harness_start_empty(0);
    fake_set_pools(81 * MB, 105 * MB, 26 * MB); // the figures off the console
    tick();                                     // takes the starting figures
    assert(pool_start[2] == 26 * MB && "the fake has to start where hardware does");

    GLuint boot[48];
    for (int i = 0; i < 48; i++) {
      boot[i] = tex_upload(0x9E000000u + (unsigned)i, 512, 512, TEX_BYTES);
      drain();
    }
    (void)boot;
    // Nothing is drawn while the movie is on screen, so every one of them is a
    // candidate by the time it ends.
    frames(TEXTURE_IDLE_FRAMES * 2);
    fake_set_pools(81 * MB, 105 * MB, 26 * MB);
    uint32_t before = evicted_count;

    fake_set_pools(81 * MB, 105 * MB, 3 * MB); // the player has phycont
    frames(600);
    assert(evicted_count == before &&
           "the movie's phycont use was read as this cache's pools running dry");
    printf("movie        : phycont at 3 MB of 26 for 600 ticks, %u evicted  OK\n",
           evicted_count - before);

    // And the pool that does mean something still does, or this test passes by
    // having broken reclaiming altogether.
    // Long enough to cover the sampling interval: a pool this far clear of its
    // mark is only asked about once every fifteen frames, which is the whole
    // point of the interval.
    fake_set_pools(81 * MB, 10 * MB, 26 * MB);
    frames(TEXTURE_POOL_SAMPLE_FRAMES * 2);
    assert(evicted_count > before && "a real shortage must still be reclaimed");
    printf("             : ram at 10 MB of 105, %u evicted  OK\n",
           evicted_count - before);
  }

  // A copy has to fit the buffer it goes back into, not match it.
  //
  // vglMallocUsableSize answers for the block a pointer landed in, and which
  // block that is depends on where there was room: a texture allocated out of
  // an on-demand phycont block reports its mapped size, rounded up to a
  // megabyte, where the same texture out of the RAM mspace reports close to
  // what was asked for. Demanding the two agree retires every texture that
  // comes back into a different kind of memory than it left.
  {
    harness_start_empty(192 * MB);
    GLuint id = tex_upload(0x510C0001u, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 2);
    glBindTextureHook(GL_TEXTURE_2D, 0); // so it is not the bound texture
    evict_texture(id);
    assert(textures[id].evicted && "the copy could not be taken");

    uint32_t failed_before = restore_failed_count;
    fake_next_usable_bonus = 512 * 1024; // it comes back in a roomier block
    glBindTextureHook(GL_TEXTURE_2D, id);
    assert(!textures[id].evicted && "a buffer with room to spare was refused");
    assert(restore_failed_count == failed_before && "and the texture written off");
    assert(fake_sampled(id) == fake_fingerprint_of(0x510C0001u) &&
           "it came back as something else");
    assert(!fake_first_overrun() && "the restore wrote past the buffer");
    printf("slack        : restored into a buffer %d KB larger than the copy  OK\n", 512);
  }

  // A copy that genuinely will not fit is refused -- and taken off the card
  // with it. Left there, store_has tells the next eviction the store already
  // holds this texture, the pixels are freed without writing, and the restore
  // meets a record it has already refused once. For the rest of the session,
  // and for every session after it, because nothing ever rewrites the file.
  {
    harness_start_empty(192 * MB);
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB; // no heap tier: it goes to the card
    // Captured out of a block worth more than the texture needs, which is what
    // a phycont-resident texture looks like: the record is written for the
    // whole block, and no ordinary buffer will ever be that size.
    fake_next_usable_bonus = 64 * 1024;
    GLuint id = tex_upload(0xB16B0001u, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 2);
    glBindTextureHook(GL_TEXTURE_2D, 0);
    card_writes_allowed = 1;
    evict_texture(id);
    assert(textures[id].evicted && "the copy could not be taken");
    assert(textures[id].backup_bytes == TEX_BYTES + 64 * 1024 &&
           "the record has to carry the block's size, not the texture's");
    unsigned files = fake_store_files();
    assert(files > 0 && "the copy had to reach the card for this");

    uint32_t failed_before = restore_failed_count;
    glBindTextureHook(GL_TEXTURE_2D, id); // replayed into an ordinary buffer
    assert(textures[id].unbacked && "a copy that does not fit has to be refused");
    assert(restore_failed_count == failed_before + 1 && "and counted, once");
    assert(fake_store_files() == files - 1 &&
           "the copy that will not fit is still on the card");
    assert(store_files == fake_store_files() &&
           "the index still claims a file the card does not have");
    assert(!fake_first_overrun() && "the refused restore wrote past the buffer");
    fake_heap_used = 0;
    printf("misfit       : a copy too big for the buffer is refused and dropped  OK\n");
  }

  // vglGetTexDataPointer returning NULL means the pools are full right now --
  // which is precisely when the cache is working hardest to empty them, so it
  // is also when a second attempt is most likely to work. Treating it as "this
  // texture is gone" threw away the only copy and marked it unbacked for the
  // rest of the session. With the pools genuinely exhausted every restore took
  // that path as the game asked for it: a real session logged 21461 failures
  // and stopped having surfaces.
  {
    harness_start_empty(192 * MB);
    GLuint id = tex_upload(0xC0FFEE00u, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 4);

    // Force it out, then make the next allocation fail the way a full pool does.
    // Push it out through the cache's own path rather than by hand, so what is
    // held afterwards is exactly what a real eviction leaves.
    assert(backup_capture(&textures[id], id) == 1 && "the copy has to be taken");
    evict_texture(id);
    assert(textures[id].evicted && "the texture has to be evicted to restore it");
    uint32_t failed_before = restore_failed_count;
    uint32_t later_before = restore_deferred_count;

    fake_reject_next_upload = 1;
    glBindTextureHook(GL_TEXTURE_2D, id);

    assert(restore_deferred_count == later_before + 1 && "counted as a shortage");
    assert(restore_failed_count == failed_before && "and not as a failure");
    assert(!textures[id].unbacked && "the texture was retired over a shortage");
    assert(textures[id].levels > 0 && "the saved shape was thrown away");
    assert(textures[id].evicted && "it must stay evicted so the next bind retries");

    // Now there is room again. The next bind must bring it back, byte for byte.
    glBindTextureHook(GL_TEXTURE_2D, id);
    assert(!textures[id].evicted && "the retry did not restore it");
    assert(fake_sampled(id) == fake_fingerprint_of(0xC0FFEE00u) &&
           "it came back as something else");
    printf("shortage     : a restore with no memory is retried, not retired  OK\n");

    // The other side of the same split. A file that is not there is not a
    // shortage and never becomes one: retrying it means a failed card open on
    // every bind of that texture for the rest of the session.
    GLuint gone = tex_upload(0xDEAD0000u, 512, 512, TEX_BYTES);
    drain();
    // The card path, explicitly: the heap tier taken away and card writes
    // opened. This used to rely on an earlier block having left the heap full,
    // and a copy that quietly goes to the heap instead leaves no file to delete
    // and a test that proves nothing. Set before the ticks, because whether the
    // heap is tight is decided once a tick and not per eviction.
    size_t heap_was = fake_heap_used;
    fake_heap_used = (size_t)MEMORY_NEWLIB_MB * MB;
    frames(TEXTURE_IDLE_FRAMES * 4);
    card_writes_allowed = 1;
    textures[gone].ram_copy = NULL;
    assert(backup_capture(&textures[gone], gone) == 1);
    fake_heap_used = heap_was;
    evict_texture(gone);
    assert(fake_wipe_store() > 0 && "the harness had a file to delete");

    failed_before = restore_failed_count;
    later_before = restore_deferred_count;
    glBindTextureHook(GL_TEXTURE_2D, gone);
    assert(restore_failed_count == failed_before + 1 &&
           "a missing file must be permanent");
    assert(restore_deferred_count == later_before &&
           "a missing file must not be retried as a shortage");
    assert(textures[gone].unbacked && "and the texture must be retired");
    printf("file gone    : a missing copy is permanent, not retried  OK\n");

    // And it is one lost texture, however many times the game goes on drawing
    // it. Counting the binds instead turned seventeen dead textures into
    // 112888 failures in a trace, which reads as a cache coming apart rather
    // than as a handful of textures that never came back.
    failed_before = restore_failed_count;
    for (int i = 0; i < 200; i++) {
      glBindTextureHook(GL_TEXTURE_2D, gone);
      tick();
    }
    assert(restore_failed_count == failed_before &&
           "every bind of a dead texture was counted as another failure");
    printf("             : 200 more binds of it counted 0 more  OK\n");
  }

  // Idle has to be time, not ticks.
  //
  // frame_counter advances once per ProcessEvents call, and a real session ran
  // those from 1 to 718 a second -- a menu spins through them while drawing
  // almost nothing. Measured in ticks, the same threshold of 150 meant 5.8
  // seconds in a busy street and 0.21 seconds in a menu, so a font atlas that
  // went one line of dialogue without being drawn was evicted, and the text
  // stopped appearing. Seven hundred times the intended policy, decided by
  // where the player happened to be standing.
  {
    harness_start_empty(192 * MB);
    GLuint id = tex_upload(0x7EA70000u, 512, 512, TEX_BYTES);
    drain();
    glBindTextureHook(GL_TEXTURE_2D, id);
    tick();
    // Bind something else afterwards. A texture still bound to a unit is never
    // a candidate whatever the policy says -- it could be drawn again without
    // the game rebinding it -- so leaving it bound would make this pass for a
    // reason that has nothing to do with time.
    GLuint other = tex_upload(0x7EA70001u, 64, 64, 32 * 1024);
    drain();
    glBindTextureHook(GL_TEXTURE_2D, other);

    // A menu: 718 ticks a second, so 1393 us each. Run well past the old
    // threshold of 150 ticks -- but nothing like the idle period in real time.
    uint32_t started_ms = now_ms;
    for (int i = 0; i < 600; i++)
      tick_us(1393);
    assert(now_ms - started_ms < TEXTURE_IDLE_MS &&
           "the test has to stay inside the idle window in real time");

    // Squeeze hard enough that it would take anything it is allowed to take,
    // with both tiers open so that a texture it decides to evict actually goes.
    // Otherwise the eviction is merely deferred, the texture stays resident for
    // a reason that has nothing to do with idle-ness, and the test passes
    // whatever the policy is.
    card_writes_allowed = 1;
    fake_set_pools(2 * MB, 2 * MB, 2 * MB);
    // More ticks than the pool re-sample interval, or the cache is still
    // looking at the healthy figures it read before the squeeze and sees no
    // shortage to act on -- which would make this pass without the policy ever
    // being consulted. Still well inside the idle window in real time.
    frames(TEXTURE_POOL_SAMPLE_FRAMES + 5);
    assert(!textures[id].evicted &&
           "a texture drawn a fraction of a second ago was evicted");
    printf("idle is time : 600 ticks in %u ms did not make a texture stale  OK\n",
           now_ms - started_ms);
  }

  // The game must never write into the placeholder.
  //
  // An evicted texture is a 1x1 stand-in. A glyph written into that with
  // glTexSubImage2D goes nowhere, and the restore afterwards puts the older
  // copy back over the top, so the glyph is simply gone. A font atlas the game
  // fills in one glyph at a time loses every one written while it was evicted,
  // which on screen is a menu with its border and its flourishes and no words.
  {
    harness_start_empty(192 * MB);
    GLuint id = tex_upload(0xF0417000u, 512, 512, TEX_BYTES);
    drain();
    frames(TEXTURE_IDLE_FRAMES * 4);
    assert(backup_capture(&textures[id], id) == 1);
    evict_texture(id);
    assert(textures[id].evicted && "it has to be evicted for this to mean anything");

    // The game writes a glyph into what it believes is the atlas.
    glBindTextureHook(GL_TEXTURE_2D, id);
    fill_source(0x9A9A9A9Au, TEX_BYTES);
    glTexSubImage2DHook(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA,
                        GL_UNSIGNED_BYTE, source_bytes);

    assert(fake_last_subimage_slot_bytes > 4 &&
           "the update was aimed at the 1x1 placeholder");
    assert(!textures[id].evicted && "the texture was not brought back first");
    assert(fake_sampled(id) == fake_fingerprint_of(0x9A9A9A9Au) &&
           "the update did not survive");
    assert(!fake_first_overrun() && "the update ran past a texture buffer");
    printf("sub-image    : a write to an evicted texture brings it back first  OK\n");
  }

  printf("PASS\n");
  return 0;
}
