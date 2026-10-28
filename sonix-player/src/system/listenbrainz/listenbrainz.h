#ifndef LISTENBRAINZ_H
#define LISTENBRAINZ_H

#include <stdbool.h>

// Scrobbling to ListenBrainz.
//
// The GUI thread reports what is playing once a second with
// listenbrainz_note_playback(), and this module decides what counts as a play
// and when it has been listened to long enough to be a listen: half its length
// or four minutes, whichever comes first, for a track of at least 30 seconds
// -- ListenBrainz's own rule, the same one Last.fm's is.
//
// A worker thread does the network and the files. "Now playing" goes out when
// there is Wi-Fi at the start of a track. Listens go into a queue in .local
// on the card and are sent in batches whenever the Wi-Fi is up. Nothing here
// turns the Wi-Fi on, and the radio is held up only while a request is on the
// wire.
//
// ListenBrainz needs no application credentials: the account is a personal user
// token, requested on listenbrainz.org/settings and pasted in the settings
// page. Only the token is kept, beside device_config.ini; it authenticates
// every request in the Authorization header.
//
// The two scrobbling services are independent: both can be on at once, each
// keeps its own token, its own queue on the card and its own worker.

typedef enum {
	LISTENBRAINZ_STATE_OFF,
	LISTENBRAINZ_STATE_SIGNED_OUT,
	LISTENBRAINZ_STATE_CONNECTED,
	LISTENBRAINZ_STATE_TOKEN_EXPIRED, // ListenBrainz refused the token: sign in again
	LISTENBRAINZ_STATE_REFUSED,       // ListenBrainz refused a request; `detail` says why
} listenbrainz_state_t;

typedef struct {
	listenbrainz_state_t state;
	bool enabled;
	char detail[160]; // ListenBrainz's own message or the transport error, in English
	int queued;
} listenbrainz_status_t;

// Reads the token and the queue and starts the worker. Call once, after
// config_init(). There are no API keys to wait for, so the module is always
// available once this has run.
void listenbrainz_init(void);

// Stored in the configuration. GUI thread.
void listenbrainz_set_enabled(bool on);

// Switched on with a token: plays are being counted.
bool listenbrainz_active(void);

// Both return at once; the worker does the work. The token is wiped as soon as
// the request has been built.
void listenbrainz_signin(const char *token);
void listenbrainz_signout(void);

void listenbrainz_get_status(listenbrainz_status_t *out);

typedef struct {
	bool playing;
	const char *file; // the loaded file, "" for none
	double position;  // seconds
	double duration;  // seconds, 0 when unknown
	const char *artist;
	const char *title;
	const char *album;
	bool skip; // radio, audiobooks and podcasts are never scrobbled
} listenbrainz_playback_t;

// GUI thread, about once a second. Tags may arrive empty on the first calls
// for a new file and be filled in later.
void listenbrainz_note_playback(const listenbrainz_playback_t *now);

// True while a request is on the wire, so the Wi-Fi is not parked under it.
bool listenbrainz_network_wanted(void);

#endif /* LISTENBRAINZ_H */
