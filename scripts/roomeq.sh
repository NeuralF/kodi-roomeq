#!/bin/sh
# kodi-roomeq control script.
#
#   roomeq.sh setup        write the ALSA config (run at boot and on USB hot-plug)
#   roomeq.sh on | off     switch Kodi's audio output to the EQ'd / direct device
#   roomeq.sh new          start a new measurement session (forget recorded points)
#   roomeq.sh measure      record one point (sweep + mic), update the preview
#   roomeq.sh undo         drop the last recorded point, update the preview
#   roomeq.sh apply        make the previewed filters active and switch the EQ on
#   roomeq.sh quick        new + measure + apply (single-point shortcut)
#   roomeq.sh reset        remove the active filters (EQ becomes transparent)
#   roomeq.sh status       print a JSON status line
#
# Configuration: $ROOMEQ_CONF (default /storage/.config/roomeq/roomeq.conf), shell syntax:
#   CARD=auto        ALSA card id (see /proc/asound/cards) or "auto" = first USB card with an input
#   FORMAT=S32_LE    sample format forced on the card ("" = let ALSA choose)
#   MIC_CH=1         input channel the measurement mic is plugged into
#   RQ_FMAX=500  RQ_BASS=2  RQ_MAXF=8  RQ_MAXBOOST=3  RQ_BOOST_FMIN=150   analyzer tuning
#   RQ_MIC_CAL=/path/to/mic.cal                                          optional calibration
# MIT License.

