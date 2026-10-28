# YouTube Music setup

The player talks to YouTube Music the way the web client does: its unofficial
InnerTube interface (`music.youtube.com/youtubei/v1`) with the `WEB_REMIX`
client. That interface is not a public API, and Google can change it without
notice.

Two things about it shape this page:

- **Browsing needs no account.** Search, Explore, Top songs and podcast search
  are public pages and answer whether or not anyone has signed in.
- **The library needs a real browser session.** Liked songs, your playlists and
  your podcast subscriptions belong to an account, and the only credential
  InnerTube honours for them is the cookie a signed-in browser sends.

So there are two ways to attach an account, and the page works without either.

## Option 1: import a session from the browser (recommended)

This is the credential the music endpoints actually accept.

1. Sign in to <https://music.youtube.com> in a desktop browser.
2. Export the site's cookies with the browser's own export (a `cookies.txt` in
   the Netscape format, which is what "Get cookies.txt LOCALLY" and friends
   produce). Or, by hand: open the developer tools, go to **Network**, reload
   the page, pick any request to `music.youtube.com` and copy the whole `Cookie`
   request header value — one line is enough.
3. Save it as `ytmusic_session.txt` in the root of the microSD card. Both shapes
   are read: every `youtube.com` / `google.com` line of a Netscape export is
   collected into a `Cookie` header, and anything else is taken as one header
   value already.
4. On the player open **Streaming → YouTube Music → the cog → Import a session
   from the card** and tap it.

The card copy is deleted once the session has been saved. The player keeps it in
`/usr/data/ytmusic_session.ini` with mode 0600, beside `device_config.ini`, and
reloads it at boot. The value must contain `SAPISID`, `__Secure-3PAPISID` or
`__Secure-1PAPISID`; anything else is rejected rather than stored.

A cookie expires when the browser session ends or when Google rotates it. If
Favourites suddenly comes back empty, import a fresh one.

## Option 2: sign in with Google on the player

The player can also run Google's device authorization flow, the same flow the
`ytmusicapi` project uses. This needs your own OAuth client:

