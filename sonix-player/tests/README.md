# Subsonic / stream-bridge integration tests

Run from the repository root on a Linux host. Every target there is, in the order the workflow runs them:

```sh
make -C sonix-player/tests test            # Subsonic client, HTTP/TLS, the RAM rings
make -C sonix-player/tests sanitize        # the same under ASan + UBSan
make -C sonix-player/tests codecs          # real MP3/FLAC/Opus decoding
make -C sonix-player/tests sanitize-codecs
make -C sonix-player/tests radioflac       # native, mid-stream join, chained Ogg FLAC
make -C sonix-player/tests sanitize-radioflac
make -C sonix-player/tests batteryguard    # warning at five, shutdown at one
make -C sonix-player/tests pictures        # the Screensaver folder's picture rules
make -C sonix-player/tests bootpic         # the Bootlogo folder and the custom boot picture
make -C sonix-player/tests swvolume        # the USB DAC's whole volume law
make -C sonix-player/tests kblayout        # the on-screen keyboard's layouts and accents
make -C sonix-player/tests eq              # EQ, PEQ and MSEB on PCM, radio and USB DAC
make -C sonix-player/tests wifi            # hidden networks and the wpa_cli command
make -C sonix-player/tests calculator       # the arithmetic
make -C sonix-player/tests notes           # the notes folder's rules
make -C sonix-player/tests listenbrainz    # the scrobbler and its queue
make -C sonix-player/tests aamlayout       # Album Art Max's arithmetic
make -C sonix-player/tests tagdate         # the dates tags carry
make -C sonix-player/tests genius          # Smart Mix track-count limits and traversal
make -C sonix-player/tests lrclib          # lyrics search, ranking and sidecar save
make -C sonix-player/tests tags            # tag reading and format sniffing
make -C sonix-player/tests covers          # the 1000-cover cache
make -C sonix-player/tests tagwrite        # rewriting a cover into an MP3 or FLAC
make -C sonix-player/tests alac            # a real ALAC file decoded back to its PCM
make -C sonix-player/tests mostplayed      # the Most played list, on the real database
make -C sonix-player/tests podcastplayed   # heard podcast episodes
make -C sonix-player/tests ui              # main menu, screensaver, Tools, Wi-Fi, charge, lyrics
make -C sonix-player/tests earfun          # the EarFun control protocol
make -C sonix-player/tests ytmusic         # YouTube Music resolution and the keyless build
make -C sonix-player/tests extractkeys     # the stock-firmware credential extractor
make -C sonix-player/tests relay           # tools/ytmusic-relay.py over a real socket
```

Or all of them, in that order:

```sh
make -C sonix-player/tests all -j"$(nproc)"
```

These require only a C compiler, Make and Python 3 (plus standard pthread/dl/math libraries, and `libopusfile-dev`/`libwavpack-dev` for the codec suites). They **do not require ALSA, a MIPS toolchain, a music server, a real account, or downloaded audio**, and the `ui` suite needs only the LVGL checkout the host build already uses.

`run_subsonic_tests.py` starts a temporary threaded localhost HTTP server plus three HTTPS servers carrying the certificates in `tls_fixtures.py`, runs the C binary with a private temporary config directory, verifies its requests, then shuts down. All account values are synthetic. Build products live in ignored `.build/`; response/PCM fixtures are generated in memory rather than added to Git, and the three test certificates are pasted into that Python file rather than downloaded.

## Coverage

The C binary links the **real** HTTP/TLS helpers, REST client, MD5, JSON, configuration, playlist, RAM producer/reader and two-slot manager. `support.c` replaces only unrelated platform scheduling/signal helpers, translations and local-library indexes; it does not replace networking, buffers or decoding.

Checks include:

