/*
 * roomeq-tool: sweep generator, room response analyzer and parametric-EQ fitter.
 *
 *   roomeq-tool gen OUT.wav [channels]
 *       Write the measurement signal: exponential sine sweep 15 Hz..22 kHz, 8 s, -10 dBFS,
 *       48 kHz / 16-bit, on channels 1-2 (other channels silent).
 *
 *   roomeq-tool check REC.wav [mic_channel]
 *       Level/SNR/clipping check of one recording. Prints one JSON object.
 *
 *   roomeq-tool analyze OUTDIR [mic_channel] REC1.wav [REC2.wav ...]
 *       Room response from one or more recordings (positions around the listening spot),
 *       power-averaged, then fit up to N peaking filters in the low-frequency band.
 *       Writes OUTDIR/filters.txt (for the LADSPA plugin) and OUTDIR/report.json.
 *
 * Tuning (environment variables):
 *   RQ_FMAX        upper edge of the correction band, Hz         (default 500)
 *   RQ_BASS        target low-frequency lift, dB (below ~150 Hz) (default 2)
 *   RQ_MAXF        maximum number of filters                     (default 8)
 *   RQ_MAXBOOST    maximum boost, dB (0 = cuts only)             (default 3)
 *   RQ_BOOST_FMIN  no boosts below this frequency, Hz            (default 150)
 *   RQ_MIC_CAL     microphone calibration file ("freq dB" lines; REW/UMIK formats work)
 *
 * MIT License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define FS 48000
#define LEAD 0.5
#define SWEEP 8.0
#define TAIL 1.5
#define F1 15.0
#define F2 22000.0
#define LEVEL_DB (-10.0)
#define MAXREC 16
#define NG 400
#define MAXF 16

static double envd(const char *k, double d) { const char *v = getenv(k); return (v && *v) ? atof(v) : d; }

/* ------------------------------------------------------------------ FFT */
static void fft(double *re, double *im, int n, int inv) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / len * (inv ? 1 : -1), wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) {
                int a = i + j, b = i + j + len / 2;
                double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
    if (inv) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

/* ------------------------------------------------------------------ signal */
static double *make_signal(int *len) {
    int n = (int)((LEAD + SWEEP + TAIL) * FS);
    double *x = calloc(n, sizeof(double));
    double amp = pow(10, LEVEL_DB / 20), L = SWEEP / log(F2 / F1);
    int s0 = (int)(LEAD * FS), ns = (int)(SWEEP * FS);
    for (int i = 0; i < ns; i++) {
        double t = (double)i / FS, fade = 1;
        if (i < FS * 0.05) fade = 0.5 - 0.5 * cos(M_PI * i / (FS * 0.05));
        if (ns - i < FS * 0.02) fade = 0.5 - 0.5 * cos(M_PI * (ns - i) / (FS * 0.02));
        x[s0 + i] = amp * fade * sin(2 * M_PI * F1 * L * (exp(t / L) - 1));
    }
    *len = n;
    return x;
}

static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static int cmd_gen(const char *out, int ch) {
    if (ch < 1 || ch > 32) ch = 2;
    int n; double *x = make_signal(&n);
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite("RIFF", 1, 4, f); w32(f, 36 + n * ch * 2); fwrite("WAVEfmt ", 1, 8, f);
    w32(f, 16); w16(f, 1); w16(f, ch); w32(f, FS); w32(f, FS * ch * 2); w16(f, ch * 2); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, n * ch * 2);
    srand(1);
    for (int i = 0; i < n; i++) for (int c = 0; c < ch; c++) {
        double v = 0;  /* TPDF dither on the sweep channels */
        if (c < 2) v = x[i] * 32767.0 + ((double)rand() / RAND_MAX - (double)rand() / RAND_MAX);
        int s = (int)lrint(v); if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        w16(f, (uint16_t)(int16_t)s);
    }
    fclose(f); free(x);
    return 0;
}

