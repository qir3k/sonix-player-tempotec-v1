**Sonix Player for the TempoTec Variations V1**

Sonix Player for the TempoTec Variations V1 follows a clear lineage: **[Jepl4r's Sonix Player](https://github.com/Jepl4r/sonix-player)**, then **[MrFox's V1 fork](https://github.com/mr-f0xx/sonix-player-tempotec-v1)**, which ported the player to the TempoTec Variations V1, and then this fork. The V1 target and firmware packaging build on MrFox's work; the additions below are maintained here. Credit for inherited player, packer and porting work remains with their original authors.

**Additions and V1-focused work maintained in this fork:**

- **One board, one tree** — the 240 × 320 V1 profile is a compile-time constant, not a switch: the 480-pixel layouts and the other HiBy targets are gone. Every page in this tree is laid out for the V1's panel.
- **Eleven interface languages** — English, Deutsch, Español, Français, Italiano, Japanese, Nederlands, Polski, Português BR, Russian and Ukrainian, each selectable in Settings → Plugins → Languages and Fonts, where the script fallbacks (Japanese, Korean, Arabic, Thai, Cyrillic) are separate add-ons so a bare build carries only the faces it needs.
- **Plugins** — every optional feature is a switch in Settings → Plugins: streaming services, music features (Lyrics Fetcher, Cover Art Fetcher, EQ, PEQ, MSEB, sleep timers, AutoEQ, Last.fm, ListenBrainz), the library shelves, the tools and the themes. A module switched off is never built, so its memory stays free; see [ADDONS.md](sonix-player/docs/ADDONS.md).
- **YouTube Music without a computer** — search, charts, Explore, albums, playlists and podcasts on the device alone, played as they download, with a download option, a session imported from the card for the account's own library, and a Piped / Invidious fallback when Google refuses the device's own player request. See [YOUTUBE_MUSIC_SETUP.md](sonix-player/YOUTUBE_MUSIC_SETUP.md).
- **Streaming that behaves like the local library** — Tidal, Qobuz, Podcasts, Radio and YouTube Music share one region list (Worldwide by default), one sleep timer, one favourites mark, one illustration of lists and one set of download pages.
- **Bluetooth** — LDAC quality settings, AirPods battery and noise control, EarFun earbuds over the GAIA serial protocol, and a codec list that ranks LHDC, LLAC, Samsung Scalable and LC3/LC3+ above the older families where the link and the daemon support them.
- **Useful diagnostics** — slow GUI event-loop passes are logged with timing and callback context to help investigate pauses and lag ([log details](docs/collecting-logs.md#the-interfaces-own-timing-ui-lines)).

The firmware builder starts with an official V1 update package, replaces the stock music player and interface with Sonix, then repackages the update as `v1.upt`.

This checkout has **two branches**. `dev` is where work is written and tested; a push to it builds the player and runs the test suites, and packages nothing. `personal` is the release branch: it holds what `dev` holds once a change has been tested there, and a push to it runs the same tests, then builds the firmware, stamps it with a new version and publishes `v1.upt` with its matching checksum as a release in this repository. `main` is not used. See [the release guide](docs/releases.md).

This is not an official TempoTec or HiBy release. **It has been tested on physical TempoTec Variations V1 hardware and works on the tested device.** It targets the **TempoTec Variations V1 only**; compatibility with the V1-A or other players in the Variations family is not established.

## Contents

- [Target device](#target-device)
- [Features](#features)
- [Build a firmware image](#build-a-firmware-image)
- [Install or restore firmware](#install-or-restore-firmware)
- [Streaming credentials](#streaming-credentials)
- [Build from source](#build-from-source)
- [Tests](#tests)
- [Project files and documentation](#project-files-and-documentation)
- [Credits](#credits)

## Target device

The **TempoTec Variations V1**:

| Component | Specification |
|---|---|
| **SoC** | Ingenic X1600 (MIPS32r2 core, o32 ABI, glibc 2.22 compatibility) |
| **Display** | 2.0-inch QVGA TFT panel, **240×320 pixels** portrait |
| **DAC** | Dual Cirrus Logic CS43131 |
| **Audio Outputs** | 3.5 mm single-ended + 4.4 mm balanced headphone jacks |
| **Storage** | One MicroSD card slot (exFAT / FAT32) |
| **Controls** | Side flank buttons (Power, Vol+, Vol-, Play/Pause, Next/Prev) + capacitive touchscreen |
| **Wireless** | 2.4 GHz Wi-Fi (802.11 b/g/n) and bi-directional Bluetooth |
| **USB** | USB Type-C (bidirectional USB DAC & USB audio out) |
| **Firmware Package** | `v1.upt` with accompanying `v1_md5.txt` |

The player UI is built with LVGL 9.6 and adapted for the V1's small display. The package is based on vendor firmware; it is **not** a Linux distribution or a kernel replacement.

## Features

The V1 build includes a local library and file browser, streaming services, wireless audio, and several non-music apps. Highlights:

- **Interface languages** — eleven packs, switched live, with script fallbacks (Japanese, Korean, Arabic, Thai, Cyrillic) installed as add-ons under Plugins → Languages and Fonts. MiSans, Neon 80s, Roboto and user-installed TTF/OTF fonts are supported; see [`sonix-player/assets/fonts/FONT-LICENSES.txt`](sonix-player/assets/fonts/FONT-LICENSES.txt) for font credits and licences.
- **Local playback** with playlists, favourites, lyrics, artwork, search, play counts, audiobooks and a broad set of formats: WAV, FLAC, MP3, Ogg Vorbis, Opus, M4A/M4B/MP4, AAC, ALAC, WavPack, APE, AIFF/AIFC, CAF and DSD (DSF/DFF).
- **Streaming and network audio:** Tidal, Qobuz, YouTube Music, Subsonic/Gonic, internet radio, Bluetooth audio in and out, AirPlay and DLNA. Streaming features need working service credentials or a compatible server and may depend on third-party availability.
- **Audio processing:** 10-band graphic EQ, parametric EQ, MSEB, crossfeed, channel balance and ReplayGain — each an optional module, and each able to process internet radio and USB DAC input as well as local files.
- **USB audio:** use the V1 as a USB DAC for a computer or as a USB audio host for compatible DACs and headphones.
- **More built-in apps:** the Podcast Index catalogue and downloaded episodes, an EPUB reader, a metronome, and a Gearboy Game Boy / Game Boy Color emulator.
- **Appearance:** four themes (Dark, Light, Amber on Black, OLED) — switchable in Plugins, with at least one always on and a boot fallback if the saved one is switched off — player layouts, optional rounded screen corners, and configurable boot screens including custom images from the card.

The boot screen can be set to **Retrospace**, **Space**, **Travelling in space**, the stock light or dark image, or a custom JPEG/PNG in the card's `Bootlogo` folder. Custom images are copied into the player so the selected boot screen remains available without the card. Rounded screen corners are also optional and off by default.

The battery is looked after at both ends. At **5 %** the player says so — a red card, and the panel comes back up for it if it was dark — and at **1 %** it switches itself off the ordinary way, position written and card unmounted, instead of letting the cell drop out from under it; below 10 % it also stays out of standby, because a sleeping player cannot watch its own gauge. A player that was **switched off and is then put on a charger** comes up on a battery screen instead of the library: the level, the percentage and what the charger is doing, going dark after ten seconds, with a **press** of the power button to bring it back and a **hold** to start the player after all.

See [FEATURES.md](FEATURES.md) for the detailed feature notes and [PATCHES.md](PATCHES.md) for how the V1 port works underneath.

## Build a firmware image

GitHub Actions builds the firmware using the MIPS toolchain in `sonix-player/rockboxdev/`, plus `7z`, `squashfs-tools` and `genisoimage`.

- **`dev`** — every push builds the host player and runs the test suites. No firmware image is produced.
- **`personal`** — every push runs those same tests first, and only then builds, stamps and releases the image. A failing suite stops the release.
- **Manual runs** — **Build TempoTec V1 firmware** can be dispatched for any branch, tag or commit. A manual run requires the stock firmware URL and the community-build acknowledgement. Use `publish_release_tag` to upload the built image to an existing release; the workflow checks that the tag and the built commit match first.

Each push to `personal` normally increments the patch version; commit messages can request a minor or major bump. The release carries `v1.upt` and `v1_md5.txt`, and its notes list the commits included. The Actions artifact for the same run also includes `BUILD-REPORT.txt`, the stock and output validation reports, and the player binaries. Always check `BUILD-REPORT.txt` to confirm which source revision produced an artifact.

The first run builds and caches the toolchain and can take 30–90 minutes; later runs are usually faster.

## Install or restore firmware

Only attempt this on a TempoTec Variations V1. Read the warning above before proceeding.

1. Keep the **official V1 `v1.upt` and its matching `v1_md5.txt`** somewhere safe. They are needed if you want to return to stock.
2. Download `v1.upt` and `v1_md5.txt` together from the personal release in this repository, and check that its build report names the intended commit. Copy both files to the root of a microSD card supported by the device.
3. Charge the player adequately, insert the card and start the device's built-in firmware update process. Menu names vary by stock firmware version; follow the device's on-screen instructions and do not interrupt the update.
4. If you want to restore stock firmware, put the original vendor `v1.upt` and its matching checksum file at the card root and use the same update process.

The package builder is designed to preserve the stock `xImage` kernel and leave the existing update/recovery path in place. This is checked during the build, but it is **not** a guarantee that recovery will work on every device or in every failure case.

## Streaming credentials

Credentials for Qobuz, Tidal, Podcast Index and Last.fm are not included in this repository or in the standard workflow artifact. Without usable credentials, those services remain disabled; local playback does not require them.

To try credentials on a build that has no keys embedded, copy `sonix-player/streaming-keys.ini.example` to the root of the microSD card as `streaming-keys.ini`, fill in only the services for which you have authorized credentials, then restart the player. Do not commit or share this file.

```ini
[qobuz]
app_id =
app_secret =

[tidal]
client_id =
client_secret =

# The InnerTube key music.youtube.com hands every visitor, read off the page.
[ytmusic]
api_key =

[podcast]
api_key =
api_secret =

[lastfm]
api_key =
api_secret =
```

The YouTube Music key is the one exception worth understanding: it is not an account credential, it is the key the public `music.youtube.com` page hands every visitor, and no account is needed for anything this player browses. A build can also supply it at compile time (`YTMUSIC_API_KEY`, or `sonix-player/.ytmusic-key`), and [YOUTUBE_MUSIC_SETUP.md](sonix-player/YOUTUBE_MUSIC_SETUP.md) says where to read it off the page. An account — for liked songs and your own playlists — is attached by importing a browser session from the card, not by signing in.

ListenBrainz is the one scrobbler that needs no credentials in the image: its account is a personal user token, requested on listenbrainz.org/settings and pasted into Settings → Music → ListenBrainz. Only the token is kept on the device. Scrobbling to Last.fm and to ListenBrainz are independent and can run side by side.

#### Qobuz without keys of your own

Qobuz does not issue application credentials for personal or open-source clients, which is why the stock V1 firmware works out of the box: its `hiby_player` carries HiBy's own `app_id`/`app_secret` internally. Sonix already speaks that exact authentication flow; it only lacks the credential pair, which belongs to HiBy and is neither committed nor shipped in published images.

When you build an image **for your own device** from the official stock firmware, the packer can extract that pair from the stock binary and install it automatically — no `streaming-keys.ini` on the card:

```sh
SONIX_EMBED_STOCK_KEYS=1 ./sonix_firmware_packer.sh
```

With a `streaming-keys.key` present the credentials are sealed into `streaming-keys.bin`; without one they ship as a plain, player-readable `streaming-keys.ini`. The extractor only runs on this explicit opt-in, masks values in its output, and leaves Tidal on the manual path (its secret is assembled at run time in the stock binary and cannot be scanned out). **An image built this way carries a third party's API credentials: keep it private, never upload it to a release.** The standalone tool (`sonix-player/tools/extract_streamkeys.py`, with `--check` and `--list-candidates`) and the full investigation are documented in [docs/qobuz-stock-firmware.md](docs/qobuz-stock-firmware.md).

### Your own server with a self-signed certificate

A home server with a self-signed certificate — the usual setup for Subsonic/Gonic, and something no CA bundle can ever validate — is refused by default. **Accept self-signed certificates** under **Streaming → Subsonic → Server settings** lets it through, without `adb` and without a restart. The same thing can be set in `/usr/data/device_config.ini`:

```ini
[network]
tls_verify = relaxed
```

**Relaxed** forgives a certificate whose issuer is not in the trust store, and nothing else: the certificate still has to name the server being contacted, and its dates still have to cover the current date, so an expired certificate or one issued to a different name fails and says so. A certificate error that names a date usually means the player's clock is wrong, not that the mode is missing.

The setting is global — it applies to every `https://` connection the player opens, radio stations included — because one TLS context serves them all. Anyone using a public network should leave it at the default, `strict`. A private certificate authority is better handled with the `[network] ca_bundle` setting, which keeps checking on: export the CA's own certificate, copy that file to the card, and point `ca_bundle` at it.

For a private development build, `sonix-player/tools/seal_streamkeys.py` can place a sealed credentials file in the assets before compilation. This format is **obfuscation, not secure encryption**: the key is compiled into the player binary and can be recovered by someone who analyzes it. Never publish a binary containing credentials you do not intend to disclose.

Subsonic and Gonic have their own chapters: **Streaming → Subsonic → Server settings** takes the base URL and account, MP3 256 kbps is the default (320, Opus and Raw are available), and [docs/subsonic.md](docs/subsonic.md) covers the transport design, the limitations and the hardware checklist.

## Build from source

**The simulator (host).** LVGL plus SDL2, FreeType and the system's opusfile and WavPack; no device and no cross-compiler.

```bash
sudo apt install build-essential pkg-config libsdl2-dev libfreetype-dev \
    libopusfile-dev libwavpack-dev libasound2-dev
cd sonix-player
make host            # ./sonix_player_host, at the V1's 240x320
./sonix_player_host
```

**The device binary (target).** Needs the MIPS toolchain built from `sonix-player/rockboxdev/`; the workflow caches it, and building it by hand is a long first run.

```bash
cd sonix-player
make target        # sonix_player, sonix_player_debug and sonix_launch
```

**The firmware package.** `sonix-packer/sonix_firmware_packer.sh` takes the official stock `v1.upt`, swaps in the built binaries and assets, and repackages the update. `sonix-packer/verify_upt.sh` checks the result; the workflow runs both.

## Tests

Every suite runs on a Linux host against the real modules — the real database, the real LVGL layout, the real decoders — rather than against mocks, and none of them needs the device, a MIPS toolchain, a music server, an account or downloaded audio.

```bash
make -C sonix-player/tests all           # everything, in the order CI runs it
make -C sonix-player/tests test          # Subsonic client, HTTP/TLS, RAM rings
make -C sonix-player/tests ui            # main menu, screensaver, Tools, Wi-Fi pages
make -C sonix-player/tests eq            # EQ, PEQ and MSEB on PCM, radio and USB DAC
make -C sonix-player/tests ytmusic       # YouTube Music resolution against a fake network
make -C sonix-player/tests earfun        # the EarFun control protocol
```

There are suites for the battery guard, the boot picture, the screensaver folder, software volume, the keyboard layouts, the calculator, notes, AutoEQ layout, tag dates, lyric search, tags, covers, tag writing, radio FLAC, most played, podcast heard marks, ListenBrainz, the credential extractor and the relay. [sonix-player/tests/README.md](sonix-player/tests/README.md) lists every target and what it pins.

`.github/workflows/tests.yml` runs all of them, and the firmware workflow calls it before packaging, so a release cannot be built from a tree whose tests fail.

## Project files and documentation

- `sonix-player/` — player sources, LVGL configuration, launcher and build system.
- `sonix-packer/` — stock-image preparation, firmware packaging and validation scripts.
- `sonix-player/tests/` — the headless suites, and [what each one pins](sonix-player/tests/README.md).
- `sonix-player/tools/` — the credential extractor and sealer, the icon and web-asset generators, the touchscreen patches, the on-device bug-report bundle and the YouTube Music relay.
- [FEATURES.md](FEATURES.md) — the detailed feature notes.
- [PATCHES.md](PATCHES.md) — the V1 port, patch by patch, and why each one looks the way it does.
- [docs/releases.md](docs/releases.md) — how `dev` and `personal` produce the firmware, and what the tests gate.
- [docs/collecting-logs.md](docs/collecting-logs.md) — which logs to send with a bug report, and how to take them.
- [docs/subsonic.md](docs/subsonic.md) — Subsonic/Gonic setup, RAM transport and hardware validation.
- [docs/qobuz-stock-firmware.md](docs/qobuz-stock-firmware.md) — the stock firmware's Qobuz credentials, and the extractor.
- [docs/ldac-diagnostics.md](docs/ldac-diagnostics.md) — reading an LDAC link's rate and quality from the device.
- [docs/sgm31324-led-control.md](docs/sgm31324-led-control.md) — driving the RGB status LED directly over I²C.
- [sonix-player/docs/ADDONS.md](sonix-player/docs/ADDONS.md) — the Plugins model, and how to add a module.
- [sonix-player/YOUTUBE_MUSIC_SETUP.md](sonix-player/YOUTUBE_MUSIC_SETUP.md) — the InnerTube key, the sources and the session import.
- `.github/workflows/` — `tests.yml` (every suite), `build-tempotec-v1.yml` (tests, then the image and the release).
- [LICENSE](LICENSE) — GNU General Public License, version 3. Third-party components retain their own licences.

For bug reports, include the exact model, the firmware build/commit and relevant logs — see [Collecting logs for a bug report](docs/collecting-logs.md) for what to turn on and where the files are. `sonix-player/tools/sonix-bugreport.sh` collects them into one file on the device over ADB. Remove personal data and **never attach streaming credentials**.

## Credits

This project builds on the original **[Sonix Player](https://github.com/Jepl4r/sonix-player)** and **[MrFox's TempoTec V1 port](https://github.com/mr-f0xx/sonix-player-tempotec-v1)**. Most of the player and packer code comes from Jepl4r's project, and the V1 board profile and firmware packaging come from MrFox's port. This fork adds and maintains the V1-focused work summarized at the top of this README. The original project is licensed under GPL-3.0, and so is this fork.

- **[Jepl4r](https://github.com/Jepl4r)** — author and maintainer of Sonix Player, and the source of the player, the packer and the interface this port is built on.
- **[mr-f0xx](https://github.com/mr-f0xx)** — the TempoTec Variations V1 port this fork is built on: the V1 board profile, the V1 packaging path, and the documentation this README grew from.
- **[Tartarus6](https://github.com/Tartarus6)**, **[noisetta](https://github.com/noisetta)**, **[endgame47](https://github.com/endgame47)**, **[hkhrithik007](https://github.com/hkhrithik007)** — the people named in the original project's own "Special thanks" list.
- **[Hinatai](https://github.com/Hinatai)**, **[KrajzegaX](https://github.com/KrajzegaX)**, **[noemdespertis](https://github.com/noemdespertis)** and everyone else on the upstream [contributor list](https://github.com/Jepl4r/sonix-player/graphs/contributors), whose commits this fork inherits.
- The HiBy and TempoTec communities — the people who test, report, translate and explain both projects — who do the work no commit log shows.

Third-party components carried in the tree, or fetched by the build, keep their own authors and licences:

| component | used for |
|---|---|
| [LVGL](https://github.com/lvgl/lvgl) 9.6 | the whole interface |
| [Gearboy](https://github.com/drhelius/Gearboy) by Ignacio Sanchez | the Game Boy core in `sonix-player/src/gb/core/`, kept verbatim |
| [dr_libs](https://github.com/mackron/dr_libs) by David Reid | FLAC and MP3 decoding |
| [stb](https://github.com/nothings/stb) by Sean Barrett | cover art and image read/write |
| [miniz](https://github.com/richgel999/miniz) (RAD Game Tools, Valve, Rich Geldreich) | inflate for covers and EPUB |
| TJpgDec by ChaN | JPEG decoding on the target |
| [SQLite](https://sqlite.org) | the library index |
| [FreeType](https://freetype.org) | fonts at runtime |
| [libogg, libopus and opusfile](https://opus-codec.org) (Xiph.Org) | Opus playback |
| [WavPack](https://www.wavpack.com) by David Bryant | WavPack playback |
| [BlueALSA](https://github.com/arkq/bluez-alsa) by Arkadiusz Bokowy | Bluetooth audio in the HiBy overlay trees, inherited from upstream |
| [Lucide](https://lucide.dev) | the Wi-Fi transfer page glyphs |
| the [Rockbox](https://rockbox.org) toolchain scripts in `sonix-player/rockboxdev/` | the MIPS cross-compiler |

The licence notice for each component sits with it in the source tree; [LICENSE](LICENSE) covers the project as a whole. The official TempoTec V1 firmware used as the base of a build is not part of this repository, and TempoTec, HiBy and the other vendors own their names and firmware.