BASE=${ROOMEQ_BASE:-$(cd "$(dirname "$0")" && pwd)}
CFG=${ROOMEQ_DIR:-/storage/.config/roomeq}
CONF=${ROOMEQ_CONF:-$CFG/roomeq.conf}
ASOUND=${ROOMEQ_ASOUND:-/storage/.config/asound.conf}
WEB=${ROOMEQ_WEBDATA:-/storage/.kodi/addons/webinterface.roomeq/data}
KODI_RPC=${KODI_RPC:-http://127.0.0.1:8080/jsonrpc}
TOOL=$BASE/roomeq-tool
SESSION=$CFG/session
DEV_EQ="ALSA:roomeq"
DEV_DIRECT="ALSA:roomeq_direct"

CARD=auto; FORMAT=S32_LE; MIC_CH=1
mkdir -p "$CFG" "$SESSION"
# shellcheck source=/dev/null
[ -f "$CONF" ] && . "$CONF"
export RQ_FMAX RQ_BASS RQ_MAXF RQ_MAXBOOST RQ_BOOST_FMIN RQ_MIC_CAL

# ---------------------------------------------------------------- helpers
rpc() { curl -s -m15 -X POST -H 'Content-Type: application/json' -d "$1" "$KODI_RPC" 2>/dev/null; }
kodi_up() { rpc '{"jsonrpc":"2.0","method":"JSONRPC.Ping","id":1}' | grep -q pong; }
setdev() { rpc "{\"jsonrpc\":\"2.0\",\"method\":\"Settings.SetSettingValue\",\"params\":{\"setting\":\"audiooutput.audiodevice\",\"value\":\"$1\"},\"id\":1}"; }
getdev() { rpc '{"jsonrpc":"2.0","method":"Settings.GetSettingValue","params":{"setting":"audiooutput.audiodevice"},"id":1}' | sed -n 's/.*"value":"\([^"|]*\).*/\1/p'; }
jstr() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
publish() { mkdir -p "$WEB" 2>/dev/null && cp "$1" "$WEB/$(basename "$1")" 2>/dev/null; :; }
state() {  # state <name> <message>
    printf '{"state":"%s","msg":"%s","time":%s,"points":%s}\n' "$1" "$(jstr "$2")" "$(date +%s)" "$(npoints)" > "$CFG/state.json"
    publish "$CFG/state.json"
}
# shellcheck disable=SC2012
npoints() { ls "$SESSION"/p*.wav 2>/dev/null | wc -l; }

card_id() {
    if [ "$CARD" != "auto" ] && [ -n "$CARD" ]; then echo "$CARD"; return; fi
    for c in /proc/asound/card*; do
        [ -f "$c/stream0" ] && grep -q '^Capture:' "$c/stream0" && { cat "$c/id"; return; }
    done
    for c in /proc/asound/card*; do [ -f "$c/stream0" ] && { cat "$c/id"; return; }; done
}
chans() {  # chans <card> Playback|Capture
    awk -v s="$2" '$1==s":"{f=1} f&&/Channels:/{print $2; exit}' "/proc/asound/$1/stream0" 2>/dev/null
}
# short card name: " 1 [U192k          ]: USB-Audio - UMC204HD 192k" -> "UMC204HD 192k"
cardname() { awk -v id="$1" -F'[][]' '{gsub(/ /,"",$2)} $2==id {sub(/^: *[^ ]+ - /,"",$3); print $3; exit}' /proc/asound/cards; }

# Some USB interfaces (e.g. Behringer UMC series) expose a digital capture gain that the
# driver initialises to its minimum (-127 dB): the mic then records digital silence.
# Raise any capture volume that sits at exactly 0.
mixfix() {
    [ -n "$1" ] || return 0
    amixer -c "$1" scontrols 2>/dev/null | sed -n "s/^Simple mixer control '\(.*\)',\([0-9]*\)$/\1,\2/p" | while IFS= read -r ctl; do
        info=$(amixer -c "$1" sget "$ctl" 2>/dev/null)
        echo "$info" | grep -q 'Capabilities:.*cvolume' || continue
        echo "$info" | grep -q 'Capture 0 \[0%\]' && amixer -q -c "$1" sset "$ctl" 100% cap 2>/dev/null
    done
    :
}

wait_released() {  # wait until nobody holds the playback device
    for _ in $(seq 1 20); do grep -q closed "/proc/asound/$1/pcm0p/sub0/status" 2>/dev/null && return 0; sleep 0.5; done
    if kodi_up; then
        rpc '{"jsonrpc":"2.0","method":"Player.Stop","params":{"playerid":0},"id":1}' >/dev/null
        rpc '{"jsonrpc":"2.0","method":"Player.Stop","params":{"playerid":1},"id":1}' >/dev/null
        # right after Kodi starts it can hold the card for up to a minute
        for _ in $(seq 1 90); do grep -q closed "/proc/asound/$1/pcm0p/sub0/status" 2>/dev/null && return 0; sleep 0.5; done
    fi
    return 1
}

# ---------------------------------------------------------------- commands
setup() {
    ID=$(card_id); PCH=""
    [ -n "$ID" ] && PCH=$(chans "$ID" Playback)
    [ -n "$ID" ] || { echo "no USB audio card found; writing config for card 'U192k'"; ID=U192k; }
    [ -n "$PCH" ] || PCH=2
    NAME=$(cardname "$ID"); [ -n "$NAME" ] || NAME=$ID
    FMT=""; [ -n "$FORMAT" ] && FMT="format $FORMAT"
    LF_FMT=${FORMAT:-S32_LE}
    cat > "$ASOUND" <<EOF
# generated by $BASE/roomeq.sh setup; edit $CONF and re-run instead
pcm.roomeq_direct {
  type plug
  slave { pcm "hw:CARD=$ID,DEV=0" channels $PCH $FMT }
  ttable.0.0 1
  ttable.1.1 1
  hint { show on description "$NAME (direct)" }
}
pcm.roomeq {
  type plug
  slave.pcm "roomeq_ladspa"
  hint { show on description "$NAME + Room EQ" }
}
pcm.roomeq_ladspa {
  type ladspa
  slave.pcm "roomeq_float"
  path "$BASE/ladspa"
  channels $PCH
  plugins [ { label roomeq_mono input { } } ]
}
pcm.roomeq_float {
  type lfloat
  slave { pcm "hw:CARD=$ID,DEV=0" format $LF_FMT }
}
EOF
    mixfix "$ID"
    echo "asound.conf written: card=$ID ($NAME), playback channels=$PCH, format=${FORMAT:-auto}"
}

preview() {
    set -- "$SESSION"/p*.wav
    if [ ! -f "$1" ]; then rm -f "$SESSION/report.json" "$WEB/report.json"; return 0; fi
    "$TOOL" analyze "$SESSION" "$MIC_CH" "$@" > "$SESSION/analyze.log" 2>&1
    RC=$?
    publish "$SESSION/report.json"
    return $RC
}

measure() {
    ID=$(card_id)
    if [ -z "$ID" ] || [ ! -f "/proc/asound/$ID/stream0" ]; then state error "audio interface not found"; return 1; fi
    PCH=$(chans "$ID" Playback); CCH=$(chans "$ID" Capture)
    [ -n "$CCH" ] || { state error "the interface has no input"; return 1; }
    [ "$MIC_CH" -le "$CCH" ] 2>/dev/null || { state error "MIC_CH=$MIC_CH but the interface has $CCH inputs"; return 1; }
    mixfix "$ID"
    state measuring "sweep running - keep quiet"
    wait_released "$ID" || { state error "the audio device is busy: another app is playing; try again in a minute"; return 1; }
    W=/tmp/roomeq; rm -rf "$W"; mkdir -p "$W"
    "$TOOL" gen "$W/sweep.wav" "${PCH:-2}" >/dev/null
    N=$(( $(npoints) + 1 )); OUT=$(printf '%s/p%02d.wav' "$SESSION" "$N")
    REC=arecord; [ -x "$BASE/arecord" ] && REC="$BASE/arecord"
    $REC -q -D "plughw:CARD=$ID,DEV=0" -f S32_LE -r 48000 -c "$CCH" -d 11 "$W/rec.wav" 2>"$W/arecord.err" &
    RP=$!
    sleep 0.5
    aplay -q -D roomeq_direct "$W/sweep.wav" 2>"$W/aplay.err"
    wait $RP
    CHK=$("$TOOL" check "$W/rec.wav" "$MIC_CH")
    if echo "$CHK" | grep -q '"ok":true'; then
        mv "$W/rec.wav" "$OUT"
        preview
        state recorded "point $N recorded"
    else
        MSG=$(echo "$CHK" | sed -n 's/.*"error":"\([^"]*\)".*/\1/p')
        [ -n "$MSG" ] || MSG="recording failed: $(cat "$W/arecord.err" "$W/aplay.err" | head -2 | tr '\n' ' ')"
        state error "$MSG"
        echo "$CHK"; return 1
    fi
    cat "$SESSION/analyze.log"
}

apply() {
    if [ ! -f "$SESSION/filters.txt" ] || ! grep -q '"ok":true' "$SESSION/report.json" 2>/dev/null; then
        state error "nothing to apply: record at least one good point"; return 1
    fi
    cp "$SESSION/filters.txt" "$CFG/filters.txt"
    cp "$SESSION/report.json" "$CFG/report.active.json"
    publish "$CFG/report.active.json"
    state applied "$(grep -c '^pk' "$CFG/filters.txt") filters active"
    kodi_up && reopen_eq
}

reopen_eq() {  # make Kodi reopen the device so the plugin reloads the filter file
    if [ "$(getdev)" = "$DEV_EQ" ]; then setdev "$DEV_DIRECT" >/dev/null; sleep 1; fi
    setdev "$DEV_EQ" >/dev/null
}

case "$1" in
    setup) setup ;;
    on) setdev "$DEV_EQ"; echo ;;
    off) setdev "$DEV_DIRECT"; echo ;;
    new) rm -f "$SESSION"/p*.wav "$SESSION"/report.json "$SESSION"/filters.txt "$WEB/report.json"; state idle "new session" ;;
    measure) measure ;;
    undo) LAST=$(find "$SESSION" -name 'p*.wav' 2>/dev/null | sort | tail -1); [ -n "$LAST" ] && rm -f "$LAST"
          rm -f "$SESSION/filters.txt"; preview; state idle "last point removed" ;;
    apply) apply ;;
    quick) "$0" new >/dev/null && "$0" measure && "$0" apply ;;
    reset) rm -f "$CFG/filters.txt" "$CFG/report.active.json" "$WEB/report.active.json"; state idle "filters removed"
           kodi_up && [ "$(getdev)" = "$DEV_EQ" ] && reopen_eq; : ;;
    status)
        ID=$(card_id); CCH=$(chans "$ID" Capture); [ -n "$CCH" ] || CCH=0
        printf '{"card":"%s","name":"%s","inputs":%s,"device":"%s","filters":%s,"points":%s,"mic_ch":%s}\n' \
            "$ID" "$(jstr "$(cardname "$ID")")" "$CCH" "$(getdev)" \
            "$([ -f "$CFG/filters.txt" ] && echo true || echo false)" "$(npoints)" "$MIC_CH" ;;
    *) sed -n '2,24p' "$0"; exit 1 ;;
esac
