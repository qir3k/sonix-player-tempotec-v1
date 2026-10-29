#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "src/system/library/tagwrite.h"

static int failures;

static void check(bool cond, const char *what) {
	printf("%s: %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) failures++;
}

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_ss(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)((v >> 21) & 0x7f); p[1] = (uint8_t)((v >> 14) & 0x7f);
	p[2] = (uint8_t)((v >> 7) & 0x7f); p[3] = (uint8_t)(v & 0x7f);
}
static uint32_t get_ss(const uint8_t *p) {
	return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
		   ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static const uint8_t OLD_JPEG[] = {0xFF, 0xD8, 0xFF, 0xE0, 'O', 'L', 'D', 0x00};
static const uint8_t NEW_JPEG[] = {0xFF, 0xD8, 0xFF, 0xE0, 'N', 'E', 'W', '!', '!', 0x00};

// A tiny but structurally valid JPEG: SOI, SOF0 with 3 components, EOI.
static size_t make_jpeg(uint8_t *out, int w, int h) {
	size_t n = 0;
	out[n++] = 0xFF; out[n++] = 0xD8;
	out[n++] = 0xFF; out[n++] = 0xC0; out[n++] = 0x00; out[n++] = 0x11; out[n++] = 0x08;
	out[n++] = (uint8_t)(h >> 8); out[n++] = (uint8_t)h;
	out[n++] = (uint8_t)(w >> 8); out[n++] = (uint8_t)w;
	out[n++] = 0x03;
	for (int i = 0; i < 9; i++) out[n++] = 0x01;
	out[n++] = 0xFF; out[n++] = 0xD9;
	return n;
}

static uint8_t *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)n + 1);
	if (!buf) { fclose(f); return NULL; }
	*size = fread(buf, 1, (size_t)n, f);
	fclose(f);
	return buf;
}

// ---------------------------------------------------------------- MP3

static const uint8_t AUDIO[] = {0xFF, 0xFB, 0x90, 0x00, 'a', 'u', 'd', 'i', 'o'};

static void make_mp3(const char *path, bool with_tag, bool with_apic) {
	uint8_t *tag = NULL;
	size_t tag_len = 0;
	uint8_t *t = malloc(4096);
	size_t at = 10; // body starts after the header
	// TIT2: a frame worth keeping.
	const char *title = "Original Title";
	size_t title_len = 1 + strlen(title);
	memcpy(t + at, "TIT2", 4); put_be32(t + at + 4, (uint32_t)title_len);
	t[at + 8] = 0; t[at + 9] = 0;
	t[at + 10] = 0; memcpy(t + at + 11, title, strlen(title));
	at += 10 + title_len;
	if (with_apic) {
		size_t body = 1 + strlen("image/jpeg") + 1 + 1 + 1 + sizeof(OLD_JPEG);
		memcpy(t + at, "APIC", 4); put_be32(t + at + 4, (uint32_t)body);
		t[at + 8] = 0; t[at + 9] = 0;
		size_t p = at + 10;
		t[p++] = 0; memcpy(t + p, "image/jpeg", strlen("image/jpeg")); p += strlen("image/jpeg");
		t[p++] = 0; t[p++] = 3; t[p++] = 0; memcpy(t + p, OLD_JPEG, sizeof(OLD_JPEG));
		at += 10 + body;
	}
	if (with_tag) {
		memcpy(t, "ID3", 3); t[3] = 3; t[4] = 0; t[5] = 0; put_ss(t + 6, (uint32_t)(at - 10));
		tag = t; tag_len = at;
	} else {
		free(t);
	}
	FILE *f = fopen(path, "wb");
	if (tag) fwrite(tag, 1, tag_len, f);
	fwrite(AUDIO, 1, sizeof(AUDIO), f);
	fclose(f);
	free(tag);
}

static void read_mp3(const char *path, uint8_t **tag_out, size_t *tag_size, uint8_t **audio, size_t *audio_len) {
	size_t size = 0;
	uint8_t *buf = read_file(path, &size);
	*tag_out = buf; *tag_size = 0; *audio = NULL; *audio_len = 0;
	if (!buf || size < 10) return;
	*tag_size = 10 + get_ss(buf + 6);
	*audio = buf + *tag_size;
	*audio_len = size - *tag_size;
}

