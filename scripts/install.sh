#!/bin/sh
# Install kodi-roomeq on LibreELEC (run as root on the box, from the unpacked release).
#   sh scripts/install.sh            install / update
#   sh scripts/install.sh uninstall  remove everything (keeps measurements in /storage/.config/roomeq)
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
DEST=/storage/roomeq
ADDONS=/storage/.kodi/addons
AUTOSTART=/storage/.config/autostart.sh
UDEV=/storage/.config/udev.rules.d/90-roomeq.rules
KODI_RPC=http://127.0.0.1:8080/jsonrpc
rpc() { curl -s -m10 -X POST -H 'Content-Type: application/json' -d "$1" "$KODI_RPC" 2>/dev/null; }

if [ "$1" = "uninstall" ]; then
    rpc '{"jsonrpc":"2.0","method":"Addons.SetAddonEnabled","params":{"addonid":"webinterface.roomeq","enabled":false},"id":1}' >/dev/null || true
    rpc '{"jsonrpc":"2.0","method":"Addons.SetAddonEnabled","params":{"addonid":"script.roomeq","enabled":false},"id":1}' >/dev/null || true
    rm -rf "$DEST" "$ADDONS/webinterface.roomeq" "$ADDONS/script.roomeq" "$UDEV"
    [ -f "$AUTOSTART" ] && sed -i '/roomeq.sh setup/d' "$AUTOSTART"
    echo "removed. /storage/.config/asound.conf was left in place: delete it if nothing else needs it."
    exit 0
fi

case "$(uname -m)" in
    armv7*|armv8l) ARCH=armhf ;;
    aarch64) ARCH=aarch64 ;;
    *) echo "unsupported CPU $(uname -m): build with 'make' and copy build/native/* to $DEST"; exit 1 ;;
esac
BIN="$SRC/bin/$ARCH"; [ -d "$BIN" ] || BIN="$SRC/build/$ARCH"
[ -x "$BIN/roomeq-tool" ] || { echo "no binaries for $ARCH in $SRC/bin or $SRC/build"; exit 1; }
# a 64-bit kernel can run a 32-bit LibreELEC userland: trust the loader we actually have
[ "$ARCH" = aarch64 ] && [ ! -e /lib/ld-linux-aarch64.so.1 ] && [ -e /lib/ld-linux-armhf.so.3 ] && BIN="$SRC/bin/armhf"

mkdir -p "$DEST/ladspa"
cp "$BIN/roomeq-tool" "$DEST/"
cp "$BIN/roomeq.so" "$DEST/ladspa/"
cp "$SRC/scripts/roomeq.sh" "$DEST/"
chmod +x "$DEST/roomeq-tool" "$DEST/roomeq.sh"
# LibreELEC ships aplay but not arecord: they are the same binary, picked by name
command -v arecord >/dev/null 2>&1 || ln -sf "$(command -v aplay)" "$DEST/arecord"

rm -rf "$ADDONS/webinterface.roomeq" "$ADDONS/script.roomeq"
cp -r "$SRC/kodi/webinterface.roomeq" "$SRC/kodi/script.roomeq" "$ADDONS/"
mkdir -p "$ADDONS/webinterface.roomeq/data"

mkdir -p "$(dirname "$UDEV")"
echo 'ACTION=="add", SUBSYSTEM=="sound", KERNEL=="controlC*", RUN+="/storage/roomeq/roomeq.sh setup"' > "$UDEV"
touch "$AUTOSTART"; chmod +x "$AUTOSTART"
grep -q 'roomeq.sh setup' "$AUTOSTART" || echo '/storage/roomeq/roomeq.sh setup >/dev/null 2>&1' >> "$AUTOSTART"

"$DEST/roomeq.sh" setup

if rpc '{"jsonrpc":"2.0","method":"JSONRPC.Ping","id":1}' | grep -q pong; then
    kodi-send --host=127.0.0.1 -a UpdateLocalAddons >/dev/null 2>&1 || true
    sleep 3
    for a in script.roomeq webinterface.roomeq; do
        rpc "{\"jsonrpc\":\"2.0\",\"method\":\"Addons.SetAddonEnabled\",\"params\":{\"addonid\":\"$a\",\"enabled\":true},\"id\":1}" >/dev/null
    done
    # release the sound card as soon as playback stops, so a measurement can start right away
    rpc '{"jsonrpc":"2.0","method":"Settings.SetSettingValue","params":{"setting":"audiooutput.streamsilence","value":0},"id":1}' >/dev/null
    echo "Kodi: add-ons enabled."
    echo "Restart Kodi once (systemctl restart kodi) so it lists the new audio devices."
else
    echo "Kodi's web server is off: enable Settings > Services > Control > 'Allow remote control via HTTP'"
    echo "(no password, or adapt KODI_RPC), then run this script again."
fi
IP=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
echo "Open http://${IP:-<kodi-ip>}:8080/addons/webinterface.roomeq/"
