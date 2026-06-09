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
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:0,0";
    unsigned int rate = 48000, chans = 2;
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 2;
    snd_pcm_t *pcm;
    int err, i;
    long frames = (long)rate * secs;
    short *buf;
    snd_pcm_sframes_t w;

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

    w = snd_pcm_writei(pcm, buf, frames);
    printf("PLAY[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("PLAY[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    printf("PLAY[%s]: drain -> %s\n", dev, snd_strerror(err));
    snd_pcm_close(pcm);
    printf("PLAY[%s]: %s (%ld frames)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w);
    return 0;
}
