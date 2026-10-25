**Sonix Player for the TempoTec Variations V1**

**Sonix Player for the TempoTec Variations V1** follows a clear lineage: **[Jepl4r's Sonix Player](https://github.com/Jepl4r/sonix-player)**, then **[MrFox's V1 fork](https://github.com/mr-f0xx/sonix-player-tempotec-v1)**, which ported the player to the TempoTec Variations V1, and then this fork. The V1 target and firmware packaging build on MrFox's work; the additions below are maintained here. Credit for inherited player, packer and porting work remains with their original authors.

**Additions and V1-focused work maintained in this fork:**

- **Multilingual interface** — the full build includes eight selectable language packs. The language can be changed in Settings, and MiSans fills in glyphs missing from the selected font; additional TTF/OTF fonts can be added from the card.
- **V1-focused Tidal and Podcasts screens** — layouts, search and navigation tailored to the V1's 240 × 320 display, with list caching to make previously loaded names available without another network fetch.
- **Podcast listening state and organization** — listened marks, manual marking, regional trending charts, followed-show ordering, and downloaded episodes stored on the card.
- **Audio controls where they are needed** — EQ, PEQ and MSEB controls are available on the USB DAC and Bluetooth receiver pages as well as in the usual player controls.
- **Consistent favourites** — the same favourite state and heart control across the player, Music page, selection mode and control centre.
- **Useful diagnostics** — slow GUI event-loop passes are logged with timing and callback context to help investigate pauses and lag ([log details](docs/collecting-logs.md#the-interfaces-own-timing-ui-lines)).

The firmware builder starts with an official V1 update package, replaces the stock music player and interface with Sonix, then repackages the update as `v1.upt`.

This checkout uses **`personal` for personal firmware releases**. Merging work from `dev` into `personal` starts a build, gives the personal build a new version, and publishes its `v1.upt` and matching checksum as a release in this repository. Pushes to `dev` do not package firmware. The `main` branch is the shareable version: it can produce an Actions artifact, and a release from it is optional. See [the release guide](docs/releases.md).

This is not an official TempoTec or HiBy release. **It has been tested on physical TempoTec Variations V1 hardware and works on the tested device.** It targets the **TempoTec Variations V1 only**; compatibility with the V1-A or other players in the Variations family is not established.


## Previews

Interface previews of Sonix, rendered at the V1's native 240 × 320 resolution. Click any picture to open it full size. The font and the colours shown are not the defaults.

<table>
  <tr>
    <td align="center">
      <a href="screenshots/main-menu.png"><img src="screenshots/main-menu.png" width="180" alt="Main menu"></a>
      <br><sub><b>Main menu</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/music.png"><img src="screenshots/music.png" width="180" alt="Music"></a>
      <br><sub><b>Music</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/more.png"><img src="screenshots/more.png" width="180" alt="More"></a>
      <br><sub><b>More</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/wireless.png"><img src="screenshots/wireless.png" width="180" alt="Wireless"></a>
      <br><sub><b>Wireless</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="screenshots/settings.png"><img src="screenshots/settings.png" width="180" alt="Settings"></a>
      <br><sub><b>Settings</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/now-playing.png"><img src="screenshots/now-playing.png" width="180" alt="Now Playing"></a>
      <br><sub><b>Now Playing</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/file-browser.png"><img src="screenshots/file-browser.png" width="180" alt="File Browser"></a>
      <br><sub><b>File Browser</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/streaming.png"><img src="screenshots/streaming.png" width="180" alt="Streaming"></a>
      <br><sub><b>Streaming</b></sub>
    </td>
  </tr>
</table>

### Amber on black theme

Showcase of **Amber** appearance: fiery orange accents on a pure black background, with warm amber artwork and glowing icons.

<table>
  <tr>
    <td align="center">
      <a href="screenshots/ember/main-menu.png"><img src="screenshots/ember/main-menu.png" width="180" alt="Ember Main menu"></a>
      <br><sub><b>Main menu</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/music.png"><img src="screenshots/ember/music.png" width="180" alt="Ember Music"></a>
      <br><sub><b>Music</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/more.png"><img src="screenshots/ember/more.png" width="180" alt="Ember More"></a>
      <br><sub><b>More</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/wireless.png"><img src="screenshots/ember/wireless.png" width="180" alt="Ember Wireless"></a>
      <br><sub><b>Wireless</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="screenshots/ember/settings.png"><img src="screenshots/ember/settings.png" width="180" alt="Ember Settings"></a>
      <br><sub><b>Settings</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/now-playing.png"><img src="screenshots/ember/now-playing.png" width="180" alt="Ember Now Playing"></a>
      <br><sub><b>Now Playing</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/file-browser.png"><img src="screenshots/ember/file-browser.png" width="180" alt="Ember File Browser"></a>
      <br><sub><b>File Browser</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/ember/streaming.png"><img src="screenshots/ember/streaming.png" width="180" alt="Ember Streaming"></a>
      <br><sub><b>Streaming</b></sub>
    </td>
  </tr>
</table>

### OLED theme

Showcase of **OLED** appearance: true black backgrounds with white as the accent, on the V1 layouts. Nothing is filled grey: cards, rows and buttons are as black as the page, and Now Playing sets the sleeve on black instead of on a blurred copy of it. The main menu leaves out the title and tagline, so its six tiles start right under the status bar. Rendered by the host simulator with the V1 profile.

<table>
  <tr>
    <td align="center">
      <a href="screenshots/oled/main-menu.png"><img src="screenshots/oled/main-menu.png" width="180" alt="OLED main menu"></a>
      <br><sub><b>Main menu</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/music.png"><img src="screenshots/oled/music.png" width="180" alt="OLED music"></a>
      <br><sub><b>Music</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/more.png"><img src="screenshots/oled/more.png" width="180" alt="OLED more"></a>
      <br><sub><b>More</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/wireless.png"><img src="screenshots/oled/wireless.png" width="180" alt="OLED wireless"></a>
      <br><sub><b>Wireless</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="screenshots/oled/settings.png"><img src="screenshots/oled/settings.png" width="180" alt="OLED settings"></a>
      <br><sub><b>Settings</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/now-playing.png"><img src="screenshots/oled/now-playing.png" width="180" alt="OLED now playing: Gold BBS'S by The Alchemist &amp; Conway The Machine"></a>
      <br><sub><b>Now Playing — Gold BBS'S (LULU)</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/file-browser.png"><img src="screenshots/oled/file-browser.png" width="180" alt="OLED file browser"></a>
      <br><sub><b>File Browser</b></sub>
    </td>
    <td align="center">
      <a href="screenshots/oled/streaming.png"><img src="screenshots/oled/streaming.png" width="180" alt="OLED streaming"></a>
      <br><sub><b>Streaming</b></sub>
    </td>
  </tr>
</table>

## Contents

- [Target device](#target-device)
- [Features](#features)
- [Previews](#previews)
  - [Amber on black theme](#amber-on-black-theme)
  - [OLED theme](#oled-theme)
- [Build a firmware image](#build-a-firmware-image)
- [Install or restore firmware](#install-or-restore-firmware)
- [Streaming credentials](#streaming-credentials)
- [Subsonic / Gonic](#subsonic--gonic)
- [Build from source](#build-from-source)
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

The V1 build includes a local library and file browser, streaming services, wireless audio, and several non-music apps. Highlights include:

- **Eight selectable interface languages** in the full build, with live language switching and font fallback for scripts missing from the selected face. MiSans, Neon 80s, Roboto and user-installed TTF/OTF fonts are supported; see [`sonix-player/assets/fonts/FONT-LICENSES.txt`](sonix-player/assets/fonts/FONT-LICENSES.txt) for font credits and licenses.
- **Local playback** with playlists, favourites, lyrics, artwork, search, play counts, audiobooks and a broad set of formats: WAV, FLAC, MP3, Ogg Vorbis, Opus, M4A/M4B/MP4, AAC, ALAC, WavPack, APE, AIFF/AIFC, CAF and DSD (DSF/DFF).
- **Streaming and network audio:** Tidal, Qobuz, Subsonic/Gonic, internet radio, Bluetooth audio in and out, AirPlay and DLNA. Streaming features need working service credentials or a compatible server and may depend on third-party availability.
- **Audio processing:** 10-band graphic EQ, parametric EQ, MSEB, crossfeed, channel balance and ReplayGain. The EQ, PEQ and MSEB can also process internet radio and USB DAC input.
- **USB audio:** use the V1 as a USB DAC for a computer or as a USB audio host for compatible DACs and headphones.
- **More built-in apps:** the Podcast Index catalogue and downloaded episodes, an EPUB reader, and a Gearboy Game Boy / Game Boy Color emulator.
- **Appearance options:** Dark and Light palettes, Amber on Black and OLED themes, player layouts, optional rounded screen corners, and configurable boot screens including custom images from the card.

The boot screen can be set to **Retrospace**, **Space**, **Travelling in space**, the stock light or dark image, or a custom JPEG/PNG in the card's `Bootlogo` folder. Custom images are copied into the player so the selected boot screen remains available without the card. Rounded screen corners are also optional and off by default.

See [FEATURES.md](FEATURES.md) for a longer feature list. Streaming services also require valid credentials and may depend on third-party service availability.

## Build a firmware image

GitHub Actions builds the firmware using the MIPS toolchain in `sonix-player/rockboxdev/`, plus `7z`, `squashfs-tools` and `genisoimage`. A push or merge to `personal` automatically builds and publishes a personal release. Each push normally increments the patch version; commit messages can request a minor or major version bump. The built personal image is stamped to check for updates from this repository and credits the personal fork. Pushes to `dev` do not run the firmware build.

To create a personal release, merge the finished work into `personal`. The workflow builds the merged commit, creates a version tag and release, and attaches `v1.upt` plus `v1_md5.txt`. Its Actions artifact also includes the build report, validation reports and player binaries. The first run builds and caches the toolchain and can take 30–90 minutes; later runs are usually faster.

For a shareable build from `main`, that branch's workflow run provides an Actions artifact named for its exact commit and kept for 14 days. It does not create a personal release. Publishing a `main` build is optional and deliberate: run the workflow for the exact tagged commit or set `publish_release_tag` for an existing release. The workflow checks that the tag and built commit match before uploading the two firmware files. It does not build a firmware image from `dev`.

The workflow can also be run manually for a chosen branch, tag or commit. A manual run requires the stock firmware URL and the community-build acknowledgement. If you publish to a release, use the exact commit named by its tag. Always check `BUILD-REPORT.txt` to confirm which source revision produced an artifact.

## Install or restore firmware

Only attempt this on a TempoTec Variations V1. Read the warning above before proceeding.

1. Keep the **official V1 `v1.upt` and its matching `v1_md5.txt`** somewhere safe. They are needed if you want to return to stock.
2. For the personal build, download `v1.upt` and `v1_md5.txt` together from the personal release in this repository. For a shareable `main` build, use its Actions artifact or its deliberately published release, and check that its build report names the intended commit. Copy both files to the root of a microSD card supported by the device.
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

[podcast]
api_key =
api_secret =

[lastfm]
api_key =
api_secret =
```

#### Qobuz without keys of your own

Qobuz does not issue application credentials for personal or open-source clients, which is why the stock V1 firmware works out of the box: its `hiby_player` carries HiBy's own `app_id`/`app_secret` internally. Sonix already speaks that exact authentication flow; it only lacks the credential pair, which belongs to HiBy and is neither committed nor shipped in published images.

When you build an image **for your own device** from the official stock firmware, the packer can extract that pair from the stock binary and install it automatically — no `streaming-keys.ini` on the card:

```sh
SONIX_EMBED_STOCK_KEYS=1 ./sonix_firmware_packer.sh
```

With a `streaming-keys.key` present the credentials are sealed into `streaming-keys.bin`; without one they ship as a plain, player-readable `streaming-keys.ini`. The extractor only runs on this explicit opt-in, masks values in its output, and leaves Tidal on the manual path (its secret is assembled at run time in the stock binary and cannot be scanned out). **An image built this way carries a third party's API credentials: keep it private, never upload it to a release.** The standalone tool (`sonix-player/tools/extract_streamkeys.py`, with `--check` and `--list-candidates`) and the full investigation, including what was traced in the stock binary, are documented in [docs/qobuz-stock-firmware.md](docs/qobuz-stock-firmware.md).

### Your own server with a self-signed certificate

A home server with a self-signed certificate — the usual setup for Subsonic/Gonic, and something no CA bundle can ever validate — is refused by default. **Accept self-signed certificates** under **Streaming → Subsonic → Server settings** lets it through, without `adb` and without a restart. The same thing can be set in `/usr/data/device_config.ini`:

```ini
[network]
tls_verify = relaxed
```

**Relaxed** forgives a certificate whose issuer is not in the trust store, and nothing else: the certificate still has to name the server being contacted, and its dates still have to cover the current date, so an expired certificate or one issued to a different name fails and says so. A certificate error that names a date usually means the player's clock is wrong, not that the mode is missing.

The setting is global — it applies to every `https://` connection the player opens, radio stations included — because one TLS context serves them all. Anyone using a public network should leave it at the default, `strict`. A private certificate authority is better handled with the `[network] ca_bundle` setting, which keeps checking on: export the CA's own certificate, copy that file to the card, and point `ca_bundle` at it.

For a private development build, `sonix-player/tools/seal_streamkeys.py` can place a sealed credentials file in the assets before compilation. This format is **obfuscation, not secure encryption**: the key is compiled into the player binary and can be recovered by someone who analyzes it. Never publish a binary containing credentials you do not intend to disclose.

## Subsonic / Gonic

**Streaming → Subsonic → Server settings** connects to your own Subsonic-compatible server, including Gonic. Enter its base URL and your account, then select **Connect / save**. Browse Music folders or Artists and select a track to play the current page.

MP3 at **256 kbps** is the default; **320 kbps**, **Opus**, and **Raw** (original FLAC/MP3/Opus) are available. Transcoding must be enabled on the server. Network workers feed two fixed **1 MiB RAM rings**, refill at about 50%, and pre-cache the next track's first **512 KiB** near 90% progress. There is no disk audio cache, localhost proxy, or socket read in the decoder.

No application API key is needed. Prefer HTTPS; credentials stay in memory unless explicitly remembered, and remembered passwords are stored **unencrypted** in a private device-local file. A server with a self-signed certificate needs **Accept self-signed certificates** on its settings page — see [your own server with a self-signed certificate](#your-own-server-with-a-self-signed-certificate). Seeking and reboot restoration are not available for these ephemeral streams. Compatible-format gapless handoff uses the existing ALSA path; network, codec timing and physical-device limits still apply.

See [setup, design, limitations and the V1 validation checklist](docs/subsonic.md), and [host integration tests](sonix-player/tests/README.md).

## Build from source

**The simulator (host).** LVGL plus SDL2, FreeType and the system's opusfile and WavPack; no device and no cross-compiler.

```bash
sudo apt install build-essential pkg-config libsdl2-dev libfreetype-dev \
    libopusfile-dev libwavpack-dev libasound2-dev
cd sonix-player
make host            # ./sonix_player_host, at the V1's 240x320
./sonix_player_host
```

The simulator draws the same profile as the device: the 480-pixel layout the
upstream tree also carried is not built here any more.

**The device binary (target).** Needs the MIPS toolchain built from `sonix-player/rockboxdev/`; the workflow caches it, and building it by hand is a long first run.

```bash
cd sonix-player
make target        # sonix_player, sonix_player_debug and sonix_launch
```

**Headless suites.** Each one has its own target and runs on the host, against the real database or the real LVGL layout rather than a mock:

```bash
make -C sonix-player/tests podcastplayed
make -C sonix-player/tests mostplayed
make -C sonix-player/tests eq
make -C sonix-player/tests ui
```

[sonix-player/tests/README.md](sonix-player/tests/README.md) lists every suite and what it pins.

## Project files and documentation

- `sonix-player/` — player sources, LVGL configuration, launcher and build system.
- `sonix-packer/` — stock-image preparation, firmware packaging and validation scripts.
- `docs/collecting-logs.md` — which logs to send with a bug report, and how to take them.
- `docs/subsonic.md` — Subsonic/Gonic setup, RAM transport and hardware validation.
- `docs/sgm31324-led-control.md` — driving the RGB status LED directly over I²C.
- `sonix-player/tools/sonix-bugreport.sh` — one-file diagnostic bundle, run on the device over ADB.
- `sonix-player/tests/README.md` — the headless suites, and what each one pins.
- `.github/workflows/build-tempotec-v1.yml` — the firmware build described above.
- `docs/releases.md` — how `dev`, `personal` and `main` produce personal and shareable firmware.
- `FEATURES.md` — detailed feature notes.
- `PATCHES.md` — device-porting and implementation notes.
- `LICENSE` — GNU General Public License, version 3. Third-party components retain their own licenses.

For bug reports, include the exact model, the firmware build/commit and relevant logs — see [Collecting logs for a bug report](docs/collecting-logs.md) for what to turn on and where the files are. Remove personal data and **never attach streaming credentials**.

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

