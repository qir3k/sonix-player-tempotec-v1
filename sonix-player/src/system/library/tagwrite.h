#ifndef TAGWRITE_H
#define TAGWRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Replacing the cover picture a file carries in its own tags.
//
// albumart.c reads a picture out of an ID3v2 APIC frame (MP3), a FLAC PICTURE
// block, an MP4 `covr` atom and the rest, and prefers it over a cover file
// beside the track. So a `cover.jpg` cannot change the sleeve of a file that
// already has one: the only way is to write the tags. This is that writer, for
// the two formats a music library on a card actually arrives in -- MP3 and
// FLAC -- and nothing else: a half-understood rewrite of an MP4 or an Ogg file
// is a corrupted album, and refusing is the better answer.
//
// A rewrite is done the safe way: a new file is written beside the old one with
// the same audio bytes and the new picture, and only a complete, closed file
// replaces the original. A card pulled out mid-way leaves the old file whole
// and a temporary to be cleaned up.
//
// Blocking, file-heavy: call from a worker thread, never from the interface or
// the audio thread.

// Whether this file's tags can be rewritten: MP3 (ID3v2.3/v2.4) or FLAC.
// Anything else -- including an MP3 with an ID3v2.2 tag -- answers false, and
// the caller says so instead of pretending.
bool tagwrite_supported(const char *path);

// Replaces the pictures in the file's tags with `data` -- every picture frame
// goes, not only the front cover, and the one written takes its place -- and
// keeps the rest of the tag as it was. `data` may be a JPEG or a PNG; it is
// sniffed, not trusted. True when the file was rewritten; false with a tag in
// `error` otherwise, the original file untouched.
//
// The interface this backs is the "Write artwork into tags" module in Settings
// -> Plugins: with it off the artwork search writes a cover file beside a track
// that has none and refuses on one that carries its own.
bool tagwrite_set_cover(const char *path, const void *data, size_t size, char *error,
						size_t error_size);

#endif // TAGWRITE_H
