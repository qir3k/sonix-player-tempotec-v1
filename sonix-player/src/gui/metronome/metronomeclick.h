#ifndef GUI_METRONOME_METRONOMECLICK_H
#define GUI_METRONOME_METRONOMECLICK_H

#include <stdbool.h>

// The metronome's click.
//
// A thread of its own, writing into the external PCM (audio.h) the way the
// emulator and Flappy Bird do: one PCM takes one writer, so starting the
// metronome stops local playback first, exactly as those two do. The click
// itself is two synthesised bursts -- a bright one on the downbeat, a lower one
// on the beats between -- so there is nothing to decode and nothing to ship in
// the resource tree.
//
// The tempo is kept by counting frames rather than by sleeping: the write of
// one block blocks until the DAC has room, which paces the loop at exactly the
// rate the samples are played, and the onset of a click is placed on a sample
// within the block rather than on a block boundary. A metronome's whole value
// is that it does not drift, so nothing here rounds the beat's position.
//
// The page reads the beat back: metronomeclick_beats() counts the clicks that
// have been written, and metronomeclick_beat_index() says which beat of the bar
// the last of them was.

// Starts clicking at `bpm` in a bar of `beats`. False only when the thread
// cannot be made; a PCM that refuses to open is reported later through
// metronomeclick_failed(), because opening it means stopping the music first
// and that is a thing to do off the interface thread. True here means the
// thread is running, not that a sound has come out of it yet.
bool metronomeclick_start(int bpm, int beats);

// A new tempo, or a new bar, while it is running. Goes into the next block.
void metronomeclick_set_tempo(int bpm, int beats);

// Stops the thread, closes the PCM and hands the output back. Safe to call
// when it was never started.
void metronomeclick_stop(void);

// True when the PCM could not be opened; the thread has ended.
bool metronomeclick_failed(void);

// Clicks written since the start, and which beat of the bar the last one was
// (0 is the downbeat).
unsigned metronomeclick_beats(void);
int metronomeclick_beat_index(void);

#endif /* GUI_METRONOME_METRONOMECLICK_H */
