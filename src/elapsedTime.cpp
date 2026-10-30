//    SAPF - Sound As Pure Form
//    Copyright (C) 2019 James McCartney
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "elapsedTime.hpp"
#include <mach/mach_time.h>
#include <pthread.h>

extern "C" {

static double gHostClockFreq;

void initElapsedTime()
{
	struct mach_timebase_info info;
	mach_timebase_info(&info);
	// The timebase converts ticks to nanoseconds: ns = ticks * numer / denom.
	gHostClockFreq = 1e9 * ((double)info.denom / (double)info.numer);
}

double elapsedTime()
{
	return (double)mach_absolute_time() / gHostClockFreq;
}

}
