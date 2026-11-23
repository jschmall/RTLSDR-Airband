/*
 * fm_demod.cpp
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "fm_demod.h"

#include <cmath>
#include <cstring>

bool parse_fm_demod(const char* name, fm_demod_algo* algo) {
    if (strcmp(name, "fast_atan2") == 0) {
        *algo = FM_FAST_ATAN2;
    } else if (strcmp(name, "atan2") == 0) {
        *algo = FM_ATAN2;
    } else if (strcmp(name, "quadri") == 0) {
        *algo = FM_QUADRI_DEMOD;
    } else {
        return false;
    }
    return true;
}

float polar_disc_atan2(float ar, float aj, float br, float bj) {
    float const cr = ar * br + aj * bj;
    float const cj = aj * br - ar * bj;
    // atan2(0, 0) is a plain 0 only without -ffast-math
    if (cr == 0.0f && cj == 0.0f) {
        return 0.0f;
    }
    return (float)(atan2f(cj, cr) * M_1_PI);
}
