#!/usr/bin/env bash
# Read-only probe of an M-Audio Keystation Mini 32 MK3.
# Sends only a Universal SysEx Identity Request; writes nothing to the device.
set -u
out="$(dirname "$0")/probe-$(date +%Y%m%d-%H%M%S).txt"
exec > >(tee "$out") 2>&1

echo "== lsusb"; lsusb
dev=$(lsusb | grep -i -E 'm-audio|0763:|avid|inmusic' | head -1 | awk '{print $6}')
if [ -z "$dev" ]; then echo "Keystation not found on USB. Is it passed through to the VM?"; exit 1; fi
echo "== USB descriptors for $dev"; lsusb -v -d "$dev" 2>/dev/null

echo "== ALSA raw MIDI ports"; amidi -l
port=$(amidi -l | grep -i -E 'keystation|mini|m-audio' | head -1 | awk '{print $2}')
[ -z "$port" ] && { echo "No MIDI port found (is snd-usb-audio loaded?)"; exit 1; }

echo "== Identity Request -> $port"
amidi -p "$port" -S 'F0 7E 7F 06 01 F7' -d -t 2

echo "== Play keys / press buttons for 15 s (shows what each control sends)"
amidi -p "$port" -d -t 15
echo "Saved to $out"
