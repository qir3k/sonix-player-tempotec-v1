// ALAC, end to end: an MP4 whose audio track is Apple Lossless is opened the
// way the player opens it, the codec is read back as ALAC rather than AAC, and
// the PCM it decodes is compared with the PCM it was encoded from.
//
// The fixtures are not checked in. `make alac` expects a folder holding
// `alac.m4a` (ffmpeg -c:a alac) and `alac.s16` (the same tone as raw
// little-endian 16-bit stereo), which the tests workflow generates; with no
// folder the check is skipped so a host without ffmpeg still builds.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/decode/decode.h"
#include "src/system/library/cue.h"
#include "src/system/streaming/subsonicstream.h"

// Stubs for the network externals decode.c knows about; nothing here streams.
bool subsonicstream_owns(const char *path) {
	(void)path;
	return false;
}

bool subsonicstream_metadata(const char *path, song_metadata_t *out) {
	(void)path;
	(void)out;
	return false;
}

streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate) {
	(void)path;
	(void)duration;
	(void)bitrate;
	return NULL;
}

void subsonicstream_release(streambridge_t *sb) { (void)sb; }

// The two decoders built on a host library. This suite does not open either;
// their symbols only have to exist for decode.c to link.
typedef struct opusdec opusdec_t;
typedef struct wavpackdec wavpackdec_t;

opusdec_t *opusdec_open(const char *p) {
	(void)p;
	return NULL;
}
opusdec_t *opusdec_open_stream(void *s, int (*r)(void *, unsigned char *, int)) {
	(void)s;
	(void)r;
	return NULL;
}
void opusdec_close(opusdec_t *o) { (void)o; }
int opusdec_channels(const opusdec_t *o) {
	(void)o;
	return 0;
}
int opusdec_sample_rate(const opusdec_t *o) {
	(void)o;
	return 0;
}
uint64_t opusdec_total_frames(const opusdec_t *o) {
	(void)o;
	return 0;
}
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t f, short *out) {
	(void)o;
	(void)f;
	(void)out;
	return 0;
}
bool opusdec_seek(opusdec_t *o, uint64_t f) {
	(void)o;
	(void)f;
	return false;
}

wavpackdec_t *wavpackdec_open(const char *p) {
	(void)p;
	return NULL;
}
void wavpackdec_close(wavpackdec_t *w) { (void)w; }
int wavpackdec_channels(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
int wavpackdec_sample_rate(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
int wavpackdec_bits(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_total_frames(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t f, short *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t f, int32_t *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
bool wavpackdec_seek(wavpackdec_t *w, uint64_t f) {
	(void)w;
	(void)f;
	return false;
}

// ---------------------------------------------------------------------------

static int16_t *read_reference(const char *path, size_t *frames) {
	FILE *f = fopen(path, "rb");
	assert(f != NULL);
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && size % 4 == 0); // stereo, 16-bit
	assert(fseek(f, 0, SEEK_SET) == 0);
	int16_t *pcm = malloc((size_t)size);
	assert(pcm != NULL);
	assert(fread(pcm, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	*frames = (size_t)size / 4;
	return pcm;
}

static int16_t *decode_all(decoder_t *dec, size_t capacity_frames, size_t *frames) {
	int16_t *pcm = malloc(capacity_frames * 2 * sizeof(int16_t));
	assert(pcm != NULL);
	size_t got = 0;
	for (;;) {
		uint64_t n = decoder_read_pcm_frames_s16(dec, capacity_frames - got, pcm + got * 2);
		if (n == 0) break;
		got += (size_t)n;
		if (got >= capacity_frames) break; // the fixtures are a couple of seconds
	}
	*frames = got;
	return pcm;
}

// An encoder is free to keep its priming samples, and ffmpeg's MP4 carries an
// edit list that trims them on playback. Rather than depend on which one this
// container got, find where the two agree and then require that they agree
// everywhere: one sample of slack covers the rounding a 16-bit round trip can
// pick up, and nothing else is tolerated.
static void require_matches(const int16_t *decoded, size_t decoded_frames, const int16_t *reference,
							size_t reference_frames) {
	size_t window = 2000;
	if (window > decoded_frames) window = decoded_frames;
	if (window > reference_frames) window = reference_frames;
	assert(window > 100);

	size_t best = 0;
	long best_cost = -1;
	for (size_t off = 0; off + window <= decoded_frames && off < 8192; off++) {
		long cost = 0;
		for (size_t i = 0; i < window * 2; i++) {
			long d = (long)decoded[off * 2 + i] - (long)reference[i];
			cost += d < 0 ? -d : d;
			if (best_cost >= 0 && cost >= best_cost) break; // not the winner
		}
		if (best_cost < 0 || cost < best_cost) {
			best_cost = cost;
			best = off;
		}
	}
	assert(best_cost >= 0);

	size_t compared = decoded_frames - best;
	if (compared > reference_frames) compared = reference_frames;
	if (compared < 22050) {
		fprintf(stderr, "alac: only %zu frames line up with the reference\n", compared);
	}
	assert(compared >= 22050); // half a second at 44.1 kHz

	for (size_t i = 0; i < compared * 2; i++) {
		long d = (long)decoded[best * 2 + i] - (long)reference[i];
		if (d < -1 || d > 1) {
			fprintf(stderr,
					"alac: frame %zu channel %zu is %d, not %d (aligned at %zu frames)\n", i / 2, i % 2,
					decoded[best * 2 + i], reference[i], best);
			assert(0);
		}
	}
	printf("alac: %zu frames match the PCM it was encoded from (aligned at %zu, %zu compared)\n",
		   decoded_frames, best, compared);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char m4a[512], raw[512];
	snprintf(m4a, sizeof(m4a), "%s/alac.m4a", argv[1]);
	snprintf(raw, sizeof(raw), "%s/alac.s16", argv[1]);

	struct stat st;
	if (stat(m4a, &st) != 0 || stat(raw, &st) != 0) {
		printf("alac: skipped (no fixtures in %s)\n", argv[1]);
		return 0;
	}

	// An ALAC file is an ordinary MP4 to the format sniffer: the codec is
	// inside the container, not in the name.
	decode_format_t format = decode_detect_format(m4a);
	assert(format == DECODE_FORMAT_AAC_MP4);

	decoder_t *dec = decoder_open(m4a, format);
	assert(dec != NULL);
	assert(strcmp(decoder_codec_name(dec), "ALAC") == 0);
	assert(decoder_sample_rate(dec) == 44100);
	assert(decoder_channels(dec) == 2);
	assert(decoder_source_bits(dec) == 16);

	size_t reference_frames = 0;
	int16_t *reference = read_reference(raw, &reference_frames);

	size_t decoded_frames = 0;
	int16_t *decoded = decode_all(dec, reference_frames + 8192 + 4096, &decoded_frames);
	assert(decoded_frames > 0);

	require_matches(decoded, decoded_frames, reference, reference_frames);

	decoder_close(dec);
	free(decoded);
	free(reference);
	puts("alac: ok");
	return 0;
}
