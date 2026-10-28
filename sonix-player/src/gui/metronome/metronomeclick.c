#include "metronomeclick.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/audio.h"
#include "src/system/streaming/radio.h"

// The stream: 16-bit stereo at 44.1 kHz, in ten-millisecond blocks.
//
// The block is the latency of a tempo change and nothing else -- a click is
// placed on its own sample inside the block -- so it is kept small; the buffer
// behind it is the usual short one the emulator uses, which is what keeps the
// click from being heard a fifth of a second after the finger.
#define RATE 44100
#define BLOCK_FRAMES 441 // 10 ms
#define BUFFER_MS 50

// The click itself: a sine burst with a fast decay, 32 ms long. Longer than a
// real metronome's wood block, because a burst that stops sooner than this
// reads as a tick from the little speaker rather than as a beat.
#define CLICK_MS 32
#define CLICK_FRAMES (RATE * CLICK_MS / 1000)
#define CLICK_ATTACK_MS 1.5f
#define CLICK_DECAY_S 0.0085f
// The downbeat is the higher of the two, and the louder: bar 1 has to be
// audible in the pattern without counting.
#define CLICK_HZ_BEAT 1046.0f  // C6
#define CLICK_HZ_FIRST 1568.0f // G6
#define CLICK_AMP_BEAT 8600.0f
#define CLICK_AMP_FIRST 11800.0f

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool thread_live;

// Read by the thread on every block and written by the page, so all of them
// are volatile: a tempo change has to reach the block after the one being
// written, without a lock in the audio path.
static volatile bool stop_flag;
static volatile bool running; // the PCM is open and clicks are being written
static volatile bool failed;  // the PCM would not open; the thread has ended
static volatile int bpm = 120;
static volatile int bar_beats = 4;
static volatile unsigned beats_written;
static volatile int last_beat = -1;

// Two bursts, built once. Not const: they are filled at the first start, so a
// page never opened costs nothing.
static int16_t clicks[2][CLICK_FRAMES];

static void build_clicks(void) {
	static bool built;
	if (built) {
		return;
	}
	built = true;
	for (int kind = 0; kind < 2; kind++) {
		float hz = kind == 0 ? CLICK_HZ_FIRST : CLICK_HZ_BEAT;
		float amp = kind == 0 ? CLICK_AMP_FIRST : CLICK_AMP_BEAT;
		for (int i = 0; i < CLICK_FRAMES; i++) {
			float t = (float)i / (float)RATE;
			// A short attack, so the burst does not begin on a step (which is
			// itself a click, of the kind that sounds like a fault), then an
			// exponential decay.
			float attack = t < CLICK_ATTACK_MS / 1000.0f ? t / (CLICK_ATTACK_MS / 1000.0f) : 1.0f;
			float env = expf(-t / CLICK_DECAY_S);
			float v = sinf(2.0f * 3.14159265f * hz * t) * env * attack * amp;
			clicks[kind][i] = (int16_t)v;
		}
	}
}

// The loop. Frames, not sleeps: every pass writes one block, and the write
// blocks until there is room, so the stream itself is the clock.
static void *click_main(void *unused) {
	(void)unused;

	// One PCM takes one writer. Stopping local playback is what the game does
	// for the same reason, and it happens here rather than on the caller's
	// thread because it takes long enough to be felt as a freeze.
	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}
	if (stop_flag) {
		return NULL;
	}

	if (!audio_external_begin_latency(RATE, 2, 16, BUFFER_MS)) {
		fprintf(stderr, "metronome: no PCM; the click cannot be heard\n");
		failed = true;
		return NULL;
	}
	running = true;

	int16_t out[BLOCK_FRAMES * 2];
	// Frames until the next click, kept as a fraction: truncating it every
	// block is what would make a metronome drift.
	double to_beat = 0.0;
	// Where the click being written has got to, and where in this block it
	// starts. -1 means nothing to write (or not yet, for the start).
	int voice_pos = CLICK_FRAMES;
	int voice_start = -1;
	const int16_t *voice = clicks[0];

	while (!stop_flag) {
		int tempo = bpm;
		if (tempo < 20) {
			tempo = 20;
		} else if (tempo > 400) {
			tempo = 400;
		}
		double per_beat = (double)RATE * 60.0 / (double)tempo;
		int beats = bar_beats < 1 ? 1 : bar_beats;

		// Is a beat due inside this block?
		int next = -1;
		voice_start = -1;
		if (to_beat < BLOCK_FRAMES) {
			voice_start = to_beat < 0.0 ? 0 : (int)to_beat;
			next = (last_beat + 1) % beats;
			voice = next == 0 ? clicks[0] : clicks[1];
			voice_pos = -1; // armed: starts at voice_start
		}

		memset(out, 0, sizeof(out));
		for (int f = 0; f < BLOCK_FRAMES; f++) {
			if (f == voice_start) {
				voice_pos = 0;
			}
			if (voice_pos >= 0 && voice_pos < CLICK_FRAMES) {
				int16_t v = voice[voice_pos++];
				out[f * 2] = v;
				out[f * 2 + 1] = v;
			}
		}

		to_beat -= (double)BLOCK_FRAMES;
		if (next >= 0) {
			to_beat += per_beat;
			last_beat = next;
			beats_written++;
		}
		if (to_beat < 0.0) {
			to_beat = 0.0; // only reachable if the tempo changed under a beat
		}

		audio_external_write(out, BLOCK_FRAMES);
	}

	audio_external_end();
	running = false;
	return NULL;
}

bool metronomeclick_start(int tempo, int beats) {
	metronomeclick_stop();
	build_clicks();

	pthread_mutex_lock(&lock);
	bpm = tempo;
	bar_beats = beats;
	beats_written = 0;
	last_beat = beats - 1; // so the first click written is the downbeat
	failed = false;
	running = false;
	stop_flag = false;
	int err = pthread_create(&thread, NULL, click_main, NULL);
	if (err == 0) {
		thread_live = true;
	}
	pthread_mutex_unlock(&lock);

	return err == 0;
}

void metronomeclick_set_tempo(int tempo, int beats) {
	bpm = tempo;
	bar_beats = beats;
}

void metronomeclick_stop(void) {
	pthread_mutex_lock(&lock);
	if (thread_live) {
		stop_flag = true;
		pthread_join(thread, NULL);
		thread_live = false;
	}
	running = false;
	failed = false;
	pthread_mutex_unlock(&lock);
}

bool metronomeclick_failed(void) { return failed; }

unsigned metronomeclick_beats(void) { return beats_written; }

int metronomeclick_beat_index(void) { return last_beat; }
