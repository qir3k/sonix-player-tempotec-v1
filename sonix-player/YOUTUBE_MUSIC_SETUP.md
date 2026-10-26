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

## What the page does

- **Search** — songs. A row plays; an album or playlist row opens its own page.
- **Explore** — the store's front page, one row per carousel ("New albums &
  singles", "Trending", …). Tapping a carousel lists its cards without another
  request.
- **Top songs** — the charts page.
- **Favourites** — the liked songs of the signed-in account. Needs Option 1 or 2.
- **Podcasts** — a podcast search. A show opens its episode list.

Selecting a track resolves an audio-only AAC stream, caches it temporarily under
`/tmp/sonix-ytmusic`, and hands that file to the player's existing decoder, which
reads both flat and fragmented (DASH) MP4 — the form YouTube serves.

Playback uses the phone app's player endpoint, which answers with plain,
un-ciphered media URLs. The web client's reply is ciphered and needs a
JavaScript engine the device does not have, and it also demands a signed-in
session, so it is only tried first when a session has been imported; the phone
client answers either way. Nothing here requires signing in.

Search and chart results inside explicitly marked ad, sponsored or promoted
renderers are skipped, and before playback the player checks that YouTube
returned the requested video and marked it playable. Only audio formats are
used, never ad-placement media. This is a narrow audio-playback safeguard, not a
network-wide ad blocker, and it does not include SponsorBlock.

## When a page comes back empty

An account Google will not honour is answered with an empty page rather than an
error. For the public pages the player notices that and asks again as a
signed-out visitor, so search, Explore and Top songs keep working. Favourites
has no such fallback: if it is empty while the sign-in still shows, import a
fresh session.
