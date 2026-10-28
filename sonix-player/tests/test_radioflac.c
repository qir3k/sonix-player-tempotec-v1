// Tests for src/system/streaming/radioflac.c and dr_flac's relaxed stream open:
//   1. Native 16/44.1 FLAC broadcast behind an ID3v2 header, with Vorbis
//      comment ARTIST + TITLE and small network reads.
//   2. Mid-stream join on a native 24/96 FLAC broadcast (no "fLaC" magic or
//      STREAMINFO), including false 0xFF 0xF8 sync markers in preceding audio
//      payload bytes, verifying bit-exact 24-bit -> left-justified 32-bit PCM.
//   3. Chained Ogg FLAC broadcast across two logical bitstreams (16/48 ->
//      24/48), including multi-page Ogg packet assembly and per-song title
//      extraction via radioflac_take_title().
//   4. Non-FLAC / corrupt streams rejected cleanly at SEARCH_LIMIT_BYTES
//      without hanging or leaking memory.

#include "src/system/streaming/radioflac.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "FAIL (%s:%d): ", __FILE__, __LINE__);             \
			fprintf(stderr, __VA_ARGS__);                                      \
			fprintf(stderr, "\n");                                             \
			exit(1);                                                           \
		}                                                                      \
	} while (0)

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} buf_t;

static void buf_reserve(buf_t *b, size_t extra) {
	if (b->len + extra <= b->cap) {
		return;
	}
	size_t next = b->cap ? b->cap * 2 : 4096;
	while (next < b->len + extra) {
		next *= 2;
	}
	uint8_t *p = realloc(b->data, next);
	CHECK(p != NULL, "out of memory");
	b->data = p;
	b->cap = next;
}

