#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

#include <stdbool.h>

// The sleep timers: the player stops by itself after a while.
//
// Two of them, not four. One covers everything you listen to -- the card, Tidal,
// Qobuz, YouTube Music, podcasts and radio -- because "stop in half an hour" is
// one thought about one evening, not a fact about where the audio came from.
// Four timers meant a stretch set at bedtime stopped counting the moment a
// station was opened instead of an album, which from the outside is a timer
// that stopped working, and it put four separate switches in the control
// centre for one intention. Podcasts and radio are aliases of the music timer
// now; the settings pages that still have a row set the same one.
//
// The audiobook stays apart, and for a reason that is not taste: falling asleep
// to a book is measured against the end of a chapter, and a book somebody
// resumes the next morning should not have inherited the album's countdown.
//
// A station has no pause -- stopping it is a disconnection -- so when the
// shared timer expires on one it is stopped rather than paused, which is what
// somebody falling asleep to it wants anyway. The player decides that: see
// sleep_timer_cb().
//
// One rule decides when any of them counts. The countdown starts fresh at every
// transition into playing and runs only while something of that kind is
// playing -- so pausing and starting again gives a full stretch back, and so
// does pressing play after the timer has stopped the music. There is no
// "resume where the countdown was": somebody who presses play is awake, and a
// timer that gave them the four minutes left over from last time would be a
// timer that had stopped doing its job.
//
// Which kind is playing is not this file's business to work out: the player
// knows, and tells it. The Game Boy and the Bluetooth receiver are neither of
// the four -- there is no pause to give them -- so the player simply never names
// them.
//
// Each counts down against the monotonic clock rather than by adding up ticks,
// so a slow tick with the screen dark loses nothing: the tick only asks what
// time it is now.
typedef enum {
	SLEEPTIMER_MUSIC = 0, // the card and every streaming service: one transport, one timer
	SLEEPTIMER_AUDIOBOOK,
	// Kept as names because the pages and the player name them, but they are
	// the music timer: one evening, one countdown, wherever the sound is from.
	SLEEPTIMER_PODCAST = SLEEPTIMER_MUSIC,
	SLEEPTIMER_RADIO = SLEEPTIMER_MUSIC,
	SLEEPTIMER_COUNT = 2,
} sleeptimer_kind_t;

// Reads all three out of the config.
void sleeptimer_init(void);

bool sleeptimer_enabled(sleeptimer_kind_t kind);
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on);

// How long a stretch is, in minutes. Zero means the switch is on but no time
// has been chosen, which arms nothing -- the wheels sitting at 00:00 are not a
// request to stop immediately.
int sleeptimer_minutes(sleeptimer_kind_t kind);
void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes);

// Which kind is playing, or SLEEPTIMER_COUNT for none of them. Told from the
// one place in the player that sees every change of playback state; naming a
// kind stops whichever other one was counting.
void sleeptimer_note_playing(sleeptimer_kind_t kind);

// True while that stretch is actually counting down: the switch is on, a time
// is set, and that kind is playing.
bool sleeptimer_running(sleeptimer_kind_t kind);

// True for any of the three, for a caller that only wants to know whether its
// tick is worth running.
bool sleeptimer_any_running(void);

// True exactly once, on the tick that kind's time runs out. The caller stops
// the music: this file has no business knowing how.
bool sleeptimer_expired(sleeptimer_kind_t kind);

// What is left of that stretch, in seconds, or 0 when it is not counting.
int sleeptimer_remaining(sleeptimer_kind_t kind);

#endif /* SLEEPTIMER_H */
