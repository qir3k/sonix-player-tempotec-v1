#include "tagwrite.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// The two formats a card full of music actually arrives in, and the only two
// this writes: ID3v2 on an MP3, and the PICTURE block in a FLAC's metadata.
// Everything else is refused by name rather than half-rewritten -- see the
// header for why.
//
// Both rewrites follow the same shape: the new picture and the parts of the old
// tag worth keeping are written to a temporary file in front of the old audio
// bytes, and the result is renamed over the original only once it is complete
// and closed. A card that goes away in the middle costs a temporary file, never
// a song.

#define COPY_CHUNK (64 * 1024)
#define MP3_TAG_MAX (24 * 1024 * 1024) // a tag is small; a damaged size is not followed

static pthread_mutex_t tag_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(char *out, size_t size, const char *reason) {
	if (out && size) {
		snprintf(out, size, "%s", reason ? reason : "artwork_tag_failed");
	}
}

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ID3v2's size fields are seven bits per byte: the top bit of each is zero so
// that a tag cannot look like an MPEG frame sync.
static uint32_t syncsafe32(const uint8_t *p) {
	return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
		   ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_syncsafe32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)((v >> 21) & 0x7f);
	p[1] = (uint8_t)((v >> 14) & 0x7f);
	p[2] = (uint8_t)((v >> 7) & 0x7f);
	p[3] = (uint8_t)(v & 0x7f);
}

// The format from the picture's own first bytes, which is also the MIME type the
// tag needs. NULL for anything else: the caller has already been given bytes
// that are not a picture, and no tag is written for them.
static const char *image_mime(const uint8_t *data, size_t size) {
	if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
		return "image/jpeg";
	}
	if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
		return "image/png";
	}
	return NULL;
}

// Width, height, colour depth and colour count for the FLAC PICTURE block. The
// block may legitimately say 0 for all four, but a reader that is told nothing
// has to decode the picture before it can lay anything out, and some do not
// bother. Both parsers are the minimum that answers the question.
static void image_dims(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h, uint32_t *depth,
					   uint32_t *colors) {
	*w = *h = *depth = *colors = 0;
	if (!data || size < 12) {
		return;
	}
	if (size >= 26 && data[0] == 0x89 && data[1] == 'P') {
		*w = be32(data + 16);
		*h = be32(data + 20);
		uint8_t color_type = data[25];
		switch (color_type) {
		case 0:
		case 4: *depth = 8; break;
		case 2:
		case 6: *depth = 24; break;
		case 3: *depth = 8; *colors = 256; break;
		default: *depth = 24; break;
		}
		return;
	}
	if (!(data[0] == 0xFF && data[1] == 0xD8)) {
		return;
	}
	size_t at = 2;
	while (at + 4 < size) {
		if (data[at] != 0xFF) {
			at++;
			continue;
		}
		uint8_t marker = data[at + 1];
		// Standalone markers carry no length.
		if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
			at += 2;
			continue;
		}
		if (at + 4 > size) {
			return;
		}
		size_t seg = ((size_t)data[at + 2] << 8) | data[at + 3];
		// SOF0..SOF15, except the four that are not frame headers.
		bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
		if (sof) {
			if (at + 10 > size) {
				return;
			}
			uint8_t precision = data[at + 4];
			uint32_t height = (uint32_t)((data[at + 5] << 8) | data[at + 6]);
			uint32_t width = (uint32_t)((data[at + 7] << 8) | data[at + 8]);
			uint8_t components = data[at + 9];
			*w = width;
			*h = height;
			*depth = (uint32_t)precision * components;
			return;
		}
		if (seg < 2) {
			return;
		}
		at += 2 + seg;
	}
}