- correct salted MD5 authentication with encoded UTF-8/special characters, fresh salts, and no password/token query in diagnostic output;
- TLS against real self-signed certificates: `strict` (the default) refuses one, `relaxed` accepts it, a certificate naming another server and an expired one are refused either way, a connection opened under the old mode survives the setting changing under it, and the mode round-trips through `device_config.ini`;
- server URL/port rejection, exact opaque/numeric IDs (including >64-bit IDs), bounded JSON/token parsing, and malformed/oversized/auth-failed responses;
- music folders → indexes → directory and artists → artist → album, and 200-entry pagination;
- optional atomic 0600 credential persistence/round-trip, clearing remembered credentials, reporting a failed Forget instead of false success, rejecting symlinks/truncated identities/embedded NUL;
- fixed RAM capacity, millions of bytes across ring wrap, 50% refill hysteresis, protected initial header read/rewind vs a racing producer, retained-header rewind vs overwritten history, and 512 KiB pre-cache promotion;
- chunk extensions/trailers, transfer framing never reaching the reader, truncation/empty bodies/error pages, and retry after timeouts inside a chunk size, CRLF and trailer;
- cancellation during HTTP header setup, a stalled body, and reader starvation, followed by generation-safe reuse;
- nonmutating playlist next-track look-ahead across repeat/shuffle/ordered modes;
- exclusive decoder leases, both handoff/teardown orders, failed pre-cache retry suppression, repeat-one starting at byte zero, account-change invalidation with an outstanding lease, metadata lookup and unsupported raw codecs.

## Album Art Max's rows

`make -C sonix-player/tests aamlayout` checks the arithmetic of the now-playing
arrangement that trades the media controls for the artwork. The sleeve is
decoded at the size the layout gives it *before* anything has been laid out, so
the box cannot be read back off the widgets: it is its own module
(`src/gui/nowplaying/aamlayout.c`), handed the heights the board and the face in
force measured. The suite drives that module at the V1's numbers: the status
bar, the header row, the sleeve and the strip have to add up to the panel with
nothing lost or overlapped; Leftie has to cost exactly its rail of width and
move nothing else; and a panel too short or too narrow for its own rows still
has to answer with something drawable rather than a negative size. Plain C, no
LVGL and no display.

## Software volume

