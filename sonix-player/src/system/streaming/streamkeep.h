#ifndef STREAMKEEP_H
#define STREAMKEEP_H

#include <stdbool.h>

// A streaming track kept on the card for good.
//
// Tidal and Qobuz are played from a download into their own cache
// (.local/tidal-cache, .local/qobuz-cache), and that cache is transient by
// design: it is pruned as it fills and emptied on the way out, because a hidden
// folder carrying a gigabyte of tracks the user never asked for is not theirs to
// find. This is the other half: a deliberate "keep this one", which copies the
// finished download into the library proper --
//
//     <card>/Music/<Tidal|Qobuz>/<artist>/<album>/<nn> <title>.<ext>
//
// -- with its tags sidecar and its cover beside it, so the next scan finds an
// ordinary track with an ordinary title, artist and sleeve. From then on it
// behaves like anything else on the card: no network, no cache sweep, playable
// in the file browser at once.

// Whether `path` is a track this module can keep: something the Tidal or Qobuz
// cache owns, not a file the user put on the card.
bool streamkeep_is_stream_track(const char *path);

// Copies the track out of the cache, on a worker thread. False when the path is
// not a cache track, or when another keep is already running. The toast that
// says how it went is posted to the GUI thread by the worker itself.
bool streamkeep_keep(const char *path);

// Whether a keep is running. One at a time: they are card copies, and the card
// is also what is being read from for playback.
bool streamkeep_busy(void);

#endif /* STREAMKEEP_H */