static void test_mp3(const char *path, bool with_tag, bool with_apic, const char *label) {
	make_mp3(path, with_tag, with_apic);
	uint8_t jpeg[64];
	size_t jpeg_len = make_jpeg(jpeg, 600, 600);
	char err[96];
	bool ok = tagwrite_set_cover(path, jpeg, jpeg_len, err, sizeof(err));
	check(ok, label);
	if (!ok) { printf("   error: %s\n", err); return; }

	uint8_t *tag = NULL, *audio = NULL;
	size_t tag_size = 0, audio_len = 0;
	read_mp3(path, &tag, &tag_size, &audio, &audio_len);
	check(tag && memcmp(tag, "ID3", 3) == 0 && tag_size > 10, "mp3: has an ID3v2 tag");
	check(audio_len == sizeof(AUDIO) && memcmp(audio, AUDIO, sizeof(AUDIO)) == 0,
		  "mp3: audio bytes untouched");
	int major = tag ? tag[3] : 0;
	check(major == 3 || major == 4, "mp3: version 3 or 4");

	bool saw_title = false, saw_apic = false, saw_old = false;
	uint32_t apic_size = 0;
	size_t at = 10;
	while (tag && at + 10 <= tag_size) {
		if (tag[at] == 0) break;
		char id[5] = {0};
		memcpy(id, tag + at, 4);
		uint32_t fsz = major >= 4 ? get_ss(tag + at + 4) : get_be32(tag + at + 4);
		if (fsz > tag_size - at - 10) break;
		if (strcmp(id, "TIT2") == 0) saw_title = true;
		if (strcmp(id, "APIC") == 0) {
			saw_apic = true;
			apic_size = fsz;
			const uint8_t *body = tag + at + 10;
			if (body + 11 < tag + tag_size &&
				memcmp(body + 11, OLD_JPEG, sizeof(OLD_JPEG) < (size_t)fsz ? sizeof(OLD_JPEG) : fsz) == 0)
				saw_old = true;
			// The picture starts after enc, mime, NUL, type, desc NUL.
			const uint8_t *data = memchr(body + 1, 0, fsz);
			if (data) {
				data += 2; // NUL, picture type
				data += 1; // empty description
				check((size_t)(tag + at + 10 + fsz - data) == jpeg_len &&
					  memcmp(data, jpeg, jpeg_len) == 0, "mp3: the new picture is the one written");
			}
		}
		at += 10 + fsz;
	}
	// A file that came with no tag has no title to keep; only the two that
	// did must come out with it intact.
	if (with_tag) {
		check(saw_title, "mp3: other frames kept");
	}
	check(saw_apic && !saw_old, "mp3: old picture replaced");
	check(apic_size > jpeg_len, "mp3: APIC frame looks right");
	free(tag);
}

// ---------------------------------------------------------------- FLAC

static size_t flac_block(uint8_t *out, bool last, uint8_t type, const uint8_t *data, uint32_t len) {
	out[0] = (uint8_t)(type | (last ? 0x80 : 0));
	out[1] = (uint8_t)(len >> 16); out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len;
	memcpy(out + 4, data, len);
	return 4 + len;
}