`make -C sonix-player/tests swvolume` checks the arithmetic of the software
volume -- the whole level law for a USB DAC with no control of its own. The
gain must mean exactly the decibels the USB route asked for, mute must be
exact, a deeply attenuated stream must stay quiet, clean and in range (rounded
and dithered, not truncated into grit), and the scaling must apply only while
the sound is leaving over the USB-C port. The three routing answers (Bluetooth,
a card on the port, that card's volume control) are stubbed; the curve, the
coefficients and the scaling are the real ones.

## Equaliser on PCM, including radio and USB DAC mode

`make -C sonix-player/tests eq` checks the chain radio stations share with a
track from the card. Internet radio does not go through `audio.c`: it decodes
on its own thread and writes through `audio_external_write()`, and
`radio_push_pcm()` runs `eq_process()` on the stereo 16-bit frames about to
be written. The suite links the real `eq.c` and drives that function: with
EQ, PEQ and MSEB all off it is a no-op; with each one on in turn a sine
changes; and every rate a station can arrive at runs -- 22.05, 24 and 32 kHz
for low-bitrate HE-AAC and speech streams as well as the usual 44.1 and
48 kHz, each with the all-off no-op checked alongside it. A mono station is
widened to an identical stereo pair before the chain, exactly as
`radio_push_pcm()` does, and the tuned output has to stay identical on both
channels. The config store is a temporary file; nothing else is stubbed.

USB DAC mode is the other source outside `audio.c`: the computer's PCM comes
off `/dev/uac_sa` as 32-bit left-justified words and `usbdac.c` runs
`eq_process_s32()` on them before they reach the DAC. The suite checks that
entry point too: with everything off it is a bit-for-bit no-op on those words,
and with a PEQ band boosted and MSEB moved off flat a 1 kHz tone comes out at
the same level as it does from the 16-bit path, within half a decibel, at every
rate the DAC answers to (44.1, 48, 96, 192 and 384 kHz) -- so a curve cannot
sound different over the cable than it does from the card.

## Heard podcast episodes

`make -C sonix-player/tests podcastplayed` checks the store behind the tick on a
podcast episode, against the real `podcast.db` on a temporary card. The set is
held sorted in memory and read by binary search on every row, so the suite
feeds it five hundred 64-bit catalogue ids in descending order and then asks
for every one of them, before and after a restart: a lookup that only worked on
the last insertion would fail here. It also checks that marking twice writes
one row, that unmarking removes it and the removal survives a restart, that an
id naming no episode (0, negative) is refused by both doors, and that a card
loses the database under the player and the store notices instead of answering
from the old set -- and makes the folder again for the next mark. The
significant four are the real ones; only `utils.c`'s file identity is
reproduced with `stat()`.

## Wi-Fi hidden networks

`make -C sonix-player/tests wifi` checks the one command that makes a hidden
network joinable. It links the real `wifi.c` and drives it against a fake
`wpa_cli` shell script placed first on `PATH` (the worker reaches it through
`execvp`, exactly as on the device): joining a hidden network has to write
`set_network <id> scan_ssid 1` -- before the passphrase, before the network is
selected -- for a network created on the spot and for one wpa_supplicant
already knows, and never for an ordinary visible network. A passphrase too
short to store has to be refused as a wrong passphrase without the supplicant
being told anything. The config store, sys_server and the power/clock helpers
are stubbed; `tests/stubs/` carries the two LVGL typedefs `power.h` needs so
this stays a plain-C suite. The Wi-Fi page itself -- the "Add hidden network"
row and the manual-entry page behind it -- is covered by `test_wifi_pages`
in the headless UI suite below.

## YouTube Music, against pages the device really received

`make -C sonix-player/tests ytmusic` links the real `ytmusic.c` against a fake
HTTP layer, so the resolver runs end to end on any host, and replays whole
responses the device received (`tests/fixtures/`): a search page, the charts,
the Explore page and the signed-out liked-songs page.

It pins what the resolver does with them: the reply is chosen by mime and
bitrate (m4a/AAC only, never a webm, never anything over the cap); the
phone-app player's refusal does not end the search, while a reply that is
playable is used as it stands -- that is the standalone path, and the fixture is
the reply the device's own request shape really got; and each fixture parses
into the rows the page showed
once the faults it guarded against were fixed -- twenty songs from a search
page, the charts, three *named* Explore carousels (a heading that failed to read
drew three rows all labelled Explore), the artwork URL of every card, and the
library page answering that the imported session is what Favourites is missing.

It also pins where the bytes are fetched from, because a relay's answer is not
always fetchable: an audio address on the relay's own base is used as it stands,
an address on somebody else's machine -- what a public Piped or Invidious
instance answers with, bound to that machine and too long for the player's URL
buffer -- is replaced with the relay's `/audio/<video-id>`, and a relay with no
such endpoint answers `ytmusic_relay_not_serving` instead of the old
"could not reach the stream".

Instances are pinned the same way. A pinned Invidious host's formats reply
(`/api/v1/videos/{id}`) is turned into that host's own
`/latest_version?id=...&itag=140&local=true` -- the endpoint that fetches the
audio and passes it on, which is the only instance shape that works from a
different machine; a Piped host is used only when its answer already points at
itself; and one that merely names a `googlevideo` address is passed over rather
than handed to the downloader. The settings list is checked too: the default,
the device on its own, the relay only once one is configured, and the built-in
instances, so the page offers something on a player that was never set up.

The order is pinned as well, because the order is what fixes a 403: an instance
is asked before the device in "auto", and the device's own playable reply is
still used when no instance can answer. And the downloader walks the same list --
the first instance refusing its bytes is passed over, the second one's file is
written and holds every byte it sent. That reply comes back in 1000-byte pieces
while 4095 were asked for, which is the proxy behaviour that once ended a track
after a minute: a short reply is not an ending, and neither is a `206` that carries
nothing -- that is the instance's upstream pausing, and the download asks again
from the byte it reached. The file is finished on a `416`, or after six empty
replies in a row, and is then checked against the `clen` the instance named. The
row's sleeve is fetched too and written beside the audio as `<id>.jpg`, so the
check covers the artwork the player looks for.

Playback starts before the download ends, and that is pinned as well: with the
same 1000-byte pieces, `ytmusic_stream_for_playback` calls its `ready` callback
once, at 3000 of the 4095 bytes, with the file still announced to `growfile` --
so the page may hand it to the player -- and by the time the call returns the
file is whole and no longer growing. That is what lets a song begin in seconds
instead of after the whole transfer, and the callback firing on a *partial*
file is the part that cannot be assumed. The fixture body starts with an `ftyp`
and a `moov` box for a reason: an M4A has no sample table until the index
arrives, so the early start waits for it, and a body without one would prove the
start happens but not that the decoder could open what it was handed.

The cache directory is `/tmp/sonix-ytmusic` on the device. The suite points
`SONIX_YTMUSIC_CACHE` at its own `.build/ytmusic-cache` first, because a Windows
host can refuse every create under `\tmp` (the CRT answers EACCES, and an
earlier version of these checks printed a skip line instead of running them).

The InnerTube key is a build input, never a file in the repository, so the suite
is built twice: `test_ytmusic` carries a dummy key (`test-key-not-a-credential`,
another value the fake network never sees), and `test_ytmusic_keyless` is built
with an empty one and checks the only thing that build can check -- that the
first search returns `ytmusic_api_key_missing` instead of sending a request the
endpoint can only refuse. Both are run by `make ytmusic`.

The settings the page stores are pinned too: the quality pill picks itag 141
(256 kbit/s AAC) when the instance offers it and 140 when the choice is
*Standard*; the region pair is stored and read back as two letter codes, with a
malformed pair refused; and the playback cache owns only its own `.m4a` files,
which is what decides whether the now-playing and row menus offer *Download*.
A song row is parsed for the album it names (`artist • album • duration`, one run
per piece, with the artist itself sometimes several runs), because that is what
names the folder a download lands in; and the downloader's progress reads back as
finished and unknown-percent once the file is whole, so the page's loading card
cannot stay up for a download that has ended.

A browser's `cookies.txt` export is imported as well as a single `Cookie` line:
the Netscape lines for `youtube.com` and `google.com` become the session header,
another domain's line is left out, and the card copy is removed afterwards.

## The YouTube Music relay

`make -C sonix-player/tests relay` builds nothing: it runs
`test_ytmusic_relay.py`, which drives the relay the player asks for when a track
is played (`tools/ytmusic-relay.py`) with its network taken out. The yt-dlp call
is a canned record, so the suite pins what the relay does with one: which streams
it will offer (m4a/AAC only, best first, never anything carrying a video track),
what URL it hands the player, and that the audio endpoint serves the upstream
bytes with the upstream's own `Content-Length` -- which is what the player's
download-complete check compares against.

What it exists for is the URL it hands over. The relay used to pass on the
googlevideo URL yt-dlp resolved, which the player then fetched from another
address: Google answers that with 403, and the URL is longer than the player's
own buffer. Both arrive at the device as "could not reach the stream". The
relay now serves the audio from itself, and the suite checks that the reply names
the relay -- the address the player already reached it on -- and stays short.

The player's side of that contract is checked in the C suite above: it never
fetches a media address the relay only pointed at, which is what made a public
instance look like a broken stream. Neither the relay nor an instance is needed
for playback any more -- both are fallbacks for when Google refuses the device's
own player request.

## Real codec integration

Install the normal host development libraries (Debian/Ubuntu example):

```sh
sudo apt-get install build-essential python3 pkg-config libopusfile-dev libwavpack-dev
make -C sonix-player/tests codecs
make -C sonix-player/tests sanitize-codecs
```

The codec suite links the actual player decoder dispatcher and bundled `dr_mp3`/`dr_flac`, plus **real libopusfile/libopus/libogg**. Other player codecs are also compiled/linked normally, not replaced with fake decoders. `PKG_CONFIG`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `BUILD` can be overridden for a custom dependency installation.

`media_fixtures.py` creates deterministic silent MP3 frames, lossless FLAC verbatim frames with real CRCs, and Ogg Opus silence packets with valid page CRCs and preskip/end trimming. No ffmpeg/encoder command or binary media fixture is needed. Tests check exact PCM frame counts (including MP3 Xing/LAME delay/padding and Opus preskip/end trim), buffered-source identity, seek/source-span exclusions, codec mismatch rejection and canceled-source opens. Large ID3/FLAC headers and >1 MiB FLAC transfers exercise forward header skipping and ring wrap without a file cache.

A no-ID3 MP3 fixture is intentional: the bundled `dr_mp3` discards its first ten bytes when given a NULL seek callback, losing the first audio frame. The production bridge supplies **limited retained-header seeks** and refuses an unknown-length whole-stream count; this test detects both regressions.

Both sanitizer targets use AddressSanitizer and UndefinedBehaviorSanitizer. At `-O1`, GCC reports pre-existing intentionally bounded/truncating MP4 tag copies; `sanitize-codecs` leaves that warning category nonfatal, while other warnings remain fatal. The core suite keeps all warnings fatal.

## Limits

The fixture proves transport, byte/sample counts and ownership behavior on the host. Scheduling helpers are stubs and there is **no ALSA/device loop** here, so these tests do not certify real-time scheduling, gapless analog output, Wi-Fi performance, target CPU use or battery life. The TLS cases run against the OpenSSL the host has (the player's own client code, `dlopen`ed exactly as on the device, sees the host's libssl 1.1/3.x); with no loadable OpenSSL they print `SKIP TLS` and the rest of the suite still runs. Real-network trust, redirects and certificate cancellation, plus UI interaction, should also be checked on hardware. See [the V1 checklist](../../docs/subsonic.md#validation).

## Headless main-menu appearance tests

```sh
# Only needed if the player build has not already fetched LVGL:
git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl.git sonix-player/lvgl
make -C sonix-player/tests ui -j"$(nproc)"
```

This optional suite links the real LVGL layout engine, main menu, shared grid,
Appearance controls, themes, language loader and configuration store. It uses
`tests/ui/lv_conf.h` to disable SDL, FreeType and device drivers, so a compiler
and the LVGL checkout are sufficient; no audio libraries or display are needed.
Deterministic built-in font metrics and unrelated navigation/playback services
are substituted, rather than running the player or touching device hardware.

It checks the default and saved **Main menu icons only** setting on both panel
sizes, all five themes, exact icon sizes and centering, glow-free icon-only
tiles, unchanged secondary-menu tiles, restoration of the labeled layout and
its decorative glows, real switch callbacks and config save/reload,
theme/accent/tint/font changes, hidden-label translation, and the six tile
destinations. The V1 language tree is used. Icon-only images must have
native-size sources and no scaling transform, including after font-metric
reflows. Actual LVGL snapshots verify pixel-for-pixel alpha edges, theme tints
and opaque pixels in ARGB8888 and the panel's RGB565 format; geometry alone
cannot catch a small bitmap being enlarged into a blurry icon.

To inspect rendered screens, optionally set `SONIX_MENU_SNAPSHOTS` to an output
prefix (its parent directory must exist), for example:

```sh
SONIX_MENU_SNAPSHOTS=.build/menu make -C sonix-player/tests ui
```

This writes ignored `.build/menu-<width>-theme<kind>.ppm` files using LVGL's
software renderer. To regenerate the main-menu-only native art, see
`tools/main_menu_icons.py`; Python image tools are not required to build or
run the player/tests. The host-build workflow runs this suite as well. These
checks do not replace visual and touch testing on a physical V1.

## Headless screensaver artwork tests

The same `ui` target also builds `test_screensaver`, which links the real
screensaver, scrolling labels, themes, settings card helpers, language loader
and configuration store. The player that lends the artwork, the cover loader
and the device services are substituted. The test drives the loader by hand,
so it decides when a decode lands, what size it is, or that it found nothing.

It checks the **Album art: Fill / Framed** choice, default
and saved:

- Fill is unchanged: the sleeve still covers its whole area, and it never
  asks for a decode. On the V1 that always means enlarging it.
- Framed asks for one copy of the artwork at exactly the frame's size (160 px
  on the V1), from the path the player read
  the artwork from. It asks as the panel goes dark, once the player has the
  artwork, and never again after a theme change or a failed decode.
- Until that copy lands, the player's sleeve stands in, never enlarged; a
  non-square Immersive crop is not used as a stand-in.
- The frame is centred in the area above the strip, with the margins, the
  player's rounded corners and no strip backdrop.
- The page behind the frame is right in all five themes.
- The time is white in every one of them, whatever accent the palette carries.
- A podcast episode is named by its own title with the podcast's author under
  it, and a file with nothing behind it falls back to the artist tag.
- Small pictures are shown at their own size.
- Track changes, abandoned decodes, switching back to Fill, switching to
  Pictures, dismissing and switching off release every borrowed and owned
  buffer.

Rendered RGB565 snapshots verify that every pixel of the frame outside its
rounded corners is the decoded picture's own pixel: drawn one to one, not
shifted, not resampled. The test pictures are one-pixel checkerboards, so any
scaling fails. The three new texts are checked in every language file, and
the captioned pill row is checked to relabel on a language switch.

The same test checks the size limit the screensaver hands the Screensaver
folder. It reads the size the empty-folder note promises from every language
file, requires them all to agree, and requires the limit to match: 480x800, the
size the V1's inherited texts promise, larger than its 240x320 panel.

Set `SONIX_SCREENSAVER_SNAPSHOTS` to an output prefix to keep the rendered
screens as ignored `.ppm` files, for example:

```sh
SONIX_SCREENSAVER_SNAPSHOTS=.build/saver make -C sonix-player/tests ui
```

## Calculator arithmetic and the notes folder

`make -C sonix-player/tests calculator` and `make -C sonix-player/tests notes`
build and run the two new pages' plain-C halves: no LVGL, no screen, the same
files the firmware compiles.

The harness supplies two font metrics, a small one and a larger one, so the
page can be checked to draw the calculator's symbols a step up instead of
merely differently styled.

`test_calculator` drives the model through the twenty keys: the display
following every key, a digit after a result starting a new sum, chained
operators applied as they are pressed, the sign on a result and on a number
being typed, percent, backspace to zero, ten digits and no more, the decimal
point entered once, division by zero reported and cleared, and a number too
wide for the display rounded down through the fallback formats instead of
being cut off.

`test_notes` covers the folder on a temporary card: what counts as a note
(including the folder wearing a note's name and the `.tmp` left by a write
that was interrupted), the A-Z, case-insensitive, digit-aware order, the name
a note is written under, names that must not reach the filesystem, the next
free name for a new note, what the editor will hold -- counted in characters
rather than bytes, so a note of three-byte letters is measured by the letters
-- and reading the files a computer leaves behind: UTF-8 with a byte order
mark, UTF-16, CRLF, tabs, Windows-1252 accents, control characters, an empty
file, and a file too long for the editor.

## The dates tags carry

`make -C sonix-player/tests tagdate` checks the reader of date-tag text
(`src/system/library/tagdate.c`), which the podcast lists order episodes and
write "12 March 2026" from. Every shape the taggers write is read: a bare
year, a year and a month, the ISO date with dashes or slashes, the compact
"20260312", one- and two-digit parts, and any of them carrying a time, a zone
or the far end of an ID3v2.4 range after it. Parts that name no real month or
day drop to whatever the tag did say ("2026-13" is a year), and a text with
no date in it -- including a day-first date, which this reader will not guess
at -- comes back empty. The fuller of two dates wins whatever order they
arrive in, and each date resolves to one moment for sorting with the parts it
left out settled on the first of January, and of the month.

## Most played, against the real database

`make -C sonix-player/tests mostplayed` checks the list the Music page's
**Most played** tile opens, against the real `library.c` and the real SQLite.
The card is a temporary directory and the scan's file-type probes (the CUE
sheet, MP4 and APE lookers) are stubbed; the schema, the play upsert
`device_state.c` runs at track start, and every query are the production ones.
The suite records plays by hand and reads the list back both ways the player
does -- the row-id handle the page opens and the streaming reader the queue is
built from -- and it pins three properties: plays are counted one per start
from the beginning, the list runs in descending play-count order and follows
the counts as they change without invalidating a queue built from the old
handle, and the counts survive closing and reopening the database (a rescan or
a reboot). It is a regression suite for a handle that answered zero rows over a
table with plays in it: the row-id pass named a `sortkey` column that
`PLAY_COUNTS` has never had, so the statement failed to prepare and the page
showed its "nothing to show" note whatever was counted.