/* ------------------------------------------------------------------ WAV reader (PCM 16/24/32, float32) */
static double *read_wav(const char *path, int chsel, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    char id[4]; uint32_t sz; int ch = 0, bits = 0, fmt = 0, rate = 0;
    fseek(f, 12, SEEK_SET);
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            uint16_t a = 0, c = 0, b = 0; uint32_t r = 0;
            if (fread(&a, 2, 1, f) != 1 || fread(&c, 2, 1, f) != 1 || fread(&r, 4, 1, f) != 1) break;
            fseek(f, 6, SEEK_CUR);
            if (fread(&b, 2, 1, f) != 1) break;
            fmt = a; ch = c; bits = b; rate = r;
            if (a == 0xFFFE && sz >= 26) {
                uint16_t sub = 0; fseek(f, 8, SEEK_CUR);
                if (fread(&sub, 2, 1, f) != 1) break;
                fmt = sub; fseek(f, sz - 26, SEEK_CUR);
            } else fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            if (rate != FS) { fprintf(stderr, "%s: sample rate %d, expected %d\n", path, rate, FS); break; }
            if (ch < 1 || bits < 16) { fprintf(stderr, "%s: unsupported format\n", path); break; }
            if (chsel >= ch || chsel < 0) chsel = 0;
            int bps = bits / 8, fr = ch * bps;
            if (sz == 0 || sz == 0xFFFFFFFF || sz > 0x7FFFFFFF) {  /* streamed WAV with no size */
                long p = ftell(f); fseek(f, 0, SEEK_END); sz = ftell(f) - p; fseek(f, p, SEEK_SET);
            }
            int n = sz / fr;
            unsigned char *buf = malloc((size_t)n * fr);
            n = fread(buf, fr, n, f);
            double *x = malloc(sizeof(double) * (n ? n : 1));
            for (int i = 0; i < n; i++) {
                unsigned char *p = buf + (size_t)i * fr + chsel * bps; double v = 0;
                if (fmt == 3 && bits == 32) { float fv; memcpy(&fv, p, 4); v = fv; }
                else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0;
                else if (bits == 24) v = ((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0;
                else if (bits == 32) v = ((int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24)) / 2147483648.0;
                x[i] = v;
            }
            free(buf); fclose(f); *len = n;
            return x;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f);
    fprintf(stderr, "%s: not a usable WAV file\n", path);
    return NULL;
}

/* ------------------------------------------------------------------ one recording */
typedef struct {
    double peak_dbfs, snr_db, delay_ms;
    int clipped;
    const char *error;          /* NULL when usable */
    double pw[NG];              /* smoothed power on the frequency grid */
} Meas;

static int ng; static double gf[NG];

static void make_grid(void) {
    ng = 0;
    for (double f = 20; f <= 20000 && ng < NG; f *= pow(2, 1.0 / 24)) gf[ng++] = f;
}

/* Blank the capture start-up transient and remove DC, then level/SNR checks.
 * Noise is taken from the tail (after sweep + decay): the very start of a USB capture
 * often contains a settling transient that would fake a high noise floor. */
static void level_check(double *rec, int nr, Meas *m) {
    int z = (int)(0.3 * FS); if (z > nr) z = nr;
    double mean = 0; int c = 0;
    for (int i = z; i < nr; i++) { mean += rec[i]; c++; }
    mean = c ? mean / c : 0;
    for (int i = 0; i < nr; i++) rec[i] = i < z ? 0 : rec[i] - mean;

    double pk = 0, nrms = 0, srms = 0; int clip = 0;
    for (int i = 0; i < nr; i++) { double a = fabs(rec[i]); if (a > pk) pk = a; if (a > 0.98) clip++; }
    int n0 = nr - (int)(0.8 * FS), n1 = nr;
    if (n0 < (int)((LEAD + SWEEP + 0.5) * FS)) { n0 = (int)(0.3 * FS); n1 = (int)(0.45 * FS); }
    if (n1 > nr) n1 = nr;
    if (n0 >= n1) n0 = n1 - 1 > 0 ? n1 - 1 : 0;
    for (int i = n0; i < n1; i++) nrms += rec[i] * rec[i];
    nrms = sqrt(nrms / (n1 - n0 > 0 ? n1 - n0 : 1) + 1e-30);
    int s0 = (int)((LEAD + 1) * FS), s1 = (int)((LEAD + SWEEP - 1) * FS);
    if (s1 > nr) s1 = nr;
    for (int i = s0; i < s1; i++) srms += rec[i] * rec[i];
    srms = sqrt(srms / (s1 - s0 > 0 ? s1 - s0 : 1) + 1e-30);
    m->peak_dbfs = 20 * log10(pk + 1e-12);
    m->snr_db = 20 * log10(srms / nrms);
    m->clipped = clip;
    m->error = NULL;
    if (clip > 10) m->error = "clipping: lower the mic gain or the volume";
    else if (m->peak_dbfs < -45) m->error = "signal too low: raise the mic gain / volume, check the mic and phantom power";
    else if (m->snr_db < 20) m->error = "SNR too low: room too noisy or the mic does not hear the sweep";
}

/* Deconvolve (H = R S* / (|S|^2 + eps)), window the impulse response, 1/6-octave smoothing. */
static void response(const double *rec, int nr, const double *sig, int ns, Meas *m) {
    int N = 1; while (N < (nr > ns ? nr : ns)) N <<= 1;
    double *Rr = calloc(N, 8), *Ri = calloc(N, 8), *Sr = calloc(N, 8), *Si = calloc(N, 8);
    memcpy(Rr, rec, sizeof(double) * nr); memcpy(Sr, sig, sizeof(double) * ns);
    fft(Rr, Ri, N, 0); fft(Sr, Si, N, 0);
    double smax = 0;
    for (int i = 0; i < N; i++) { double v = Sr[i] * Sr[i] + Si[i] * Si[i]; if (v > smax) smax = v; }
    double eps = smax * 1e-5;
    for (int i = 0; i < N; i++) {
        double d = Sr[i] * Sr[i] + Si[i] * Si[i] + eps;
        double hr = (Rr[i] * Sr[i] + Ri[i] * Si[i]) / d, hi = (Ri[i] * Sr[i] - Rr[i] * Si[i]) / d;
        Rr[i] = hr; Ri[i] = hi;
    }
    fft(Rr, Ri, N, 1);
    int pki = 0; double pv = 0;
    for (int i = 0; i < N; i++) if (fabs(Rr[i]) > pv) { pv = fabs(Rr[i]); pki = i; }
    m->delay_ms = 1000.0 * (pki > N / 2 ? pki - N : pki) / FS;

    /* window: -2 ms .. +300 ms (Hann edges) -> ~3 Hz resolution, enough for room modes */
    int M = 1; while (M < (int)(0.35 * FS)) M <<= 1; M <<= 1;
    double *wr = calloc(M, 8), *wi = calloc(M, 8);
    int pre = (int)(0.002 * FS), post = (int)(0.300 * FS), fo = (int)(0.100 * FS);
    for (int j = -pre; j < post; j++) {
        double w = 1;
        if (j < -pre / 2) w = 0.5 - 0.5 * cos(M_PI * (j + pre) / (pre / 2.0));
        if (j > post - fo) w = 0.5 + 0.5 * cos(M_PI * (j - (post - fo)) / fo);
        wr[j + pre] = w * Rr[((pki + j) % N + N) % N];
    }
    fft(wr, wi, M, 0);
    for (int g = 0; g < ng; g++) {
        double f = gf[g], lo = f * pow(2, -1.0 / 12), hi = f * pow(2, 1.0 / 12), s = 0; int c = 0;
        int i0 = (int)floor(lo * M / FS), i1 = (int)ceil(hi * M / FS);
        if (i1 - i0 < 2) { i0 = (int)(f * M / FS); i1 = i0 + 1; }
        for (int i = i0; i <= i1 && i < M / 2; i++) { s += wr[i] * wr[i] + wi[i] * wi[i]; c++; }
        m->pw[g] = s / (c ? c : 1) + 1e-30;
    }
    free(Rr); free(Ri); free(Sr); free(Si); free(wr); free(wi);
}

/* ------------------------------------------------------------------ mic calibration */
static void apply_cal(double *db, const char *path, int verbose) {
    if (!path || !*path) return;
    FILE *cf = fopen(path, "r");
    if (!cf) { if (verbose) fprintf(stderr, "mic calibration %s: cannot open\n", path); return; }
    static double cfq[4000], cdb[4000]; int nc = 0; char line[512];
    while (fgets(line, sizeof line, cf) && nc < 4000) {
        for (char *p = line; *p; p++) if (*p == ',' || *p == ';' || *p == '\t') *p = ' ';
        double a, b;
        if (sscanf(line, "%lf %lf", &a, &b) == 2 && a > 0 && (nc == 0 || a > cfq[nc - 1])) { cfq[nc] = a; cdb[nc] = b; nc++; }
    }
    fclose(cf);
    if (nc < 2) { if (verbose) fprintf(stderr, "mic calibration %s: no data\n", path); return; }
    for (int i = 0; i < ng; i++) {
        double c;
        if (gf[i] <= cfq[0]) c = cdb[0];
        else if (gf[i] >= cfq[nc - 1]) c = cdb[nc - 1];
        else {
            int k = 1; while (cfq[k] < gf[i]) k++;
            double t = log(gf[i] / cfq[k - 1]) / log(cfq[k] / cfq[k - 1]);
            c = cdb[k - 1] + t * (cdb[k] - cdb[k - 1]);
        }
        db[i] -= c;
    }
    if (verbose) fprintf(stderr, "mic calibration applied (%d points)\n", nc);
}

/* ------------------------------------------------------------------ PEQ fit */
typedef struct { double fc, g, q; } Peq;

static double peq_db(const Peq *p, double f) {
    double A = pow(10, p->g / 40), w0 = 2 * M_PI * p->fc / FS, al = sin(w0) / (2 * p->q), c = cos(w0);
    double b0 = 1 + al * A, b1 = -2 * c, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * c, a2 = 1 - al / A;
    double w = 2 * M_PI * f / FS, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

static double meas[NG], targ[NG];
static int b0i, b1i, np;
static Peq P[MAXF];
static double maxboost, boost_fmin;

static double eqdb(int i) { double s = 0; for (int k = 0; k < np; k++) s += peq_db(&P[k], gf[i]); return s; }

/* Asymmetric error: peaks above target cost fully, dips cost 35% (dips are usually
 * position-dependent cancellations that EQ cannot fill). Plus penalties that keep the
 * result safe: limited boost, no boost leaking below boost_fmin, no narrow boosts,
 * never boost the speaker's own roll-off. */
static double cost(void) {
    double c = 0, pen = 0;
    for (int i = b0i; i <= b1i; i++) {
        double q = eqdb(i), e = meas[i] + q - targ[i];
        c += e > 0 ? e * e : 0.35 * e * e;
        if (q > maxboost) pen += 10 * (q - maxboost) * (q - maxboost);
    }
    for (int i = b0i; i <= b1i && gf[i] < boost_fmin; i++) {
        double up = 0;
        for (int k = 0; k < np; k++) if (P[k].g > 0) up += peq_db(&P[k], gf[i]);
        if (up > 1.0) pen += 10 * (up - 1.0) * (up - 1.0);
    }
    c = (c + pen) / (b1i - b0i + 1);
    for (int k = 0; k < np; k++) if (P[k].g > 0) {
        c += 0.02 * P[k].g;
        if (P[k].q > 3) c += 0.5 * (P[k].q - 3) * (P[k].q - 3);
        if (P[k].fc < gf[b0i] * 1.3 || P[k].fc < boost_fmin) c += 5;
    }
    return c;
}

/* rms of the part above target in the band: what the listener hears as boom */
static double excess(void) {
    double c = 0;
    for (int i = b0i; i <= b1i; i++) { double e = meas[i] + eqdb(i) - targ[i]; if (e > 0) c += e * e; }
    return sqrt(c / (b1i - b0i + 1));
}

static void optimize(void) {
    double step[3] = { 0.06, 0.5, 0.25 };  /* log2 freq, dB, log2 Q */
    double best = cost(), flo = gf[b0i], fhi = gf[b1i];
    for (int it = 0; it < 60; it++) {
        int improved = 0;
        for (int k = 0; k < np; k++) for (int p = 0; p < 3; p++) for (int sgn = -1; sgn <= 1; sgn += 2) {
            Peq old = P[k];
            if (p == 0) { P[k].fc *= pow(2, sgn * step[0]); if (P[k].fc < flo || P[k].fc > fhi) { P[k] = old; continue; } }
            if (p == 1) { P[k].g += sgn * step[1]; if (P[k].g < -15 || P[k].g > maxboost) { P[k] = old; continue; } }
            if (p == 2) { P[k].q *= pow(2, sgn * step[2]); if (P[k].q < 0.5 || P[k].q > 12) { P[k] = old; continue; } }
            double c = cost();
            if (c < best - 1e-6) { best = c; improved = 1; } else P[k] = old;
        }
        if (!improved) { step[0] *= 0.5; step[1] *= 0.5; step[2] *= 0.5; if (step[1] < 0.05) break; }
    }
}

static void fit(int maxf) {
    static char tried[NG];
    memset(tried, 0, sizeof tried);
    np = 0;
    while (np < maxf) {
        int bp = -1, bd = -1; double ep = 1.0, ed = -2.5;
        for (int i = b0i; i <= b1i; i++) {
            if (tried[i]) continue;
            double e = meas[i] + eqdb(i) - targ[i];
            if (e > ep) { ep = e; bp = i; }
            if (e < ed && gf[i] >= boost_fmin) { ed = e; bd = i; }
        }
        if (maxboost <= 0) bd = -1;
        int cand[2] = { bp, bd }, done = 0;
        for (int ci = 0; ci < 2 && !done; ci++) {
            int bi = cand[ci]; if (bi < 0) continue;
            double be = meas[bi] + eqdb(bi) - targ[bi];
            int boost = be < 0;
            double half = be / 2; int l = bi, r = bi;
            while (l > b0i && fabs(meas[l] + eqdb(l) - targ[l]) > fabs(half)) l--;
            while (r < b1i && fabs(meas[r] + eqdb(r) - targ[r]) > fabs(half)) r++;
            double q = gf[bi] / (gf[r] - gf[l] + 1e-9); if (q < 0.7) q = 0.7; if (q > 10) q = 10;
            double g = -be; if (g < -15) g = -15; if (boost && g > maxboost) g = maxboost;
            Peq save[MAXF]; memcpy(save, P, sizeof P);
            double before = cost();
            P[np].fc = gf[bi]; P[np].g = g; P[np].q = boost && q > 3 ? 3 : q; np++;
            optimize();
            if (cost() < before * 0.95) done = 1;
            else { np--; memcpy(P, save, sizeof P); for (int k = bi - 3; k <= bi + 3; k++) if (k >= 0 && k < NG) tried[k] = 1; }
        }
        if (!done && bp < 0 && bd < 0) break;
        if (sqrt(cost()) < 0.7) break;
    }
    for (int k = 0; k < np; k++) if (fabs(P[k].g) < 0.5) { P[k] = P[--np]; k--; }
}

/* ------------------------------------------------------------------ commands */
static int load(const char *path, int micch, const double *sig, int ns, Meas *m, int full) {
    int nr; double *rec = read_wav(path, micch - 1, &nr);
    if (!rec) { m->error = "cannot read recording"; return -1; }
    level_check(rec, nr, m);
    if (full && !m->error) response(rec, nr, sig, ns, m);
    free(rec);
    return 0;
}

static int cmd_check(const char *path, int micch) {
    Meas m; memset(&m, 0, sizeof m);
    load(path, micch, NULL, 0, &m, 0);
    printf("{\"ok\":%s,\"error\":\"%s\",\"peak_dbfs\":%.1f,\"snr_db\":%.1f,\"clipped\":%d}\n",
           m.error ? "false" : "true", m.error ? m.error : "", m.peak_dbfs, m.snr_db, m.clipped);
    return m.error ? 2 : 0;
}

static int cmd_analyze(const char *outdir, int micch, int nrec, char **recs) {
    make_grid();
    int ns; double *sig = make_signal(&ns);
    static Meas M[MAXREC];
    if (nrec > MAXREC) nrec = MAXREC;
    int good = 0;
    for (int r = 0; r < nrec; r++) {
        memset(&M[r], 0, sizeof M[r]);
        load(recs[r], micch, sig, ns, &M[r], 1);
        fprintf(stderr, "point %d: peak %.1f dBFS, SNR %.1f dB, delay %.1f ms%s%s\n", r + 1, M[r].peak_dbfs,
                M[r].snr_db, M[r].delay_ms, M[r].error ? " -- SKIPPED: " : "", M[r].error ? M[r].error : "");
        if (!M[r].error) good++;
    }
    const char *err = good ? NULL : (nrec == 1 && M[0].error ? M[0].error : "no usable recordings");

    /* per-point curves, each normalised to its own 200..2000 Hz level; power average */
    static double pdb[MAXREC][NG], spread[NG];
    double avg[NG] = { 0 };
    int calmsg = 0;
    for (int r = 0; r < nrec; r++) {
        if (M[r].error) continue;
        for (int i = 0; i < ng; i++) pdb[r][i] = 10 * log10(M[r].pw[i]);
        apply_cal(pdb[r], getenv("RQ_MIC_CAL"), !calmsg);
        calmsg = 1;
        double ref = 0; int rc = 0;
        for (int i = 0; i < ng; i++) if (gf[i] >= 200 && gf[i] <= 2000) { ref += pow(10, pdb[r][i] / 10); rc++; }
        ref = 10 * log10(ref / rc);
        for (int i = 0; i < ng; i++) { pdb[r][i] -= ref; avg[i] += pow(10, pdb[r][i] / 10); }
    }
    for (int i = 0; i < ng; i++) {
        meas[i] = good ? 10 * log10(avg[i] / good) : 0;
        double s = 0;
        for (int r = 0; r < nrec; r++) if (!M[r].error) s += (pdb[r][i] - meas[i]) * (pdb[r][i] - meas[i]);
        spread[i] = good > 1 ? sqrt(s / (good - 1)) : 0;
    }

    double fmax = envd("RQ_FMAX", 500), bass = envd("RQ_BASS", 2);
    int maxf = (int)envd("RQ_MAXF", 8); if (maxf > MAXF) maxf = MAXF; if (maxf < 1) maxf = 1;
    maxboost = envd("RQ_MAXBOOST", 3);
    boost_fmin = envd("RQ_BOOST_FMIN", 150);
    for (int i = 0; i < ng; i++) {
        double t = log2(150.0 / gf[i]) / log2(150.0 / 30.0);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        targ[i] = bass * t;
    }
    double flo = 25;  /* lowest frequency where the speaker still delivers (within 6 dB of target) */
    for (int i = 0; i < ng; i++) if (gf[i] >= 20 && meas[i] - targ[i] > -6) { flo = gf[i]; break; }
    if (flo < 25) flo = 25;
    if (flo > fmax / 2) flo = fmax / 2;
    b0i = 0; while (b0i < ng - 1 && gf[b0i] < flo) b0i++;
    b1i = b0i; while (b1i < ng - 1 && gf[b1i + 1] <= fmax) b1i++;

    np = 0;
    double c0 = good ? cost() : 0, c1 = c0, x0 = good ? excess() : 0, x1 = x0, preamp = 0;
    if (good) {
        fit(maxf);
        c1 = cost();
        x1 = excess();
        double maxeq = 0;
        for (double f = 20; f < 20000; f *= 1.02) {
            double s = 0; for (int k = 0; k < np; k++) s += peq_db(&P[k], f);
            if (s > maxeq) maxeq = s;
        }
        preamp = maxeq > 0 ? -(maxeq + 0.5) : 0;
    }
    fprintf(stderr, "%d/%d points used, band %.0f..%.0f Hz, peaks above target %.2f -> %.2f dB rms, %d filters, preamp %.1f dB\n",
            good, nrec, gf[b0i], gf[b1i], x0, x1, np, preamp);

    char path[1024];
    if (!err) {
        snprintf(path, sizeof path, "%s/filters.txt", outdir);
        FILE *o = fopen(path, "w");
        if (!o) { perror(path); return 1; }
        fprintf(o, "# roomeq: %d point(s), band %.0f-%.0f Hz\npreamp %.2f\n", good, gf[b0i], gf[b1i], preamp);
        for (int k = 0; k < np; k++) fprintf(o, "pk %.2f %.3f %.3f\n", P[k].fc, P[k].g, P[k].q);
        fclose(o);
    } else fprintf(stderr, "ERROR: %s\n", err);

    snprintf(path, sizeof path, "%s/report.json", outdir);
    FILE *j = fopen(path, "w");
    if (!j) { perror(path); return 1; }
    fprintf(j, "{\"ok\":%s,\"error\":\"%s\",\"used\":%d,\"band\":[%.0f,%.0f],\"preamp\":%.2f,"
               "\"rms_before\":%.2f,\"rms_after\":%.2f,\"excess_before\":%.2f,\"excess_after\":%.2f,\"filters\":[",
            err ? "false" : "true", err ? err : "", good, gf[b0i], gf[b1i], preamp, sqrt(c0), sqrt(c1), x0, x1);
    for (int k = 0; k < np; k++) fprintf(j, "%s{\"fc\":%.1f,\"g\":%.2f,\"q\":%.2f}", k ? "," : "", P[k].fc, P[k].g, P[k].q);
    fprintf(j, "],\"points\":[");
    for (int r = 0; r < nrec; r++)
        fprintf(j, "%s{\"ok\":%s,\"error\":\"%s\",\"peak_dbfs\":%.1f,\"snr_db\":%.1f,\"delay_ms\":%.1f}", r ? "," : "",
                M[r].error ? "false" : "true", M[r].error ? M[r].error : "", M[r].peak_dbfs, M[r].snr_db, M[r].delay_ms);
#define ARR(name, expr) do { fprintf(j, "],\"" name "\":["); for (int i = 0; i < ng; i++) fprintf(j, "%s%.2f", i ? "," : "", (expr)); } while (0)
    fprintf(j, "],\"f\":[");
    for (int i = 0; i < ng; i++) fprintf(j, "%s%.1f", i ? "," : "", gf[i]);
    ARR("meas", meas[i]);
    ARR("corr", meas[i] + eqdb(i));
    ARR("target", targ[i]);
    ARR("spread", spread[i]);
    fprintf(j, "],\"curves\":[");
    int first = 1;
    for (int r = 0; r < nrec; r++) {
        if (M[r].error) continue;
        fprintf(j, "%s[", first ? "" : ","); first = 0;
        for (int i = 0; i < ng; i++) fprintf(j, "%s%.2f", i ? "," : "", pdb[r][i]);
        fprintf(j, "]");
    }
    fprintf(j, "]}\n");
    fclose(j);
    free(sig);
    return err ? 2 : 0;
}

static int usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s gen OUT.wav [channels]\n"
            "  %s check REC.wav [mic_channel]\n"
            "  %s analyze OUTDIR [mic_channel] REC1.wav [REC2.wav ...]\n", a0, a0, a0);
    return 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "gen")) return cmd_gen(argv[2], argc > 3 ? atoi(argv[3]) : 2);
    if (argc >= 3 && !strcmp(argv[1], "check")) return cmd_check(argv[2], argc > 3 ? atoi(argv[3]) : 1);
    if (argc >= 4 && !strcmp(argv[1], "analyze")) {
        int micch = 1, i = 3;
        char *end;
        long v = strtol(argv[3], &end, 10);
        if (*end == 0 && v > 0 && v < 64) { micch = (int)v; i = 4; }
        if (i >= argc) return usage(argv[0]);
        return cmd_analyze(argv[2], micch, argc - i, argv + i);
    }
    return usage(argv[0]);
}
