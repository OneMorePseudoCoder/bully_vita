/* frame_profile.h -- where a frame of the game's own code goes
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FRAME_PROFILE_H__
#define __FRAME_PROFILE_H__

#include <stdint.h>

// Hooks the engine's frame-level Update/Process methods, if the opt-in file is
// there. Call once, after the module is loaded and before the game runs.
void frame_profile_init(void);

// One heartbeat's worth: the slots that cost anything, busiest first, as deltas
// over the interval. Does nothing when profiling is off.
void frame_profile_report(void);

// The game's own file reading, counted only while the profiler is on. main.c
// wraps fopen/fseek/fread with these so that a function measured as slow can be
// told from a card measured as slow.
void frame_profile_io_open(unsigned started);
void frame_profile_io_seek(void);
unsigned frame_profile_io_begin(void);
void frame_profile_io_end(unsigned started, unsigned bytes);

#endif