## Headless calculator and notes page tests

The `ui` target also builds `test_tools_pages`, which lays the two new pages
out on the real renderer at the V1 panel size, with the Game Boy core compiled
in as the firmware does, so the More menu is laid out with the seven entries
the device has.

It checks that both destinations are on the More page in every theme -- the
tiles of Dark and Light, and the V1's single-column list under the palettes
built on function glyphs -- that nothing on either page is drawn outside the
panel, that every key is at least the size of the shell's own corner buttons
and every notes row a finger target, that the calculator's symbol keys are
drawn in the larger, bold font while its digits and the decimal point are not
and that every caption stays inside the key it is drawn on, that the calculator's display follows the
keys actually tapped and comes back holding the sum it was left with, that the
error state is drawn in the language's own words, that a note opens with its
file's text and its name as the title, that opening one is reading it -- no
keyboard, no caret, a tap on the text that changes nothing, and nothing
written to the card -- that the corner pencil is what turns the page into an
editor and the keyboard comes up with it, that leaving the editor with writing
unsaved leaves the writing on the card, that a note at the editor's ceiling
opens in the editor in a fraction of a second and whole, that a note past it
is shown by the text viewer instead, and that a new note is written and listed
before a single character is typed into it. Every language file is also
required to carry the two pages' strings: `tr()` falls back to the key it was
given, so a missing translation shows the tag itself rather than failing.

