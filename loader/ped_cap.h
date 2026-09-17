/* ped_cap.h -- a ceiling on the ambient ped count
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __PED_CAP_H__
#define __PED_CAP_H__

// Hooks CPopulation::RoomForAnotherAmbientPed and refuses the spawn once the
// ambient population is at the cap. Call once, from patch_game.
void ped_cap_init(void);

// One line in the heartbeat: what the cap is and how often it has bitten.
void ped_cap_report(void);

#endif
