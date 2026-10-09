#!/usr/bin/env python3
"""End-to-end test on a simulated room. Needs numpy + scipy.

Builds the measurement sweep with roomeq-tool, "plays" it through a synthetic room
(speaker roll-off, a few room modes whose strength depends on the listening position,
one early reflection, latency, noise, DC offset and a capture start-up click), records it
at several positions, runs the analyzer on all of them, then checks that:

  * every recording passes the level check;
  * the fitted filters cut the strongest mode;
  * the correction lowers the deviation at every position, not only on the average;
  * no boost is placed below RQ_BOOST_FMIN;
  * the LADSPA-style biquad chain built from filters.txt matches the predicted curve.

Usage: tests/simulate.py path/to/roomeq-tool
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
import scipy.io.wavfile as wav
import scipy.signal as ss

FS = 48000
TOOL = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/roomeq-tool')


def peaking(f0, g, q):
    a = 10 ** (g / 40); w0 = 2 * np.pi * f0 / FS; al = np.sin(w0) / (2 * q); c = np.cos(w0)
    b = np.array([1 + al * a, -2 * c, 1 - al * a]); aa = np.array([1 + al / a, -2 * c, 1 - al / a])
    return b / aa[0], aa / aa[0]


# modes: (freq, gain at position 0..2, Q)
MODES = [(48, (9, 7, 10), 6), (95, (-7, -3, -9), 4), (186, (8, 6, 9), 8), (260, (-4, -2, -5), 2)]


def room(x, pos, rng):
    y = x.copy()
    for f0, gains, q in MODES:
        b, a = peaking(f0, gains[pos], q)
        y = ss.lfilter(b, a, y)
    b, a = ss.butter(2, 35 / (FS / 2), 'high')          # speaker roll-off
    y = ss.lfilter(b, a, y)
    d = int((0.003 + 0.001 * pos) * FS)                  # floor bounce, position dependent
    y[d:] += 0.35 * y[:-d]
    lat = 700 + 37 * pos                                  # playback/capture latency
    y = np.concatenate([np.zeros(lat), y])[:len(x) + 24000]
    y = y * 0.3 + rng.normal(0, 3e-4, len(y)) + 0.002    # gain, noise, DC
    y[:300] += np.hanning(300) * 0.05                     # capture start-up click
    return y


def main():
    rng = np.random.default_rng(1)
    with tempfile.TemporaryDirectory() as d:
        sweep = os.path.join(d, 'sweep.wav')
        subprocess.run([TOOL, 'gen', sweep, '2'], check=True)
        fs, x = wav.read(sweep)
        assert fs == FS
        x = x[:, 0].astype(float) / 32768
        recs = []
        for pos in range(3):
            y = room(x, pos, rng)
            stereo = np.stack([rng.normal(0, 1e-5, len(y)), y], 1)   # mic on channel 2
            p = os.path.join(d, 'rec%d.wav' % pos)
            wav.write(p, FS, (np.clip(stereo, -1, 1) * (2 ** 31 - 1)).astype(np.int32))
            chk = json.loads(subprocess.run([TOOL, 'check', p, '2'], check=True,
                                            capture_output=True, text=True).stdout)
            assert chk['ok'], chk
            recs.append(p)
        env = dict(os.environ, RQ_MAXBOOST='3', RQ_BOOST_FMIN='150')
        r = subprocess.run([TOOL, 'analyze', d, '2'] + recs, env=env, capture_output=True, text=True)
        print(r.stderr.strip())
        assert r.returncode == 0, r.returncode
        rep = json.load(open(os.path.join(d, 'report.json')))
        if os.environ.get('KEEP_REPORT'):  # used to render docs/screenshot.png
            import shutil
            shutil.copy(os.path.join(d, 'report.json'), os.environ['KEEP_REPORT'])
        filt = open(os.path.join(d, 'filters.txt')).read()
        print(filt.strip())

        f = np.array(rep['f']); band = (f >= rep['band'][0]) & (f <= rep['band'][1])
        assert rep['used'] == 3 and len(rep['curves']) == 3
        # dips below the boost floor are left alone by design, so judge the peaks
        assert rep['excess_after'] < rep['excess_before'] * 0.5, (rep['excess_before'], rep['excess_after'])

        cuts = [x for x in rep['filters'] if x['g'] < -3]
        assert any(40 <= x['fc'] <= 56 for x in cuts), 'the 48 Hz mode was not cut'
        assert any(165 <= x['fc'] <= 210 for x in cuts), 'the 186 Hz mode was not cut'
        for x in rep['filters']:
            assert not (x['g'] > 0 and x['fc'] < 150), 'boost below RQ_BOOST_FMIN: %s' % x

        # the same correction must help at every individual position
        eq = np.array(rep['corr']) - np.array(rep['meas'])
        tg = np.array(rep['target'])
        for i, c in enumerate(rep['curves']):
            c = np.array(c)
            before = np.sqrt(np.mean(np.maximum(c - tg, 0)[band] ** 2))
            after = np.sqrt(np.mean(np.maximum(c + eq - tg, 0)[band] ** 2))
            print('position %d: peak excess %.2f -> %.2f dB' % (i + 1, before, after))
            assert after < before, 'correction made position %d worse' % (i + 1)

        # biquad chain from filters.txt (what the LADSPA plugin runs) vs. predicted curve
        pre = 0.0; chain = []
        for line in filt.splitlines():
            t = line.split()
            if t and t[0] == 'preamp':
                pre = float(t[1])
            elif t and t[0] == 'pk':
                chain.append(peaking(float(t[1]), float(t[2]), float(t[3])))
        h = np.ones(len(f), complex)
        for b, a in chain:
            _, hh = ss.freqz(b, a, worN=f, fs=FS); h *= hh
        got = 20 * np.log10(np.abs(h))
        assert np.max(np.abs(got - eq)) < 0.06, np.max(np.abs(got - eq))  # report is rounded to 0.01 dB
        assert pre <= 0
    print('OK')


if __name__ == '__main__':
    main()