1. Create or choose a project in the [Google Cloud Console](https://console.cloud.google.com/).
2. Enable the **YouTube Data API v3** for that project.
3. Create an OAuth client of type **TVs and Limited Input devices**.
4. Copy the client ID and client secret into `streaming-keys.ini` in the root of
   the microSD card:

   ```ini
   [ytmusic]
   client_id = YOUR_CLIENT_ID.apps.googleusercontent.com
   client_secret = YOUR_CLIENT_SECRET
   ```

Keep this file private; the example format is in `streaming-keys.ini.example`.
The card can add YouTube Music credentials even when the firmware already has
credentials for other services.

Then open **Streaming → YouTube Music → the cog → Sign in with Google**. Scan
the QR code or open the address shown, enter the code on the player, and approve
access. Leave the player on that screen until it confirms the sign-in: it has to
receive and store the refresh token first. The token is refreshed automatically
whenever it expires.

The flow asks for the full `https://www.googleapis.com/auth/youtube` scope. The
narrower `youtube.readonly` scope is refused by the music endpoints: a request
carrying such a token comes back empty instead of failing, which is why a
session signed in that way used to make search return nothing. If a player was
set up before this change, sign in again so the new scope is granted.

**This token alone does not fill Favourites.** The music endpoints refuse a
Google sign-in token when it is asked for the library and answer with an empty
page rather than an error, so the liked songs and your own playlists need the
imported session of Option 1. Where the player is signed in that way and
Favourites is opened, it now says what is missing instead of showing an empty
list.

## What the page does

- **Search** — songs. A row plays; an album or playlist row opens its own page.
- **Explore** — the store's front page, one row per carousel ("New albums &
  singles", "Trending", …). Tapping a carousel lists its cards without another
  request.
- **Top songs** — the charts page.
- **Favourites** — the liked songs of the account. Needs Option 1: the imported
  browser session is the only credential Google's music endpoints accept for the
  library, and where it is missing the player says so rather than listing
  nothing.
- **Podcasts** — a podcast search. A show opens its episode list.
- **Artwork** — every row draws the sleeve its entry came with, the same shared
  cover loader the Tidal, Qobuz and Podcasts tabs use. The images are kept on
  the card under `.local/qobuz-art`, so a second look at a list downloads
  nothing.

Selecting a track resolves an audio-only AAC stream, caches it temporarily under
`/tmp/sonix-ytmusic`, and hands that file to the player's existing decoder, which
reads both flat and fragmented (DASH) MP4 — the form YouTube serves. The bytes
are fetched in 256 KB ranges rather than as one long transfer: an unsigned
`googlevideo` address is throttled to about 32 KB/s when read straight through,
and not throttled at all when it is read in ranges, which is the difference
between a couple of seconds and a couple of minutes for one track.

A short or empty range reply is **not** the end of the file. Invidious passes the
bytes on from Google and hands back less than was asked for whenever its own
upstream pauses; taking that for an ending is what used to finish a three-minute
track after a minute, at a file size the player then reported as a 35 kbit/s
stream. The download now asks again from the exact byte it reached, stops only
on a `416` (or after six empty replies in a row), and checks the total against
the `clen` the instance named before calling the file whole.

## Settings

All of these are on **YouTube Music → the cog**:

- **Sleep timer** — the music transport's own timer, the one the card and Tidal
  share, so a stretch set here stops a song at the same point it stops an album.
  A YouTube Music track plays through the same player, so it is the same countdown.
- **Audio quality** — *High* takes the 256 kbit/s AAC stream (itag 141) where the
  instance offers one and the 128 kbit/s one elsewhere; *Standard* pins 128, which
  is half the bytes and the way back when a public instance is too slow to carry
  the better one.
- **Recommendations for** — the country whose store answers, and the language the
  row names come back in: *Worldwide (US)*, *Poland*, *Poland, English rows*,
  *Czechia*, *Germany*, *United Kingdom*. Both parts go into the InnerTube
  `gl` / `hl` of every search, browse and player request, so choosing Poland
  while reading English gives Polish charts with English titles.
- **Home rows** — a page of its own: up and down move a row, and the switch
  beside it decides whether the home page shows it at all. Hiding Explore is the
  point; the order is kept in the config and read again whenever the home page is
  built.

## While a song is loading

A track is downloaded whole before the player is handed it, which over a public
instance is tens of seconds. The page shows a card with a spinner, the progress
in percent and a bar, driven by the downloader's own byte counter -- so a slow
start is visibly working rather than indistinguishable from a tap that did
nothing. The card takes the taps while it is up, so a second press cannot start a
second download behind the first. The percent appears as soon as the source says
how large the file is, and the card goes the moment the player starts or the
download fails.

The album in the folder path comes from the row YouTube sent (its second column
is `artist • album • duration`) and, where a page does not name it -- an album's
own track list, a playlist -- from the title of the list the song was opened in.
Only when neither exists does the file land under `Unknown album`.

## Saving a song to the card

Once a track has played, open the now-playing menu (the track's own menu, the
same place Tidal and Qobuz offer it) and choose **Keep on card**; a song's row
has the same **Download** entry behind its three dots, without playing it first.
Either way the file is copied out of the temporary cache into
`<card>/Music/YouTube Music/<artist>/<album>/<title>.m4a`, with its `.tags`
sidecar and its sleeve beside it, so the next library scan lists it as an
ordinary track and it plays with no network at all.

## Playback: the device, an instance, or a relay

Google demands a browser-grade attestation — a PO token its JavaScript computes —
before it hands out media, and this device has no JavaScript engine. The
phone-app player request still answers with plain `https` audio addresses, but
when this device fetched one Google refused every media request with 403 —
ranged or whole, and whatever the address. Those refusals are in the player's
own log as `http: media answered 403: …`, with Google's reason in the body. So
the device's own request is no longer the first answer, and a track does not
depend on it.

The order a track is played in, and all of it is done by the player itself: no
PC, no laptop, no phone.

1. **The imported browser session**, when there is one: the web music player
   knows the account's region and, with Premium, its own stream.
2. **An instance** — a Piped or Invidious server. An Invidious instance fetches
   the audio from Google and passes the bytes on through
   `/latest_version?id=…&itag=…&local=true`, so the player only ever talks to
   the instance. This is the path that plays today: measured against a real
   track, `yt.chocolatemoo53.com` answered the formats reply and then served the
   audio in ranges (206 with the matching `Content-Range`, a 3.4 MB track).
3. **The device itself**, through the phone-app player — kept for the day Google
   stops refusing what it hands out.
4. **The relay** — `tools/ytmusic-relay.py` on a machine of your own. Last,
   because it needs another machine.

A source that resolves an address the download is then refused does not end the
attempt: the downloader moves to the next source in the list. Every attempt is
in the log — `asking …`, `fetching from …`, `… kept 0 bytes (failed, …)`,
`played from …` — and the media status is followed by the body of the refusal,
so a failed play names the host that refused instead of saying "could not reach
the stream".

The download is finished when the server has nothing left to send, not when a
reply is short: a proxy handing back less than was asked for is normal, and
taking that for the end of the file is what once cut a three-minute track off
after a minute — at a file size the player then reported as a 35 kbps stream.
So the next range is asked for from where the last one stopped, a dropped
connection gets one more try at the same offset, and the result is checked
against the byte count the instance's own reply gives (`clen`), which the log
names along with the format it chose: `offers itag 140 at 128 kbps`.

The sleeve comes from the row: its thumbnail is fetched at 320 px and written
beside the cached track as `<id>.jpg`, which is where the player looks for a
file's artwork — so now-playing shows the cover the list showed, and not the
placeholder glyph.

**Playback source** in the settings page (Streaming → YouTube Music → the cog)
shows what is in force and opens the list: *Automatic* (the default, the order
above), *This device only*, *Relay on my network*, or any instance. A pinned
source is tried first and the rest are still tried after it, because a pinned
host being down should not mean silence.

### Instances

```ini
[ytmusic]
source = https://invidious.example      # pin one; "auto" is the default
instances = inv.example, piped.example   # replace the built-in list
```

in `/usr/data/device_config.ini` (not `streaming-keys.ini`, which holds the
Tidal and Qobuz keys) — the same file the player writes its own settings to.
`instances` takes a comma or space separated list, replaces the built-in hosts
instead of adding to them, and accepts a bare host name, read as `https://`.

Both keys can be set from the player instead: Streaming → YouTube Music → the
cog → **Playback source**, which lists *Automatic*, *This device only*, *Relay on
my network* and every instance. Tapping one stores it and marks it *On*.

Measured when the built-in list was last checked, against a real track: every
Piped host on the published list (including `pipedapi.kavin.rocks`) answered
502, 403 or nothing at all, and of the Invidious hosts, `inv.nadeko.net` and
`invidious.tiekoetter.com` answered 403, `invidious.nerdvpn.de` answered 401 —
only `yt.chocolatemoo53.com` answered the API *and* served audio. That is why it
is first in the built-in list, and why a host of your own, pinned in the
settings, is the reliable version of this: instances come and go in weeks.

What an instance can contribute depends on what it does with the bytes. A Piped
instance answers `/streams/<id>` with addresses on Google's own hosts, which
belong to the instance's address and are refused when fetched from here — so a
Piped instance only helps when its answer already points at itself. An Invidious
instance is asked for its formats reply, and the bytes are then fetched through
its own `/latest_version?id=…&itag=…&local=true`, which makes the instance fetch
the audio and pass it on: that is the shape that works from another machine.

That last step is a redirect chain, not a file: `/latest_version` answers `302`
to a `companion/…` address on a second host of the same instance, and that one
answers `302` to the `videoplayback` address it proxies — 2256 characters when
this was measured, `expire` and signature included. The player follows both hops
and keeps the `Range` header across them. It also no longer follows a truncated
one: a `Location` longer than the address buffer is refused with
`http_redirect_too_long`, because cutting that signed address at 2047 characters
— what the old 2048-byte buffer did — makes the host answer `500`, and that used
to reach the screen as "could not reach the stream" from every instance, however
healthy.

Nothing here logs in to an instance: the player never sends a Google password
anywhere, and the only account it knows is the imported browser session above.
An instance that answers 401 or 403 is simply passed over, and the next source
is asked. Self-hosting Invidious is what makes this permanent, and its address
then goes in `instances` or in **Playback source**.

### The relay

The relay is for anyone who would rather run one job on their own machine than
depend on an instance. `sonix-player/tools/ytmusic-relay.py` is a single file,
on any machine the player can reach. It needs `yt-dlp`, which runs the
attestation:

```sh
pip install yt-dlp
python ytmusic-relay.py            # listens on 0.0.0.0:8010
```

It answers the two endpoints the player asks for. `/streams/<video-id>` is the
Piped-shaped reply — title, artist, duration, and the one audio stream to fetch —
and `/audio/<video-id>` is those audio bytes, fetched by the relay and passed on.

That second endpoint is the point: the media URL `yt-dlp` resolves is bound to
the address that resolved it, and it is several thousand characters long,
past what the player's own URL buffer holds. A short URL on the LAN has neither
problem, and the audio-only filter stays where it was: the relay offers m4a/AAC
only, and never fetches the video.

What the player does with the reply: an audio address on the relay itself is
fetched as it stands, and one that points anywhere else — a public instance's
`googlevideo` URL — is not fetched at all, because fetching it cannot work; the
player asks the relay for `/audio/<video-id>` instead. A relay that answers with
neither is named as that ("This relay hands out YouTube addresses the player
cannot fetch"), rather than blamed as a broken stream, and the device log records
the address that was tried and why it failed. The relay's own console prints a
line per request (`relay: "GET /audio/..." 200 -`), which is the first place to
look when a track does not start. When every source has been refused, the
message says so ("YouTube refused the audio and no instance answered") instead
of asking for a relay, because a relay is no longer required.

Search and chart results inside explicitly marked ad, sponsored or promoted
renderers are skipped, and before playback the player checks that YouTube
returned the requested video and marked it playable. Only audio formats are
used, never ad-placement media. This is a narrow audio-playback safeguard, not a
network-wide ad blocker, and it does not include SponsorBlock.

## When a page comes back empty

An account Google will not honour is answered with an empty page rather than an
error. For the public pages the player notices that and asks again as a
signed-out visitor, so search, Explore and Top songs keep working. Favourites
has no such fallback: a signed-in player whose session cannot serve the library
is told to import one, and an imported session that has expired — they do —
does it with a fresh one.
