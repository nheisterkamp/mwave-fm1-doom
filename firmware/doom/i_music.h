// SPDX-License-Identifier: GPL-2.0-or-later
// Music: the MUS lumps played by a small FM synthesizer.

#ifndef __I_MUSIC__
#define __I_MUSIC__

#include <stdint.h>

// Add the music to a block of the mix: frames of stereo int32, at rate.
// Called from the sound mixer (the audio interrupt on the FM-1).
void I_MusicMix(int32_t *acc, int frames, int rate);

#endif