// Copies `len` bytes from `in` at `off` into `out`, in chunks: a song is tens of
// megabytes and this device has no business holding one. A negative `len` means
// "to the end of the file", which is how the audio after the tags is copied.
static bool copy_range(FILE *in, FILE *out, long long off, long long len) {
	uint8_t *buffer = malloc(COPY_CHUNK);
	if (!buffer) {
		return false;
	}
	// fseeko, not fseek: on this device `long` is 32 bits whatever
	// _FILE_OFFSET_BITS says, and a card can hold a file past two gigabytes.
	if (fseeko(in, (off_t)off, SEEK_SET) != 0) {
		free(buffer);
		return false;
	}
	bool to_eof = len < 0;
	bool ok = true;
	while ((to_eof || len > 0) && ok) {
		size_t want = (!to_eof && len < COPY_CHUNK) ? (size_t)len : COPY_CHUNK;
		size_t got = fread(buffer, 1, want, in);
		if (got == 0) {
			ok = to_eof; // running out at the end of a file is the end, not a fault
			break;
		}
		if (fwrite(buffer, 1, got, out) != got) {
			ok = false;
			break;
		}
		if (!to_eof) {
			len -= (long long)got;
		}
	}
	free(buffer);
	return ok;
}

// ---------------------------------------------------------------------------
// MP3 / ID3v2
// ---------------------------------------------------------------------------

// The frames worth keeping: everything except the pictures, which are what this
// is replacing. `frames` is the old tag read whole; the copy is appended to a
// buffer the caller writes out.
static bool mp3_keep_frames(const uint8_t *tag, size_t tag_size, int major, uint8_t **out,
							size_t *out_size) {
	*out = NULL;
	*out_size = 0;
	if (tag_size <= 10) {
		return true; // an empty tag: nothing to keep
	}
	uint8_t *kept = malloc(tag_size);
	if (!kept) {
		return false;
	}
	size_t used = 0;
	size_t at = 10;
	while (at + 10 <= tag_size) {
		const uint8_t *h = tag + at;
		if (h[0] == 0) {
			break; // padding from here on
		}
		char id[5];
		memcpy(id, h, 4);
		id[4] = '\0';
		// A frame id is four capital letters or digits. Anything else means the
		// walk has left the frames (or the tag is damaged), and stopping here
		// keeps what was read rather than inventing sizes.
		for (int i = 0; i < 4; i++) {
			if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) {
				at = tag_size;
				break;
			}
		}
		if (at >= tag_size) {
			break;
		}
		uint32_t frame_size = major >= 4 ? syncsafe32(h + 4) : be32(h + 4);
		if (frame_size == 0 || frame_size > tag_size - at - 10) {
			break;
		}
		bool picture = strcmp(id, "APIC") == 0 || strcmp(id, "PIC") == 0;
		if (!picture) {
			memcpy(kept + used, h, 10 + (size_t)frame_size);
			used += 10 + (size_t)frame_size;
		}
		at += 10 + (size_t)frame_size;
	}
	*out = kept;
	*out_size = used;
	return true;
}