static void buf_push(buf_t *b, const void *src, size_t n) {
	if (n == 0) {
		return;
	}
	buf_reserve(b, n);
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void buf_push_u8(buf_t *b, uint8_t v) { buf_push(b, &v, 1); }

static void buf_push_le32(buf_t *b, uint32_t v) {
	uint8_t tmp[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	buf_push(b, tmp, 4);
}

static uint8_t flac_crc8(const uint8_t *data, size_t len) {
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static uint16_t flac_crc16(const uint8_t *data, size_t len) {
	uint16_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static uint32_t ogg_crc32(const uint8_t *data, size_t len) {
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
		}
	}
	return crc;
}

static void make_streaminfo(uint8_t out[34], int sample_rate, int channels, int bits) {
	memset(out, 0, 34);
	// Fixed 256-frame blocks, unknown min/max frame byte sizes, total_samples=0 (live).
	out[0] = 0x01;
	out[1] = 0x00;
	out[2] = 0x01;
	out[3] = 0x00;
	uint64_t packed = ((uint64_t)sample_rate << 44) | ((uint64_t)(channels - 1) << 41) | ((uint64_t)(bits - 1) << 36);
	for (int i = 0; i < 8; i++) {
		out[10 + i] = (uint8_t)(packed >> (56 - 8 * i));
	}
}

static void build_vorbis_comment_body(buf_t *vc, const char *artist, const char *title) {
	const char *vendor = "sonix-test";
	buf_push_le32(vc, (uint32_t)strlen(vendor));
	buf_push(vc, vendor, strlen(vendor));

	uint32_t count = (artist ? 1u : 0u) + (title ? 1u : 0u);
	buf_push_le32(vc, count);

	char line[256];
	if (artist) {
		int n = snprintf(line, sizeof(line), "ARTIST=%s", artist);
		buf_push_le32(vc, (uint32_t)n);
		buf_push(vc, line, (size_t)n);
	}
	if (title) {
		int n = snprintf(line, sizeof(line), "TITLE=%s", title);
		buf_push_le32(vc, (uint32_t)n);
		buf_push(vc, line, (size_t)n);
	}
}

// Deterministic test waveform in native sample width (16 or 24-bit signed).
static int32_t test_sample_raw(int bits, int frame_idx, int sample_idx, int ch) {
	int32_t base = (frame_idx * 256 + sample_idx) * (ch == 0 ? 97 : -131) + (ch == 0 ? 1234 : -4321);
	if (bits == 16) {
		return (int16_t)(base & 0xFFFF);
	}
	// Full 24-bit dynamic range with non-zero low 8 bits so 16-bit truncation
	// would noticeably alter the low byte.
	int32_t s24 = ((base * 257) ^ 0x5A) & 0xFFFFFF;
	if (s24 & 0x800000) {
		s24 -= 0x1000000;
	}
	return s24;
}

// Builds one byte-aligned stereo VERBATIM FLAC audio frame of 256 PCM frames.
static void build_flac_frame(buf_t *out, int sample_rate, int bits, uint8_t frame_num) {
	uint8_t sr_code = sample_rate == 44100 ? 9 : sample_rate == 48000 ? 10 : 11;
	uint8_t bps_code = bits == 16 ? 4 : 6;
	CHECK(frame_num < 128, "single-byte UTF-8 frame index expected");

	size_t start = out->len;
	uint8_t hdr[6];
	hdr[0] = 0xFF;
	hdr[1] = 0xF8; // fixed blocksize strategy
	hdr[2] = (uint8_t)((8u << 4) | sr_code); // blocksize code 8 => 256 frames
	hdr[3] = (uint8_t)((1u << 4) | (bps_code << 1)); // 2 independent channels
	hdr[4] = frame_num;
	hdr[5] = flac_crc8(hdr, 5);
	buf_push(out, hdr, sizeof(hdr));

	for (int ch = 0; ch < 2; ch++) {
		buf_push_u8(out, 0x02); // 0 pad | 000001 VERBATIM | 0 wasted bits
		for (int i = 0; i < 256; i++) {
			int32_t s = test_sample_raw(bits, frame_num, i, ch);
			if (bits == 16) {
				uint8_t b[2] = {(uint8_t)((uint32_t)s >> 8), (uint8_t)(s & 0xFF)};
				buf_push(out, b, 2);
			} else {
				uint8_t b[3] = {(uint8_t)((uint32_t)s >> 16), (uint8_t)((uint32_t)s >> 8), (uint8_t)(s & 0xFF)};
				buf_push(out, b, 3);
			}
		}
	}

	uint16_t crc16 = flac_crc16(out->data + start, out->len - start);
	uint8_t ftr[2] = {(uint8_t)(crc16 >> 8), (uint8_t)(crc16 & 0xFF)};
	buf_push(out, ftr, 2);
}

// Appends one Ogg page carrying `payload[0..payload_len)`. When `continued_end`
// is true, `payload_len` must be a multiple of 255 so every lacing value is 255
// and the packet continues onto the next page.
static void append_ogg_page(buf_t *out, uint8_t header_type, uint64_t granule, uint32_t serial, uint32_t seq,
							const uint8_t *payload, size_t payload_len, bool continued_end) {
	uint8_t segs[255];
	int nsegs = 0;
	size_t rem = payload_len;
	if (continued_end) {
		CHECK(payload_len > 0 && payload_len % 255 == 0, "continued slice must be a multiple of 255");
		while (rem > 0) {
			CHECK(nsegs < 255, "too many segments");
			segs[nsegs++] = 255;
			rem -= 255;
		}
	} else {
		while (rem >= 255) {
			CHECK(nsegs < 255, "too many segments");
			segs[nsegs++] = 255;
			rem -= 255;
		}
		CHECK(nsegs < 255, "too many segments");
		segs[nsegs++] = (uint8_t)rem;
	}

	size_t page_pos = out->len;
	uint8_t hdr[27];
	memcpy(hdr, "OggS", 4);
	hdr[4] = 0;
	hdr[5] = header_type;
	for (int i = 0; i < 8; i++) {
		hdr[6 + i] = (uint8_t)(granule >> (8 * i));
	}
	for (int i = 0; i < 4; i++) {
		hdr[14 + i] = (uint8_t)(serial >> (8 * i));
		hdr[18 + i] = (uint8_t)(seq >> (8 * i));
		hdr[22 + i] = 0;
	}
	hdr[26] = (uint8_t)nsegs;

	buf_push(out, hdr, 27);
	buf_push(out, segs, (size_t)nsegs);
	buf_push(out, payload, payload_len);

	uint32_t crc = ogg_crc32(out->data + page_pos, out->len - page_pos);
	for (int i = 0; i < 4; i++) {
		out->data[page_pos + 22 + i] = (uint8_t)(crc >> (8 * i));
	}
}

static void append_ogg_flac_link(buf_t *out, uint32_t serial, int sample_rate, int bits, const char *artist,
								 const char *title, int num_frames) {
	uint32_t seq = 0;

	// Packet 0 (BOS): 0x7F "FLAC" 1.0 + 1 header count + "fLaC" + STREAMINFO.
	uint8_t bos[51];
	bos[0] = 0x7F;
	memcpy(bos + 1, "FLAC", 4);
	bos[5] = 1;
	bos[6] = 0;
	bos[7] = 0;
	bos[8] = 1;
	memcpy(bos + 9, "fLaC", 4);
	bos[13] = 0x00;
	bos[14] = 0x00;
	bos[15] = 0x00;
	bos[16] = 34;
	make_streaminfo(bos + 17, sample_rate, 2, bits);
	append_ogg_page(out, 0x02, 0, serial, seq++, bos, sizeof(bos), false);

	// Packet 1: VORBIS_COMMENT metadata block.
	buf_t vc = {0};
	build_vorbis_comment_body(&vc, artist, title);
	buf_t vc_pkt = {0};
	uint8_t vc_hdr[4] = {0x84, (uint8_t)(vc.len >> 16), (uint8_t)(vc.len >> 8), (uint8_t)vc.len};
	buf_push(&vc_pkt, vc_hdr, 4);
	buf_push(&vc_pkt, vc.data, vc.len);
	append_ogg_page(out, 0x00, 0, serial, seq++, vc_pkt.data, vc_pkt.len, false);
	free(vc.data);
	free(vc_pkt.data);

	// Audio packets: split frame 0 across two Ogg pages (510 bytes = 2 * 255 +
	// continuation page) to exercise multi-page packet assembly in radioflac.c.
	for (int f = 0; f < num_frames; f++) {
		buf_t frame = {0};
		build_flac_frame(&frame, sample_rate, bits, (uint8_t)f);
		uint64_t gran = (uint64_t)(f + 1) * 256u;
		uint8_t eos = (f == num_frames - 1) ? 0x04 : 0x00;
		if (f == 0 && frame.len > 510) {
			append_ogg_page(out, 0x00, UINT64_MAX, serial, seq++, frame.data, 510, true);
			append_ogg_page(out, 0x01 | eos, gran, serial, seq++, frame.data + 510, frame.len - 510, false);
		} else {
			append_ogg_page(out, eos, gran, serial, seq++, frame.data, frame.len, false);
		}
		free(frame.data);
	}
}

typedef struct {
	const uint8_t *data;
	size_t len;
	size_t at;
	int max_chunk;
} mem_source_t;

static int mem_source_read(void *user, unsigned char *dst, int max) {
	mem_source_t *s = user;
	if (s->at >= s->len) {
		return 0;
	}
	size_t avail = s->len - s->at;
	int cap = max;
	if (s->max_chunk > 0 && cap > s->max_chunk) {
		cap = s->max_chunk;
	}
	int n = avail < (size_t)cap ? (int)avail : cap;
	memcpy(dst, s->data + s->at, (size_t)n);
	s->at += (size_t)n;
	return n;
}

static void test_native_16bit_with_id3_and_comments(void) {
	buf_t stream = {0};

	// Prepend a 20-byte ID3v2.3 header (10-byte header + 10-byte padding) to
	// verify native FLAC streams with leading ID3 tags still open cleanly.
	uint8_t id3[20] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10};
	buf_push(&stream, id3, sizeof(id3));

	buf_push(&stream, "fLaC", 4);
	uint8_t si_hdr[4] = {0x00, 0x00, 0x00, 34};
	uint8_t si[34];
	make_streaminfo(si, 44100, 2, 16);
	buf_push(&stream, si_hdr, 4);
	buf_push(&stream, si, 34);

	buf_t vc = {0};
	build_vorbis_comment_body(&vc, "Radio Paradise", "Solitude Standing");
	uint8_t vc_hdr[4] = {0x84, (uint8_t)(vc.len >> 16), (uint8_t)(vc.len >> 8), (uint8_t)vc.len};
	buf_push(&stream, vc_hdr, 4);
	buf_push(&stream, vc.data, vc.len);
	free(vc.data);

	for (int f = 0; f < 3; f++) {
		build_flac_frame(&stream, 44100, 16, (uint8_t)f);
	}

	// Deliver 37 bytes at a time to stress pull-callback reassembly.
	mem_source_t src = {stream.data, stream.len, 0, 37};
	radioflac_t *rf = radioflac_open(false, mem_source_read, &src);
	CHECK(rf != NULL, "radioflac_open failed");

	int32_t pcm[256 * 2 * 4];
	int total_frames = 0;
	for (;;) {
		int got = radioflac_read(rf, pcm + total_frames * 2, 200);
		CHECK(got >= 0, "unexpected decode error on native 16-bit FLAC");
		if (got == 0) {
			break;
		}
		total_frames += got;
	}
	CHECK(total_frames == 3 * 256, "expected 768 frames, got %d", total_frames);

	int rate = 0, channels = 0, bits = 0;
	radioflac_format(rf, &rate, &channels, &bits);
	CHECK(rate == 44100 && channels == 2 && bits == 16, "bad format %d/%d/%d", rate, channels, bits);

	char title[256] = "";
	CHECK(radioflac_take_title(rf, title, sizeof(title)), "expected Vorbis comment title");
	CHECK(strcmp(title, "Radio Paradise - Solitude Standing") == 0, "unexpected title '%s'", title);
	CHECK(!radioflac_take_title(rf, title, sizeof(title)), "title should be consumed once");

	for (int f = 0; f < 3; f++) {
		for (int i = 0; i < 256; i++) {
			int idx = (f * 256 + i) * 2;
			int32_t exp_l = (int32_t)((uint32_t)test_sample_raw(16, f, i, 0) << 16);
			int32_t exp_r = (int32_t)((uint32_t)test_sample_raw(16, f, i, 1) << 16);
			CHECK(pcm[idx] == exp_l && pcm[idx + 1] == exp_r,
				  "16-bit PCM mismatch at frame %d sample %d: got (%d,%d), expected (%d,%d)",
				  f, i, pcm[idx], pcm[idx + 1], exp_l, exp_r);
		}
	}

	radioflac_close(rf);
	free(stream.data);
}

static void test_midstream_24bit_relaxed_join(void) {
	buf_t stream = {0};

	// Simulate joining mid-frame: 512 bytes of arbitrary compressed payload
	// containing false 0xFF 0xF8 sync markers:
	//   - one claiming 16-bit blockSize == 0xFFFF (must continue scanning, not abort)
	//   - one claiming wrong bitsPerSample with bad CRC8
	uint8_t midframe[512];
	for (size_t i = 0; i < sizeof(midframe); i++) {
		midframe[i] = (uint8_t)((i * 73 + 19) & 0x7F);
	}
	// False sync #1: 0xFF 0xF8, blockSize code 7 (16-bit literal = 0xFFFF), 48 kHz, stereo, 24-bit.
	midframe[64] = 0xFF;
	midframe[65] = 0xF8;
	midframe[66] = (7u << 4) | 10u;
	midframe[67] = (1u << 4) | (6u << 1);
	midframe[68] = 0x05;
	midframe[69] = 0xFF;
	midframe[70] = 0xFF;
	// False sync #2: 0xFF 0xF8 with valid header shape but wrong CRC8.
	midframe[220] = 0xFF;
	midframe[221] = 0xF8;
	midframe[222] = (8u << 4) | 11u;
	midframe[223] = (1u << 4) | (4u << 1);
	midframe[224] = 0x01;
	midframe[225] = 0x00;

	buf_push(&stream, midframe, sizeof(midframe));

	// Followed by 3 real 24-bit / 96 kHz stereo frames (frames #10, #11, #12).
	for (int f = 10; f < 13; f++) {
		build_flac_frame(&stream, 96000, 24, (uint8_t)f);
	}

	mem_source_t src = {stream.data, stream.len, 0, 113};
	radioflac_t *rf = radioflac_open(false, mem_source_read, &src);
	CHECK(rf != NULL, "radioflac_open failed");

	int32_t pcm[256 * 2 * 4];
	int total_frames = 0;
	for (;;) {
		int got = radioflac_read(rf, pcm + total_frames * 2, 256);
		CHECK(got >= 0, "mid-stream 24-bit relaxed open failed (%d)", got);
		if (got == 0) {
			break;
		}
		total_frames += got;
	}
	CHECK(total_frames == 3 * 256, "expected 768 mid-stream frames, got %d", total_frames);

	int rate = 0, channels = 0, bits = 0;
	radioflac_format(rf, &rate, &channels, &bits);
	CHECK(rate == 96000 && channels == 2 && bits == 24, "bad mid-stream format %d/%d/%d", rate, channels, bits);

	for (int f = 0; f < 3; f++) {
		for (int i = 0; i < 256; i++) {
			int idx = (f * 256 + i) * 2;
			int32_t exp_l = (int32_t)((uint32_t)test_sample_raw(24, 10 + f, i, 0) << 8);
			int32_t exp_r = (int32_t)((uint32_t)test_sample_raw(24, 10 + f, i, 1) << 8);
			CHECK(pcm[idx] == exp_l && pcm[idx + 1] == exp_r,
				  "24-bit PCM mismatch at frame %d sample %d: got (%d,%d), expected (%d,%d)",
				  f, i, pcm[idx], pcm[idx + 1], exp_l, exp_r);
		}
	}

	radioflac_close(rf);
	free(stream.data);
}

static void test_chained_ogg_flac_and_titles(void) {
	buf_t stream = {0};
	append_ogg_flac_link(&stream, 0x11112222u, 48000, 16, "Massive Attack", "Teardrop", 2);
	append_ogg_flac_link(&stream, 0x33334444u, 48000, 24, "Portishead", "Roads", 2);

	mem_source_t src = {stream.data, stream.len, 0, 97};
	radioflac_t *rf = radioflac_open(true, mem_source_read, &src);
	CHECK(rf != NULL, "radioflac_open(ogg) failed");

	int32_t pcm[256 * 2 * 5];
	int total_frames = 0;
	char seen_titles[4][256];
	int title_count = 0;
	bool saw_16 = false;
	bool saw_24 = false;

	for (;;) {
		int got = radioflac_read(rf, pcm + total_frames * 2, 128);
		CHECK(got >= 0, "chained Ogg FLAC decode failed (%d)", got);
		if (got == 0) {
			break;
		}
		total_frames += got;

		int rate = 0, channels = 0, bits = 0;
		radioflac_format(rf, &rate, &channels, &bits);
		CHECK(rate == 48000 && channels == 2, "unexpected Ogg rate/ch %d/%d", rate, channels);
		if (bits == 16) {
			saw_16 = true;
		} else if (bits == 24) {
			saw_24 = true;
		}

		char announced[256];
		while (radioflac_take_title(rf, announced, sizeof(announced))) {
			CHECK(title_count < 4, "too many titles");
			snprintf(seen_titles[title_count++], sizeof(seen_titles[0]), "%s", announced);
		}
	}

	CHECK(total_frames == 4 * 256, "expected 1024 frames across 2 Ogg links, got %d", total_frames);
	CHECK(saw_16 && saw_24, "expected both 16-bit and 24-bit links (saw_16=%d saw_24=%d)", saw_16, saw_24);
	CHECK(title_count == 2, "expected 2 link titles, got %d", title_count);
	CHECK(strcmp(seen_titles[0], "Massive Attack - Teardrop") == 0, "bad link 1 title '%s'", seen_titles[0]);
	CHECK(strcmp(seen_titles[1], "Portishead - Roads") == 0, "bad link 2 title '%s'", seen_titles[1]);

	// Verify PCM integrity on both Link 1 (16-bit) and Link 2 (24-bit).
	for (int f = 0; f < 2; f++) {
		for (int i = 0; i < 256; i++) {
			int idx1 = (f * 256 + i) * 2;
			int32_t e1_l = (int32_t)((uint32_t)test_sample_raw(16, f, i, 0) << 16);
			int32_t e1_r = (int32_t)((uint32_t)test_sample_raw(16, f, i, 1) << 16);
			CHECK(pcm[idx1] == e1_l && pcm[idx1 + 1] == e1_r, "Ogg link 1 PCM mismatch");

			int idx2 = ((2 + f) * 256 + i) * 2;
			int32_t e2_l = (int32_t)((uint32_t)test_sample_raw(24, f, i, 0) << 8);
			int32_t e2_r = (int32_t)((uint32_t)test_sample_raw(24, f, i, 1) << 8);
			CHECK(pcm[idx2] == e2_l && pcm[idx2 + 1] == e2_r, "Ogg link 2 PCM mismatch");
		}
	}

	radioflac_close(rf);
	free(stream.data);
}

static void test_non_flac_rejected(void) {
	size_t len = 300 * 1024;
	uint8_t *junk = malloc(len);
	CHECK(junk != NULL, "out of memory");
	for (size_t i = 0; i < len; i++) {
		junk[i] = (uint8_t)((i * 31 + 7) & 0x7F);
	}

	int32_t pcm[256];
	for (int ogg = 0; ogg <= 1; ogg++) {
		mem_source_t src = {junk, len, 0, 4096};
		radioflac_t *rf = radioflac_open(ogg != 0, mem_source_read, &src);
		CHECK(rf != NULL, "radioflac_open failed");
		int rc = radioflac_read(rf, pcm, 32);
		CHECK(rc == -1, "expected -1 for non-FLAC input (ogg=%d), got %d", ogg, rc);
		radioflac_close(rf);
	}

	free(junk);
}

int main(void) {
	test_native_16bit_with_id3_and_comments();
	test_midstream_24bit_relaxed_join();
	test_chained_ogg_flac_and_titles();
	test_non_flac_rejected();
	printf("radioflac: native 16-bit, mid-stream 24-bit relaxed, chained Ogg FLAC + titles, and rejection ok\n");
	return 0;
}
