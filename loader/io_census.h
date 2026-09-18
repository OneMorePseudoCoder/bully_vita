/* io_census.h -- which files the game keeps opening
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IO_CENSUS_H__
#define __IO_CENSUS_H__

// Creates the lock. Call once, before the game can open anything.
void io_census_init(void);

// Records one fopen against the path it was for, with what it cost.
void io_census_open(const char *path, unsigned us);
// Records a read's bytes and cost against whatever was opened most recently on
// this thread, which is the file it is almost certainly reading from.
void io_census_read(unsigned us, unsigned bytes);
// The heartbeat line: the paths opened most often, and the ones that cost most.
void io_census_report(void);

#endif
