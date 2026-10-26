# LDAC bit rate: why the fixed presets stutter, and what the link is really doing

The LDAC quality choice (`Settings → Wireless → Bluetooth audio`, the LDAC rows)
is a setting of the bluealsa daemon, not of the link: the player starts
bluealsa with `--ldac-quality=high|standard|mobile` or `--ldac-abr`, and the
patched daemon also reads the same choice out of `/usr/data/alsa.conf`. Fixed
990 (high) and 660 (standard) can stutter where adaptive (ABR) does not. This
page says why, and how to see the real numbers.

## Why fixed high bit rate stutters

The radio budget. The V1's Bluetooth is a BCM4343A1 combo chip reached over a
3 Mbaud UART (`brcm_patchram_plus` on `/dev/ttyS0`). Classic Bluetooth EDR
tops out near 2.1 Mbit/s of usable ACL throughput, and the UART caps it at
about the same figure before radio time is even counted. LDAC 990 kbps of
audio costs roughly 1.3–1.6 Mbit/s on air once AVDTP, L2CAP and HCI framing
and retransmissions are added — and the same 2.4 GHz silicon is also the
Wi-Fi radio, so streaming from Tidal or Qobuz while listening over Bluetooth
time-slices the two.

At 990 the headroom for a retransmission burst is close to zero. One
interference burst (a Wi-Fi beacon collision, a microwave, a body between
antennas) fills the controller's transmit queue; L2CAP flow control then
stalls bluealsa's encoder thread mid-write; the ALSA PCM underruns; that is
the stutter. Close range does not save you, because the bottleneck is the
protocol budget and the coexistence, not only the distance.

ABR survives the same conditions because it watches the link and steps the
bit rate down before the queue overflows. A fixed preset has no such valve:
it drops packets instead. That is also why the default is ABR.

Many sinks add their own limit: a headphone that negotiates EDR 2 Mbit/s, or
keeps a small decoder buffer sized for ABR pacing, cannot take 990 even on a
clean link.

## What the daemon was told

With Developer options → ADB on:

```sh
adb shell "cat /proc/$(pidof bluealsa)/cmdline | tr '\0' '\n'"
adb shell cat /usr/data/alsa.conf
```

The first shows the daemon's flags as the player built them
(`--ldac-quality=high`, `--ldac-abr`, …); the second is the file the patched
daemon reads its LDAC/UAT quality from. Changing the preset in Settings
restarts the daemon and both of these change with it.

## What the link is really sending

bluealsa exposes the codec over D-Bus but not the live bit rate, so the
actual transmission rate is read off the HCI traffic:

```sh
# capture ~30 seconds while it stutters
adb shell "/usr/bin/btmon -w /mnt/sd_0/.local/ldac.cfa"
adb pull /mnt/sd_0/.local/ldac.cfa
```

Open the capture in Wireshark (Bluetooth ACL link key off, or the file as it
is):

- **Statistics → Conversations → Bluetooth**: bytes per second to the
  headphones. Divide the media-payload rate over the audio's seconds by 8 to
  get the effective kbit/s; fixed 990 reads ≈ 990000, ABR shows it stepping
  down under load.
- The AVDTP media packets carry the LDAC frames; their size and cadence show
  whether the sink is acknowledging or the queue is backing up.

The player's own timeline, without a capture: **Developer options →
Bluetooth log**, then

```sh
adb shell tail -f /mnt/sd_0/.local/bluetooth.log
```

carries btmon's HCI traffic (audio payloads left out, on purpose), the
bluetoothd/bluealsa syslog and the player's `bluetooth:` lines on one
timeline. The `N data packets not shown` counters are the ACL skip counters —
climbing while audio plays is packet loss in progress.

## System pressure, while it stutters

```sh
adb shell "top -b -n 1 | grep -E 'bluealsa|sonix'"
adb shell head -5 /proc/meminfo
```

The encoder runs `SCHED_FIFO` (`--io-rt-priority`, default 12); what hurts it
is not CPU but the blocked write described above, visible as the skip
counters climbing while `top` shows bluealsa mostly idle.

## Practical choices

- **ABR** for anything mobile, and whenever Wi-Fi is busy (Tidal/Qobuz are
  exactly the case the fixed presets lose).
- **660 (standard)** for a desk setup with the player still on the network.
- **990 (high)** only line-of-sight, Wi-Fi idle, and a sink known to take it.
