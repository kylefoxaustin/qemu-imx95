/*
 * Minimal ALSA playback oracle for the i.MX 95 wm8962/SAI3 card.
 *
 * Copyright (c) 2026, Kyle Fox
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plays a generated square wave to an ALSA PCM device (default hw:0,0), driving
 * the eDMA cyclic -> SAI3 TX FIFO -> audio-backend datapath. A clean "PASS"
 * with drain success means the DMA paced the whole stream at the audio rate
 * without under-running; the SAI hands the samples to the QEMU audio backend,
 * so -audio captures the playback to a wav for content verification. There is
 * no aplay in a busybox initramfs, hence this. Cross-compile against an ALSA
 * sysroot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:0,0";
    unsigned int chans = 2;
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 2;
    /*
     * THE RATE IS AN ARGUMENT, and that is the whole point of this change.
     *
     * It used to be hardcoded to 48000 - and so is the SAI's FIFO pacing and
     * the
     * rate it opens its audio voice at. A test that only ever asks for 48 kHz
     * is
     * structurally incapable of noticing that the model IGNORES the rate it was
     * asked for: the one operating point everybody tests is the one where a
     * rate-ignoring model and a correct one are indistinguishable.
     *
     * A FORMULA - OR A MODEL - THAT IS CORRECT AT THE POINT YOU TESTED IT IS
     * NOT ONE YOU HAVE TESTED.   (93emulator)
     */
    unsigned int rate = argc > 3 ? (unsigned)atoi(argv[3]) : 48000;
    snd_pcm_t *pcm;
    int err, i;
    long frames = (long)rate * secs;
    short *buf;
    snd_pcm_sframes_t w;
    struct timespec t0, t1;
    double elapsed;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("PLAY[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate,
                             1, 200000);
    if (err < 0) {
        printf("PLAY[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("PLAY[%s]: %u Hz %u ch S16_LE, %ld frames\n", dev, rate, chans,
           frames);

    /* 440 Hz square wave: half-period ~55 frames at 48 kHz. */
    buf = malloc(frames * chans * sizeof(short));
    for (i = 0; i < frames; i++) {
        short v = ((i / 55) & 1) ? 8000 : -8000;
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }

    /*
     * TIME IT, BECAUSE TIME IS THE ONLY RATE-INDEPENDENT ORACLE HERE.
     *
     * snd_pcm_writei() + drain block until the hardware has actually clocked
     * the
     * samples out. So N seconds of audio must take N seconds of wall clock AT
     * ANY RATE. If the SAI paces its FIFO at a hardcoded 48 kHz, a 16 kHz
     * stream
     * is clocked out three times too fast and finishes in a third of the time -
     * while writei succeeds, drain succeeds, and every frame is accounted for.
     *
     * (The captured WAV cannot see this: QEMU's wav backend writes its own
     * sample rate into the header - 44100 by default - and the audio layer
     * resamples into it, so the header says nothing about what the SAI did. An
     * oracle that measures the wrong device is worse than no oracle: it is a
     * green light with a citation.)
     */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    w = snd_pcm_writei(pcm, buf, frames);
    printf("PLAY[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("PLAY[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed = (t1.tv_sec - t0.tv_sec) +
              (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("PLAY[%s]: drain -> %s\n", dev, snd_strerror(err));
    snd_pcm_close(pcm);
    printf("PLAY[%s]: elapsed=%.2fs want=%.2fs (%u Hz)\n",
           dev, elapsed, (double)secs, rate);
    printf("PLAY[%s]: %s (%ld frames)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w);
    return 0;
}