To inspect the pages, set `SONIX_PAGE_SNAPSHOTS` to an output prefix:

```sh
SONIX_PAGE_SNAPSHOTS=.build/page make -C sonix-player/tests ui
```

This writes ignored `.build/page-<board>-<page>-<theme>.ppm` files: one per
page, per More-page shape, per theme that draws one and per panel size.

## Screensaver folder rules

`make -C sonix-player/tests pictures` builds and runs `test_screensaverpics`,
plain C with no LVGL. It links the real folder code (`screensaverpics.c`) and
JPEG header reader (`jpeginfo.c`), and checks them on synthetic JPEGs in a
temporary card. Only headers matter here, so each file has a JPEG's segment
structure, with filler where the picture data would be.

- **Headers:** the size and coding are read even when the frame header sits
  behind about 93 KB of an editor's metadata (Exif with a thumbnail, a colour
  profile, XMP, Photoshop resources). The metadata filler contains frame
  markers of its own, as a real Exif thumbnail does, so a reader that looked
  inside a segment would be caught. Fill bytes, truncated files, scans with no
  frame, non-JPEGs and both extensions in either case are covered too.
- **Folder rules:** pictures over the limit, landscape or progressive are
  turned away, and so are hidden files (including a Mac's `._` companions),
  other names and missing files. Pictures made to the V1's note (480x800) are
  found with the V1's limit and not with its panel size, which is the fault
  this guards against.
- **Random pick:** it only ever returns a usable picture and reaches all of
  them. It avoids the picture already up while there is another, and falls
  back to it when it is the only one. Twenty `._` files beside one picture
  never make it miss.


## The battery guard

`make -C sonix-player/tests batteryguard` runs `batteryguard.c`, the rule with
no LVGL, no sysfs and no clock behind it: that five percent is where the red
card is raised, that one percent is where the player switches itself off the
ordinary way instead of letting the cell drop out from under it, that the
warning is not raised twice for the same discharge, and that readings under ten
percent keep the player out of standby. `power.c` reads the gauge and does the
two things; the rules it asks are checked here.

## The boot picture

`make -C sonix-player/tests bootpic` links the real Bootlogo folder code and the
player's own JPEG and PNG scalers. It checks which files the page offers (a
JPEG or PNG the boot script can draw, in either extension case, with hidden and
`._` files refused), that the copy written into the player comes out the other
end as a **240x320 baseline JPEG**, and that decoding that copy back gives the
picture it started from. A file too large to decode, or one that is not a
picture at all, is refused with the note the page shows rather than half-written.

