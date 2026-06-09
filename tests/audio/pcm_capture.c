/*
 * Minimal ALSA capture oracle for the i.MX 95 MICFIL (PDM) audio card.
 *
 * Copyright (c) 2026, Kyle Fox
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Records from an ALSA capture PCM and checks the buffer carries real,
 * non-silent, varying samples. Used for the MICFIL PDM card (S32_LE - the
 * MICFIL DATACH0 -> eDMA cyclic path). The model synthesises a sawtooth (no
 * physical mic is wired), so a working capture path returns real signal. Prints
 * "CAP[...]: PASS (non-silent)" on success. Cross-compile against an ALSA
 * sysroot; harvested into the test initramfs alongside libasound.
 *
 * Usage: pcm_capture <dev> [frames] [S16|S32] [channels]  (S16/2ch default)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:0,0";
    long frames = argc > 2 ? atol(argv[2]) : 4096;
    int s32 = argc > 3 && strcmp(argv[3], "S32") == 0;
    snd_pcm_format_t fmt = s32 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE;
    unsigned int rate = 48000, chans = argc > 4 ? (unsigned)atoi(argv[4]) : 2;
    snd_pcm_t *pcm;
    void *buf;
    snd_pcm_sframes_t r;
    long i, samples, nonzero = 0, distinct = 0;
    long long peak = 0, prev = 0;
    int err;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        printf("CAP[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED, chans,
                             rate, 1, 500000);
    if (err < 0) {
        printf("CAP[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("CAP[%s]: %u Hz %u ch %s, reading %ld frames\n", dev, rate, chans,
           s32 ? "S32_LE" : "S16_LE", frames);

    /*
     * Start the stream explicitly. snd_pcm_set_params leaves the capture
     * stream PREPARED but does not auto-start it on the first readi here, so
     * without this the read returns -EIO. An explicit start enables the
     * receiver (MICFIL CTRL1.PDMIEN) and the eDMA channel.
     */
    err = snd_pcm_start(pcm);
    if (err < 0) {
        printf("CAP[%s]: start: %s\n", dev, snd_strerror(err));
        return 1;
    }

    buf = malloc(frames * chans * (s32 ? 4 : 2));
    r = snd_pcm_readi(pcm, buf, frames);
    printf("CAP[%s]: readi -> %ld\n", dev, (long)r);
    if (r < 0) {
        printf("CAP[%s]: FAIL (%s)\n", dev, snd_strerror((int)r));
        return 1;
    }

    samples = r * chans;
    for (i = 0; i < samples; i++) {
        long long v = s32 ? ((int32_t *)buf)[i] : ((short *)buf)[i];
        long long a = v < 0 ? -v : v;

        if (a > peak) {
            peak = a;
        }
        if (v) {
            nonzero++;
        }
        if (i && v != prev) {
            distinct++;
        }
        prev = v;
    }
    printf("CAP[%s]: frames=%ld peak=%lld nonzero=%ld changes=%ld\n",
           dev, (long)r, peak, nonzero, distinct);

    /* Real signal: a meaningful peak and a varying (not stuck) waveform. */
    if (peak > (s32 ? (1 << 20) : 1000) && nonzero > r && distinct > 8) {
        printf("CAP[%s]: PASS (non-silent)\n", dev);
        err = 0;
    } else {
        printf("CAP[%s]: FAIL (silent/stuck)\n", dev);
        err = 1;
    }
    snd_pcm_close(pcm);
    return err;
}
