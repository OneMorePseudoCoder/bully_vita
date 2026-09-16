/* fps_cap.h -- the frame limiter the game runs itself
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FPS_CAP_H__
#define __FPS_CAP_H__

// Hooks Application::ClampFPS and switches its limiter off, if the opt-in file
// is there. Call once, from patch_game, before the frame profiler -- they
// cannot both hook the same function.
void fps_cap_init(void);

// Whether the above took the hook, so the profiler knows to leave it alone.
extern int fps_cap_lifted;

#endif
