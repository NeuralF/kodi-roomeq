# Implementation notes

## Kodi and ALSA

- Kodi opens ALSA PCMs with `SND_PCM_NO_AUTO_FORMAT | NO_AUTO_CHANNELS | NO_AUTO_RESAMPLE`. A LADSPA plugin works on floats, so a chain of only `plug → ladspa → hw` is not offered to Kodi at all, or fails to open. The chain that works is:

  ```
  plug → ladspa (channels = card's playback channels) → lfloat (S32_LE) → hw
  ```

  The outer `plug` handles Kodi's format and channel count. `lfloat` converts float back to the card's integer format.
- The ALSA LADSPA plugin, with `policy duplicate`, runs one mono instance per channel. Each instance reads the filter file when it is instantiated, which happens when Kodi opens the device. So `apply` re-opens the output: it switches to `roomeq_direct` for a second and then back.
- Kodi stores the device as `ALSA:<pcm>|<description>`. Compare only the part before `|`. On start-up Kodi falls back to the default device if the stored one does not exist. That is why `setup` runs at boot and on USB hot-plug, before Kodi starts.
- With `audiooutput.streamsilence` set to anything other than 0, Kodi keeps the card open forever and `arecord`/`aplay` on the same hw device get `EBUSY`. The installer sets it to 0. `roomeq.sh measure` also waits for the device and stops Kodi's players if needed.

## Behringer UMC204HD (and UMC202HD/404HD, same firmware family)

- Playback has 4 channels and two USB alternate settings:
  - altset 1: 24-in-32-bit, implicit feedback;
  - altset 2: 16-bit. With the Linux driver, altset 2 produces audible crackle and digital distortion on sweeps, while music sounds almost fine because it masks the problem.
  Forcing `S32_LE` on the hw device selects altset 1. This is the `FORMAT` setting.
- After every cold boot, the ALSA `Mic Capture Volume` control comes up at 0, which is −127 dB. Recordings are then digital silence even with the gain knob all the way up. `roomeq.sh` raises any capture control sitting at 0% before recording.
- The card's ALSA id is `U192k`.
- Input 1 and input 2 are separate channels of one stereo capture stream. `MIC_CH` picks the channel.

## Measurement

- Sweep: exponential, 15 Hz-22 kHz, 8 s, −10 dBFS with TPDF dither, played on output channels 1-2 (both speakers at once, other channels silent), with 0.5 s of lead-in and 1.5 s of tail. The sweep runs at 48 kHz and the tool is fixed at that rate.
- The impulse response comes from regularised FFT deconvolution. It is windowed from −2 ms to +300 ms around the peak, which is enough for modal behaviour without letting late noise in, and then smoothed to 1/6 octave.
- Level check: the first 0.3 s are ignored (USB start-up clicks) and DC is removed. Noise is measured over the last 0.8 s.
  - Rejected: more than 10 clipped samples, a peak below −45 dBFS, or SNR below 20 dB.
- Multi-point: each curve is normalised to its own 200-2000 Hz mean, and the points are power-averaged. The UI also reports the spread between points.

## Fitting

- Greedy placement, then coordinate descent over frequency, gain and Q for all filters together. The error is asymmetric: excess above the target counts fully, deficits count at 35%. Dips are usually position-dependent nulls, and boosting into them wastes headroom.
- The lower band edge is the lowest frequency where the system is within 6 dB of the target. The speaker's natural roll-off below that edge is never boosted.
- Boosts are limited three ways:
  - maximum gain (`RQ_MAXBOOST`);
  - minimum bandwidth (a boost with Q above 3 is penalised);
  - a floor (`RQ_BOOST_FMIN`), enforced with a penalty on any boost energy that leaks below it.
- Preamp equals minus (the largest positive gain of the combined curve + 0.5 dB), so the EQ itself never pushes a full-scale signal into clipping.
- `make test` builds a synthetic room with modes at 48, 95, 186 and 260 Hz that vary between three positions, plus latency, noise, DC and a start-up click. It checks that the fit removes most of the excess at every position, that the biquad implementation matches the predicted curve within 0.06 dB, and that the boost rules hold.

## Mics

- A measurement mic with a calibration file is the reference. A dynamic vocal mic (SM58, AKG D5, ...) has a fairly smooth response between 150 Hz and 1 kHz but rolls off below about 100 Hz. Keep `RQ_BOOST_FMIN` at 150 with such a mic: cuts are still valid, and modal peaks are far larger than the mic's own deviations.
- Since the fit mostly cuts, a mic error usually means a filter that does too little, not one that does harm.