static void make_flac(const char *path, bool with_picture) {
	uint8_t buf[4096];
	size_t at = 0;
	memcpy(buf, "fLaC", 4); at = 4;
	uint8_t streaminfo[34];
	memset(streaminfo, 0x11, sizeof(streaminfo));
	bool next_last = !with_picture;
	at += flac_block(buf + at, next_last, 0, streaminfo, sizeof(streaminfo));
	if (with_picture) {
		uint8_t pic[128];
		size_t p = 0;
		put_be32(pic + p, 3); p += 4;
		put_be32(pic + p, 10); p += 4; memcpy(pic + p, "image/jpeg", 10); p += 10;
		put_be32(pic + p, 0); p += 4;
		put_be32(pic + p, 0); p += 4; put_be32(pic + p, 0); p += 4;
		put_be32(pic + p, 0); p += 4; put_be32(pic + p, 0); p += 4;
		put_be32(pic + p, sizeof(OLD_JPEG)); p += 4;
		memcpy(pic + p, OLD_JPEG, sizeof(OLD_JPEG)); p += sizeof(OLD_JPEG);
		// A VORBIS_COMMENT block after it, to prove the rest survives.
		at += flac_block(buf + at, false, 6, pic, (uint32_t)p);
		uint8_t vc[8] = {4, 0, 0, 0, 0, 0, 0, 0};
		at += flac_block(buf + at, true, 4, vc, 8);
	}
	FILE *f = fopen(path, "wb");
	fwrite(buf, 1, at, f);
	fwrite(AUDIO, 1, sizeof(AUDIO), f);
	fclose(f);
}

static void test_flac(const char *path) {
	make_flac(path, true);
	uint8_t jpeg[64];
	size_t jpeg_len = make_jpeg(jpeg, 1200, 1200);
	char err[96];
	bool ok = tagwrite_set_cover(path, jpeg, jpeg_len, err, sizeof(err));
	check(ok, "flac: rewrite succeeded");
	if (!ok) { printf("   error: %s\n", err); return; }

	size_t size = 0;
	uint8_t *buf = read_file(path, &size);
	check(buf && memcmp(buf, "fLaC", 4) == 0, "flac: magic kept");
	int pictures = 0, comments = 0, streaminfos = 0;
	bool found_picture = false;
	uint32_t dims_w = 0;
	size_t at = 4;
	while (buf && at + 4 <= size) {
		bool last = (buf[at] & 0x80) != 0;
		uint8_t type = buf[at] & 0x7f;
		uint32_t len = ((uint32_t)buf[at + 1] << 16) | ((uint32_t)buf[at + 2] << 8) | buf[at + 3];
		if (type == 0) { streaminfos++; check(len == 34, "flac: STREAMINFO kept whole"); }
		if (type == 4) comments++;
		if (type == 6) {
			pictures++;
			const uint8_t *p = buf + at + 4;
			uint32_t mime_len = get_be32(p + 4);
			const uint8_t *desc = p + 8 + mime_len;
			uint32_t desc_len = get_be32(desc);
			const uint8_t *dims = desc + 4 + desc_len;
			dims_w = get_be32(dims);
			uint32_t data_len = get_be32(dims + 16);
			const uint8_t *data = dims + 20;
			check(data_len == jpeg_len && memcmp(data, jpeg, jpeg_len) == 0,
				  "flac: the new picture is the one written");
			found_picture = true;
		}
		at += 4 + len;
		if (last) break;
	}
	check(streaminfos == 1, "flac: one STREAMINFO");
	check(comments == 1, "flac: VORBIS_COMMENT kept");
	check(pictures == 1 && found_picture, "flac: exactly one PICTURE");
	check(dims_w == 1200, "flac: picture dimensions read from the image");
	check(size - at == sizeof(AUDIO) && memcmp(buf + at, AUDIO, sizeof(AUDIO)) == 0,
		  "flac: audio bytes untouched");
	free(buf);
}

int main(void) {
	test_mp3("t1.mp3", true, true, "mp3: tag with a picture rewritten");
	test_mp3("t2.mp3", true, false, "mp3: tag without a picture gains one");
	test_mp3("t3.mp3", false, false, "mp3: file without a tag gains a whole one");
	test_flac("t4.flac");

	char err[96];
	check(!tagwrite_set_cover("t1.ogg", NEW_JPEG, sizeof(NEW_JPEG), err, sizeof(err)),
		  "ogg: refused");
	check(strcmp(err, "artwork_tag_unsupported") == 0, "ogg: says why");
	check(!tagwrite_set_cover("t1.mp3", "not an image", 12, err, sizeof(err)), "garbage refused");

	remove("t1.mp3"); remove("t2.mp3"); remove("t3.mp3"); remove("t4.flac");
	printf("%s\n", failures ? "FAILURES" : "all tag writer checks passed");
	return failures ? 1 : 0;
}