## Tags, covers and the tag writer

`make -C sonix-player/tests tags` reads tags and sniffs formats from files
written in the test: ID3v2.2/2.3/2.4, APE and Vorbis comments, the Latin-1,
UTF-16 and UTF-8 encodings ID3 allows, unsynchronised frames, and the formats
the library has to recognise by their first bytes rather than their extension.
`make -C sonix-player/tests covers` drives the cover cache to its thousand-entry
ceiling and past it, including a card pulled out mid-decode.

## ALAC

`make -C sonix-player/tests alac` decodes a real Apple Lossless file and
compares the PCM with the samples it was encoded from. The fixtures are not
checked in — this project has no ALAC encoder — so the tests workflow generates
`.build/alac/alac.m4a` and `.build/alac/alac.s16` with ffmpeg first. The check
opens the file the way the player does (an MP4, with the codec read from the
track rather than the name), requires the decoder to report **ALAC**, 44.1 kHz,
stereo and 16 bits, and then compares every sample: a one-sample tolerance
covers the 16-bit round trip and nothing else. An encoder's priming samples
are allowed for by finding the alignment first. Without a fixture folder the
target prints `skipped` and passes, so a host without ffmpeg still builds.

`make -C sonix-player/tests tagwrite` is the Cover Art Fetcher's writing half:
it replaces the APIC frame of an MP3 and the PICTURE block of a FLAC, keeps
every other frame and the audio bytes identical, refuses ID3v2.2, OGG, Opus and
MP4 by name instead of half-rewriting them, and leaves a temporary file rather
than a damaged song when the write is interrupted.

