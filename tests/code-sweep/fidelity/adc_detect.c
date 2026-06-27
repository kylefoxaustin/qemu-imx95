// SPDX-License-Identifier: GPL-2.0-or-later
// Fidelity detector: does the i.MX ADC return real conversions, or a constant?
// The imx93_adc model returns the synthetic value 0x100+ch*0x111 per channel and
// raises EOC instantly (hw/misc/imx93_adc.c). We read each iio channel twice; if
// a channel is invariant AND matches the synthetic formula, it's SILENT-WRONG
// (signals a conversion that never sampled anything). A real ADC would at least
// vary, or the block should fault. The detector "passes" by classifying the
// block correctly; a SILENT-WRONG verdict is the detector working.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static int rd(const char *path, long *v) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    int n = fscanf(f, "%ld", v); fclose(f); return n == 1 ? 0 : -1;
}

int main(void) {
    char base[300] = "";
    DIR *d = opendir("/sys/bus/iio/devices"); struct dirent *e;
    if (d) {
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "iio:device", 10)) continue;
            char nm[400], buf[64];
            snprintf(nm, sizeof nm, "/sys/bus/iio/devices/%s/name", e->d_name);
            FILE *f = fopen(nm, "r");
            if (f) { if (fgets(buf, sizeof buf, f) && (strstr(buf, "adc") || strstr(buf, "ADC")))
                         snprintf(base, sizeof base, "/sys/bus/iio/devices/%s", e->d_name);
                     fclose(f); }
        }
        closedir(d);
    }
    if (!base[0]) { printf("[adc] no iio ADC device found\n"); return 77; }
    printf("[adc] device %s\n", base);
    int silent = 0, real = 0, ch;
    for (ch = 0; ch < 8; ch++) {
        char p[400]; long a, b;
        snprintf(p, sizeof p, "%s/in_voltage%d_raw", base, ch);
        if (rd(p, &a) < 0) continue;
        usleep(1000); rd(p, &b);
        long synth = (0x100 + ch * 0x111) & 0xfff;
        const char *v = (a == b && a == synth) ? "SILENT-WRONG(synthetic constant)"
                      : (a == b)               ? "constant(suspect)"
                                               : "varies(plausibly real)";
        printf("  ch%d raw=%ld reread=%ld synth=%ld -> %s\n", ch, a, b, synth, v);
        if (a == b && a == synth) silent++; else real++;
    }
    if (silent > 0 && real == 0)
        printf("VERDICT: adc = SILENT-WRONG (synthetic constants, not conversions)\n");
    else if (real > 0)
        printf("VERDICT: adc = values vary (may be real)\n");
    else
        printf("VERDICT: adc = no channels readable\n");
    return 0;
}
