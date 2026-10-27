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
2. Open the developer tools, go to **Network**, reload the page and pick any
   request to `music.youtube.com`.
3. Copy the whole `Cookie` request header value, or run `document.cookie` in the
   console and copy the result.
4. Paste it into a plain text file holding a single line, and save it as
   `ytmusic_session.txt` in the root of the microSD card.
5. On the player open **Streaming → YouTube Music → the cog → Import a session
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

## Playback: the device, an instance, or a relay

Google demands a browser-grade attestation — a PO token its JavaScript computes —
before it hands out media, and this device has no JavaScript engine. What it
does have is the phone-app player request, which still answers with plain
`https` audio addresses the device can fetch itself. That is the default: a
track plays with nothing else on the network, no account, no helper machine.

When Google refuses that request, the player falls back, in this order:

1. **An instance** — a Piped or Invidious server. The settings page lists the
   built-in public hosts and anything in `[ytmusic] instances`, and one can be
   pinned so it is asked first.
2. **The relay** — `tools/ytmusic-relay.py` on a machine of your own.

**Playback source** in the settings page (Streaming → YouTube Music → the cog)
shows what is in force and opens the list: *Automatic* (the default, the order
above), *This device only*, *Relay on my network*, or any instance. A pinned
source is tried first and the rest are still tried after it, because a pinned
host being down should not mean silence.

### Instances

```ini
[ytmusic]
source = https://invidious.example     # pin one; "auto" is the default
instances = inv.example, piped.example  # replace the built-in list
```

in `/usr/data/device_config.ini` — the same file the player writes its own
settings to. The settings page writes both keys, so neither has to be edited by
hand; a bare host name is accepted and read as `https://`.

What an instance can contribute depends on what it does with the bytes. A Piped
instance answers `/streams/<id>` with addresses on Google's own hosts, which
belong to the instance's address and are refused when fetched from here — so a
Piped instance only helps when its answer already points at itself. An Invidious
instance is asked for its formats reply, and the bytes are then fetched through
its own `/latest_version?id=…&itag=…&local=true`, which makes the instance fetch
the audio and pass it on: that is the shape that works from another machine.

Most public instances are refused by YouTube on any given day (403 to every
request), which is why this is a fallback and why a self-hosted instance is the
reliable version of it. Nothing here logs in to an instance: the player never
sends a Google password anywhere, and the only account it knows is the imported
browser session above.

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