## Lyrics search

`make -C sonix-player/tests lrclib` covers the LRCLIB client: the request it
builds from a title and artist, the ranking of what comes back (a synced lyric
outranks a plain one, an exact title and artist outranks a near miss), the
sidecar file saved beside the track, and the refusal of an answer that names
nothing.

## ListenBrainz

`make -C sonix-player/tests listenbrainz` links the real scrobbler: what a
listen looks like on the wire, the queue that holds one while the player is
offline, the token stored and cleared from the card, and that listening with the
module switched off sends nothing at all.

## FLAC radio streams

`make -C sonix-player/tests radioflac` runs the real radio decoder against FLAC
stations: a native stream, one joined mid-way (the first frames of a stream
that started before the player arrived), and a chained Ogg FLAC stream, which
is the shape most FLAC radio serves. `sanitize-radioflac` is the same suite
under ASan and UBSan.

## EarFun earbuds

`make -C sonix-player/tests earfun` drives the control protocol without a pair
of earbuds: the frame encoder and the decoders, with every socket call in the
worker stubbed. It checks the GAIA-shaped packets the page's switches send
(noise control, its strength, transparency, Game Mode, in-ear detection) and
that a reply the earbud sends back becomes the state the page draws -- including
a truncated or unknown frame, which has to leave the last state alone rather
than report a value nobody sent.

## Smart Mix sizes

`make -C sonix-player/tests genius` links the real mix generator to a synthetic
memory-only similarity graph. It checks every popup size (10, 15, 20, 25, 30),
the seed counting toward the requested size, unique independently owned output
paths, invalid sizes, missing seeds, cancellation/card invalidation, shorter
mixes when candidates run out, and failure when only the seed is available.
No LVGL, database or card is required.
