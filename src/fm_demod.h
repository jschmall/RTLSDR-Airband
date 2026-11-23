/*
 * fm_demod.h
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

#ifndef _FM_DEMOD_H
#define _FM_DEMOD_H

enum fm_demod_algo { FM_FAST_ATAN2, FM_QUADRI_DEMOD, FM_ATAN2 };

// Values of the "fm_demod" global config option
static const char* const fm_demod_names = "\"fast_atan2\", \"atan2\" or \"quadri\"";

// Returns false, leaving *algo unchanged, if name is not a known demodulator
bool parse_fm_demod(const char* name, fm_demod_algo* algo);

// Phase of a * conj(b) in units of pi, from the exact arctangent
float polar_disc_atan2(float ar, float aj, float br, float bj);

#endif /* _FM_DEMOD_H */