static bool mp3_write(FILE *in, const char *tmp_path, const uint8_t *cover, size_t cover_size,
					  const char *mime, char *error, size_t error_size) {
	uint8_t header[10];
	memset(header, 0, sizeof(header));
	bool has_tag = fread(header, 1, sizeof(header), in) == sizeof(header) &&
				   memcmp(header, "ID3", 3) == 0;
	int major = 3;
	uint32_t tag_body = 0;
	uint8_t flags = 0;
	if (has_tag) {
		major = header[3];
		flags = header[5];
		tag_body = syncsafe32(header + 6);
		if (major < 3 || major > 4) {
			// ID3v2.2 used three-character frame ids and has no APIC; rewriting
			// one would be a second tag format. Refused rather than guessed at.
			set_error(error, error_size, "artwork_tag_unsupported");
			return false;
		}
		if ((flags & 0x80) || (flags & 0x40)) {
			// Whole-tag unsynchronisation or an extended header: the frames are
			// not where this reader looks for them. Rare, and refused.
			set_error(error, error_size, "artwork_tag_unsupported");
			return false;
		}
		if (tag_body > MP3_TAG_MAX) {
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
	}

	uint8_t *tag = NULL;
	size_t tag_size = 0;
	if (has_tag) {
		tag_size = 10 + (size_t)tag_body + ((flags & 0x10) ? 10 : 0); // footer, v2.4
		tag = malloc(tag_size);
		if (!tag) {
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
		memcpy(tag, header, 10);
		if (fread(tag + 10, 1, tag_size - 10, in) != tag_size - 10) {
			free(tag);
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
	}
	long long audio_at = has_tag ? (long long)tag_size : 0;

	uint8_t *kept = NULL;
	size_t kept_size = 0;
	if (has_tag) {
		if (!mp3_keep_frames(tag, tag_size, major, &kept, &kept_size)) {
			free(tag);
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
		free(tag);
	}

	// The new picture, as an APIC frame: encoding 0 (ISO-8859-1, one byte per
	// character and no BOM to argue about), the MIME type, a NUL, the picture
	// type 3 (front cover), an empty description, then the bytes.
	size_t mime_len = strlen(mime);
	size_t body = 1 + mime_len + 1 + 1 + 1 + cover_size;
	uint8_t *apic = malloc(10 + body);
	if (!apic) {
		free(kept);
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}
	memcpy(apic, "APIC", 4);
	if (major >= 4) {
		put_syncsafe32(apic + 4, (uint32_t)body);
	} else {
		put_be32(apic + 4, (uint32_t)body);
	}
	apic[8] = 0;
	apic[9] = 0;
	size_t at = 10;
	apic[at++] = 0; // encoding
	memcpy(apic + at, mime, mime_len);
	at += mime_len;
	apic[at++] = 0; // MIME terminator
	apic[at++] = 3; // front cover
	apic[at++] = 0; // empty description
	memcpy(apic + at, cover, cover_size);

	// The header's size counts every byte after the ten-byte tag header: the
	// new frame together with its own ten-byte frame header, then the frames
	// kept from the old tag. Leaving the frame header out would claim a tag
	// ten bytes shorter than the one written, and the decoder would start the
	// audio inside the last kept frame.
	size_t total = 10 + 10 + body + kept_size;
	if (total > MP3_TAG_MAX) {
		free(apic);
		free(kept);
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}

	FILE *out = fopen(tmp_path, "wb");
	if (!out) {
		free(apic);
		free(kept);
		set_error(error, error_size, "card_write_failed");
		return false;
	}
	uint8_t tag_header[10] = {'I', 'D', '3', (uint8_t)major, 0, 0, 0, 0, 0, 0};
	put_syncsafe32(tag_header + 6, (uint32_t)(10 + body + kept_size));
	bool ok = fwrite(tag_header, 1, sizeof(tag_header), out) == sizeof(tag_header);
	ok = ok && fwrite(apic, 1, 10 + body, out) == 10 + body;
	if (ok && kept_size > 0) {
		ok = fwrite(kept, 1, kept_size, out) == kept_size;
	}
	ok = ok && copy_range(in, out, audio_at, -1);
	free(apic);
	free(kept);
	if (fclose(out) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(tmp_path);
		set_error(error, error_size, "card_write_failed");
	}
	return ok;
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------

// The PICTURE block: type 3 (front cover), MIME, empty description, the picture
// dimensions from the picture itself, then its bytes.
static uint8_t *flac_picture_block(const uint8_t *cover, size_t cover_size, const char *mime,
								   size_t *block_size) {
	uint32_t w, h, depth, colors;
	image_dims(cover, cover_size, &w, &h, &depth, &colors);
	size_t mime_len = strlen(mime);
	size_t body = 4 + 4 + mime_len + 4 + 4 + 4 + 4 + 4 + 4 + cover_size;
	if (body + 4 > 0xFFFFFF) {
		return NULL; // a block length is 24 bits
	}
	uint8_t *block = calloc(1, body + 4);
	if (!block) {
		return NULL;
	}
	// The metadata block header: not last (the caller sets the bit), type 6.
	block[0] = 6;
	block[1] = (uint8_t)(body >> 16);
	block[2] = (uint8_t)(body >> 8);
	block[3] = (uint8_t)body;
	size_t at = 4;
	put_be32(block + at, 3);
	at += 4;
	put_be32(block + at, (uint32_t)mime_len);
	at += 4;
	memcpy(block + at, mime, mime_len);
	at += mime_len;
	put_be32(block + at, 0); // empty description
	at += 4;
	put_be32(block + at, w);
	at += 4;
	put_be32(block + at, h);
	at += 4;
	put_be32(block + at, depth);
	at += 4;
	put_be32(block + at, colors);
	at += 4;
	put_be32(block + at, (uint32_t)cover_size);
	at += 4;
	memcpy(block + at, cover, cover_size);
	*block_size = body + 4;
	return block;
}

typedef struct {
	uint8_t type;
	long long offset; // of the block's data, past its 4-byte header
	uint32_t length;
} flac_block_t;

#define FLAC_BLOCKS_MAX 64

static bool flac_write(FILE *in, const char *tmp_path, const uint8_t *cover, size_t cover_size,
					   const char *mime, char *error, size_t error_size) {
	uint8_t magic[4];
	if (fread(magic, 1, 4, in) != 4 || memcmp(magic, "fLaC", 4) != 0) {
		set_error(error, error_size, "artwork_tag_unsupported");
		return false;
	}
	flac_block_t blocks[FLAC_BLOCKS_MAX];
	int count = 0;
	long long at = 4;
	bool last = false;
	while (!last) {
		uint8_t h[4];
		if (fseeko(in, (off_t)at, SEEK_SET) != 0 || fread(h, 1, 4, in) != 4) {
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
		last = (h[0] & 0x80) != 0;
		uint8_t type = h[0] & 0x7f;
		uint32_t len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
		if (count == 0 && type != 0) {
			set_error(error, error_size, "artwork_tag_failed"); // STREAMINFO must be first
			return false;
		}
		if (count >= FLAC_BLOCKS_MAX) {
			set_error(error, error_size, "artwork_tag_failed");
			return false;
		}
		blocks[count].type = type;
		blocks[count].offset = at + 4;
		blocks[count].length = len;
		count++;
		at += 4 + (long long)len;
	}
	long long audio_at = at;

	size_t picture_size = 0;
	uint8_t *picture = flac_picture_block(cover, cover_size, mime, &picture_size);
	if (!picture) {
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}

	// The new order: STREAMINFO, the picture, then everything that was not a
	// picture. Exactly one block carries the last flag, and it is the one
	// written last.
	int order[FLAC_BLOCKS_MAX + 1];
	int order_count = 0;
	for (int i = 0; i < count; i++) {
		if (blocks[i].type == 0) {
			order[order_count++] = i;
			break;
		}
	}
	int picture_slot = order_count;
	order[order_count++] = -1; // the new picture
	for (int i = 0; i < count; i++) {
		if (blocks[i].type == 0 || blocks[i].type == 6) {
			continue;
		}
		order[order_count++] = i;
	}

	FILE *out = fopen(tmp_path, "wb");
	if (!out) {
		free(picture);
		set_error(error, error_size, "card_write_failed");
		return false;
	}
	bool ok = fwrite(magic, 1, 4, out) == 4;
	for (int k = 0; ok && k < order_count; k++) {
		uint8_t header[4];
		uint32_t len;
		long long data_at;
		if (k == picture_slot) {
			memcpy(header, picture, 4);
			len = (uint32_t)(picture_size - 4);
			data_at = -1;
		} else {
			flac_block_t *b = &blocks[order[k]];
			header[0] = b->type;
			header[1] = (uint8_t)(b->length >> 16);
			header[2] = (uint8_t)(b->length >> 8);
			header[3] = (uint8_t)b->length;
			len = b->length;
			data_at = b->offset;
		}
		header[0] &= 0x7f;
		if (k == order_count - 1) {
			header[0] |= 0x80;
		}
		ok = fwrite(header, 1, 4, out) == 4;
		if (!ok) {
			break;
		}
		if (k == picture_slot) {
			ok = fwrite(picture + 4, 1, len, out) == len;
		} else {
			ok = copy_range(in, out, data_at, (long long)len);
		}
	}
	free(picture);
	ok = ok && copy_range(in, out, audio_at, -1);
	if (fclose(out) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(tmp_path);
		set_error(error, error_size, "card_write_failed");
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the one entry point
// ---------------------------------------------------------------------------

static bool has_extension(const char *path, const char *ext) {
	size_t n = strlen(path), m = strlen(ext);
	return n > m && strcasecmp(path + n - m, ext) == 0;
}

bool tagwrite_supported(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	// The extension is the promise; the magic number is checked when the file is
	// opened, since an .mp3 that is really something else must not be rewritten
	// as one.
	return has_extension(path, ".mp3") || has_extension(path, ".mp2") ||
		   has_extension(path, ".flac");
}

bool tagwrite_set_cover(const char *path, const void *data, size_t size, char *error,
						size_t error_size) {
	if (error && error_size) {
		error[0] = '\0';
	}
	if (!path || !path[0] || !data || size == 0) {
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}
	const char *mime = image_mime(data, size);
	if (!mime) {
		set_error(error, error_size, "artwork_bad_image");
		return false;
	}
	if (!tagwrite_supported(path)) {
		set_error(error, error_size, "artwork_tag_unsupported");
		return false;
	}

	pthread_mutex_lock(&tag_lock);
	FILE *in = fopen(path, "rb");
	if (!in) {
		pthread_mutex_unlock(&tag_lock);
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}
	struct stat st;
	if (fstat(fileno(in), &st) != 0) {
		fclose(in);
		pthread_mutex_unlock(&tag_lock);
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}

	// The temporary lives in the same folder: a rename is only atomic within
	// one filesystem, and a card's folder is where the space has to come from
	// anyway.
	char tmp_path[1200];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tagtmp", path);
	if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
		fclose(in);
		pthread_mutex_unlock(&tag_lock);
		set_error(error, error_size, "artwork_tag_failed");
		return false;
	}

	uint8_t head[4] = {0};
	size_t got = fread(head, 1, 4, in);
	rewind(in);
	bool ok;
	if (got == 4 && memcmp(head, "fLaC", 4) == 0) {
		ok = flac_write(in, tmp_path, data, size, mime, error, error_size);
	} else if (has_extension(path, ".mp3") || has_extension(path, ".mp2")) {
		ok = mp3_write(in, tmp_path, data, size, mime, error, error_size);
	} else {
		// A .flac that does not start with fLaC, or a file whose extension and
		// contents disagree: nothing is written.
		set_error(error, error_size, "artwork_tag_unsupported");
		ok = false;
	}
	fclose(in);
	if (!ok) {
		remove(tmp_path);
		pthread_mutex_unlock(&tag_lock);
		return false;
	}
	// The mode travels: a song that was writable stays writable, and one that
	// was not does not become so.
	chmod(tmp_path, st.st_mode & 07777);
#ifdef _WIN32
	// Windows' rename() refuses to replace a file that exists; POSIX's, which
	// is what the player runs, replaces it in one step. The host build is the
	// only caller that meets the former, and there the original is removed
	// first -- with a complete, closed replacement sitting beside it.
	remove(path);
#endif
	if (rename(tmp_path, path) != 0) {
		remove(tmp_path);
		pthread_mutex_unlock(&tag_lock);
		set_error(error, error_size, "card_write_failed");
		return false;
	}
	pthread_mutex_unlock(&tag_lock);
	fprintf(stderr, "tagwrite: %s now carries a %s cover (%zu bytes)\n", path, mime, size);
	return true;
}
