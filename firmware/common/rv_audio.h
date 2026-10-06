// Audio format shared by the Pico A firmware (and later the rvlink code).
#pragma once

#define RV_SAMPLE_RATE_HZ   48000u
#define RV_BLOCK_FRAMES     64u     // stereo frames per block (matches rvlink)
#define RV_BLOCK_WORDS      (RV_BLOCK_FRAMES * 2u)
#define RV_BLOCKS_PER_SEC   (RV_SAMPLE_RATE_HZ / RV_BLOCK_FRAMES)   // 750
