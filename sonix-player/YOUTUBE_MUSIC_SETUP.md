# YouTube Music setup

The player uses Google's device authorization flow. You approve access once on
a phone; the player stores the returned refresh token locally and refreshes its
short-lived access token when needed. No computer companion service is needed
after setup.

## Create personal OAuth credentials

1. Create or choose a project in the [Google Cloud Console](https://console.cloud.google.com/).
2. Enable the **YouTube Data API v3** for that project.
3. Create an OAuth client with the application type **TVs and Limited Input
   devices**. The device flow is intended for devices with limited input and
   supports the `youtube.readonly` scope used by this player. See Google's
   [device authorization guide](https://developers.google.com/youtube/data/guides/auth/devices).
4. Copy the client ID and client secret into `streaming-keys.ini`:

   ```ini
   [ytmusic]
   client_id = YOUR_CLIENT_ID.apps.googleusercontent.com
   client_secret = YOUR_CLIENT_SECRET
   ```

Keep this file private. Put `streaming-keys.ini` in the root of the microSD card
and restart the player. The card can add YouTube Music credentials even when
the firmware already has credentials for other streaming services; bundled
values are kept. The example format is in `streaming-keys.ini.example`.

## Sign in on the player

Open **Streaming → YouTube Music → Sign in with Google**. Scan the QR code or
open the displayed address on a phone, enter the code shown on the player, and
approve access. Leave the player on the sign-in screen until it confirms the
sign-in; it must receive and save the refresh token before it can be powered
off.

The session is saved in `/usr/data/ytmusic_session.ini` with restricted file
permissions. After reboot, the player refreshes its access token automatically
when YouTube Music needs it. Signing out removes the local session.

Search and the Top Songs chart browse are available in the YouTube Music page.
Selecting a result resolves an audio-only AAC stream, caches it temporarily,
and hands that audio file to the player's existing decoder. The chart and
playback requests use YouTube Music's unofficial InnerTube interface, which
Google can change without notice. Search and chart results inside explicitly
marked ad, sponsored, or promoted renderers are skipped. Before playback, the
player also checks that YouTube returned the requested video and marked it
playable; only its audio formats are used, never ad-placement media. This is a
narrow audio-playback safeguard, not a network-wide ad blocker, and it does
not include SponsorBlock.
