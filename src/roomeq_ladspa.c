/*
 * roomeq LADSPA plugin: a mono chain of peaking biquads plus a preamp.
 *
 * The filter set is read from a text file every time the plugin is instantiated
 * (i.e. every time an application opens the ALSA device), so a new measurement takes
 * effect on the next playback without restarting anything:
 *
 *     preamp <dB>
 *     pk <frequency Hz> <gain dB> <Q>      (up to 16 lines)
 *
 * File: $ROOMEQ_FILTERS, or ROOMEQ_DEFAULT_FILTERS set at build time.
 * Missing or empty file = transparent passthrough.
 *
 * ALSA's ladspa plugin instantiates one copy per channel ("policy duplicate").
 * MIT License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef float LADSPA_Data;
typedef int LADSPA_Properties;
typedef int LADSPA_PortDescriptor;
typedef int LADSPA_PortRangeHintDescriptor;
typedef struct { LADSPA_PortRangeHintDescriptor HintDescriptor; LADSPA_Data LowerBound, UpperBound; } LADSPA_PortRangeHint;
typedef void *LADSPA_Handle;
typedef struct _LADSPA_Descriptor {
    unsigned long UniqueID; const char *Label; LADSPA_Properties Properties;
    const char *Name, *Maker, *Copyright; unsigned long PortCount;
    const LADSPA_PortDescriptor *PortDescriptors; const char *const *PortNames;
    const LADSPA_PortRangeHint *PortRangeHints; void *ImplementationData;
    LADSPA_Handle (*instantiate)(const struct _LADSPA_Descriptor *, unsigned long);
    void (*connect_port)(LADSPA_Handle, unsigned long, LADSPA_Data *);
    void (*activate)(LADSPA_Handle);
    void (*run)(LADSPA_Handle, unsigned long);
    void (*run_adding)(LADSPA_Handle, unsigned long);
    void (*set_run_adding_gain)(LADSPA_Handle, LADSPA_Data);
    void (*deactivate)(LADSPA_Handle);
    void (*cleanup)(LADSPA_Handle);
} LADSPA_Descriptor;

#define MAXF 16
#ifndef ROOMEQ_DEFAULT_FILTERS
#define ROOMEQ_DEFAULT_FILTERS "/storage/.config/roomeq/filters.txt"
#endif

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } Biquad;
typedef struct { LADSPA_Data *in, *out; int n; double pre; Biquad f[MAXF]; } Inst;

static void peaking(Biquad *q, double fs, double f0, double g, double Q) {
    double A = pow(10.0, g / 40.0), w = 2.0 * M_PI * f0 / fs, al = sin(w) / (2.0 * Q), c = cos(w);
    double a0 = 1.0 + al / A;
    q->b0 = (1.0 + al * A) / a0; q->b1 = -2.0 * c / a0; q->b2 = (1.0 - al * A) / a0;
    q->a1 = -2.0 * c / a0; q->a2 = (1.0 - al / A) / a0; q->z1 = q->z2 = 0;
}

static LADSPA_Handle inst(const LADSPA_Descriptor *d, unsigned long fs) {
    (void)d;
    Inst *s = calloc(1, sizeof(Inst));
    if (!s) return NULL;
    s->pre = 1.0;
    const char *path = getenv("ROOMEQ_FILTERS"); if (!path) path = ROOMEQ_DEFAULT_FILTERS;
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof line, fp)) {
            double a, b, c;
            if (sscanf(line, "preamp %lf", &a) == 1) s->pre = pow(10.0, a / 20.0);
            else if (sscanf(line, "pk %lf %lf %lf", &a, &b, &c) == 3 && s->n < MAXF &&
                     a > 10 && a < fs * 0.45 && c > 0.1 && c < 50 && fabs(b) < 30)
                peaking(&s->f[s->n++], (double)fs, a, b, c);
        }
        fclose(fp);
    }
    return s;
}
static void conn(LADSPA_Handle h, unsigned long p, LADSPA_Data *d) {
    Inst *s = h; if (p == 0) s->in = d; else s->out = d;
}
static void act(LADSPA_Handle h) {
    Inst *s = h; for (int i = 0; i < s->n; i++) s->f[i].z1 = s->f[i].z2 = 0;
}
static void run(LADSPA_Handle h, unsigned long n) {
    Inst *s = h; const int nf = s->n; const double pre = s->pre;
    for (unsigned long i = 0; i < n; i++) {
        double x = s->in[i] * pre + 1e-25;
        for (int k = 0; k < nf; k++) {
            Biquad *q = &s->f[k];
            double y = q->b0 * x + q->z1;
            q->z1 = q->b1 * x - q->a1 * y + q->z2;
            q->z2 = q->b2 * x - q->a2 * y;
            x = y;
        }
        s->out[i] = (LADSPA_Data)x;
    }
}
static void cleanup(LADSPA_Handle h) { free(h); }

static const LADSPA_PortDescriptor pd[2] = { 0x1 | 0x8, 0x2 | 0x8 };
static const char *const pn[2] = { "Input", "Output" };
static const LADSPA_PortRangeHint ph[2] = { {0, 0, 0}, {0, 0, 0} };
static const LADSPA_Descriptor desc = {
    4242001, "roomeq_mono", 4 /* HARD_RT_CAPABLE */, "Room EQ (file driven)", "kodi-roomeq", "MIT",
    2, pd, pn, ph, NULL, inst, conn, act, run, NULL, NULL, NULL, cleanup };

const LADSPA_Descriptor *ladspa_descriptor(unsigned long i) { return i == 0 ? &desc : NULL; }
