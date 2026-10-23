// Headless checks of the Bootlogo folder: which files the page offers, and
// what becomes of the one that is chosen.
//
// The pictures are made here rather than shipped: a real JPEG at a known size
// and of a known colour, written with the same encoder the player uses to make
// the copy. So this is the whole path a tap takes -- a file on a card in, a
// 240x320 baseline JPEG out -- and the end of it is checked by decoding the
// copy back and looking at what is in it.
//
// Nothing here draws anything and nothing here touches LVGL. The copy goes to
// a temporary directory, through SONIX_BOOTPIC_DEST.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/device/bootpic.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/library/jpeginfo.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic ignored "-Wunused-function"
#include "src/system/image/stb_image_write.h"

static char root[512]; // the card
static char dest[512]; // where the copy is put

/* ---- making files --------------------------------------------------------- */

static bool write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	fputs(text, f);
	fclose(f);
	return true;
}

// A real JPEG at `path`, `w` x `h`, filled top to bottom with `bands`
// horizontal bands of colour: the shape of it is what the crop is checked
// against, so the picture has to say where its own top and bottom are.
static bool write_bands(const char *path, int w, int h, const uint8_t bands[][3], int band_count) {
	uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
	assert(rgb);
	for (int y = 0; y < h; y++) {
		const uint8_t *c = bands[y * band_count / h];
		for (int x = 0; x < w; x++) {
			uint8_t *p = rgb + ((size_t)y * w + x) * 3;
			p[0] = c[0];
			p[1] = c[1];
			p[2] = c[2];
		}
	}
	int ok = stbi_write_jpg(path, w, h, 3, rgb, 95);
	free(rgb);
	return ok != 0;
}

// The same, split down the middle instead: a left half and a right half.
static bool write_halves(const char *path, int w, int h, const uint8_t left[3], const uint8_t right[3]) {
	uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
	assert(rgb);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t *p = rgb + ((size_t)y * w + x) * 3;
			const uint8_t *c = x < w / 2 ? left : right;
			p[0] = c[0];
			p[1] = c[1];
			p[2] = c[2];
		}
	}
	int ok = stbi_write_jpg(path, w, h, 3, rgb, 95);
	free(rgb);
	return ok != 0;
}

static uint8_t *decode(const char *path, int *w, int *h) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	rewind(f);
	uint8_t *data = malloc((size_t)size);
	assert(data);
	assert(fread(data, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	uint8_t *rgb = jpeg_scaled_decode(data, (size_t)size, 480, w, h, NULL, NULL);
	free(data);
	return rgb;
}

// What a rectangle of a decoded picture is made of, as one mean colour.
static void mean(const uint8_t *rgb, int w, int x0, int y0, int x1, int y1, double out[3]) {
	double sum[3] = {0, 0, 0};
	long n = 0;
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
			sum[0] += p[0];
			sum[1] += p[1];
			sum[2] += p[2];
			n++;
		}
	}
	out[0] = sum[0] / n;
	out[1] = sum[1] / n;
	out[2] = sum[2] / n;
}

/* ---- the folder ----------------------------------------------------------- */

static void check_folder(void) {
	char folder[600];
	snprintf(folder, sizeof(folder), "%s/Bootlogo", root);
	assert(mkdir(folder, 0755) == 0);

	// What the page offers, and what it passes over: the extensions a camera
	// uses, the case it uses them in, and nothing that is not a picture --
	// including the file a Mac leaves beside every one.
	const char *const made[] = {"b.jpg", "A.JPEG", "c.png", ".hidden.jpg", "notes.txt", "d.jpg"};
	for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", folder, made[i]);
		assert(write_file(path, "not a picture, but a name is all the list needs"));
	}

	char names[8][BOOTPIC_NAME_MAX + 1];
	int count = bootpic_list(names, 8);
	assert(count == 4);
	assert(strcmp(names[0], "A.JPEG") == 0); // alphabetical, case aside
	assert(strcmp(names[1], "b.jpg") == 0);
	assert(strcmp(names[2], "c.png") == 0); // a PNG is a picture too
	assert(strcmp(names[3], "d.jpg") == 0);

	// The cap: a folder of thousands is somebody's photo library, and the
	// page only has room for a page.
	char few[2][BOOTPIC_NAME_MAX + 1];
	assert(bootpic_list(few, 2) == 2);
	assert(strcmp(few[0], "A.JPEG") == 0);

	// No card, no pictures -- and no folder named either.
	bootpic_set_root("");
	assert(bootpic_list(names, 8) == 0);
	assert(strcmp(bootpic_dir(), "") == 0);
	bootpic_set_root(root);

	char wanted[600];
	snprintf(wanted, sizeof(wanted), "%s/Bootlogo", root);
	assert(strcmp(bootpic_dir(), wanted) == 0);
}

/* ---- the copy ------------------------------------------------------------- */

static void check_copy(void) {
	char folder[600];
	snprintf(folder, sizeof(folder), "%s/Bootlogo", root);

	// A landscape photograph: 640x480, red on the left, blue on the right.
	// Filled to a 240x320 panel it must be cut to the middle 360 columns,
	// which leaves exactly half of each colour -- and no black bands above
	// and below it, which is what fitting rather than filling would give.
	static const uint8_t red[3] = {200, 20, 20};
	static const uint8_t blue[3] = {20, 20, 200};
	char wide[1024];
	snprintf(wide, sizeof(wide), "%s/wide.jpg", folder);
	assert(write_halves(wide, 640, 480, red, blue));

	bootpic_result_t r = bootpic_install("wide.jpg", 240, 320);
	assert(r == BOOTPIC_OK);

	// The size and the coding of what the boot script will draw: the scaled
	// decoder cannot read a progressive file, which is the one shape a boot
	// screen must never be.
	jpeg_info_t info;
	assert(jpeg_info_read(dest, &info));
	assert(info.width == 240 && info.height == 320);
	assert(!info.progressive);

	// Nothing left beside it: the copy is written under another name and
	// moved over the real one, so a power-on that catches the write halfway
	// cannot draw half a picture.
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
	assert(access(tmp, F_OK) != 0);

	// And what is in it.
	int w = 0, h = 0;
	uint8_t *rgb = decode(dest, &w, &h);
	assert(rgb);
	assert(w == 240 && h == 320);
	double left[3], right[3];
	mean(rgb, w, 0, h / 3, w / 2, 2 * h / 3, left);
	mean(rgb, w, w / 2, h / 3, w, 2 * h / 3, right);
	assert(left[0] > 150 && left[2] < 70);	// red
	assert(right[2] > 150 && right[0] < 70); // blue
	free(rgb);

	// A portrait picture is cut the other way. This one is green on top and
	// white below, and 480x640 is the panel's own shape: nothing is cut at
	// all, so both halves have to come through as they are.
	static const uint8_t bands[2][3] = {{10, 180, 40}, {235, 235, 235}};
	char tall[1024];
	snprintf(tall, sizeof(tall), "%s/tall.png", folder);
	assert(write_bands(tall, 480, 640, bands, 2));
	assert(bootpic_install("tall.png", 240, 320) == BOOTPIC_OK);

	rgb = decode(dest, &w, &h);
	assert(rgb);
	double top[3], bottom[3];
	mean(rgb, w, 0, 0, w, h / 4, top);
	mean(rgb, w, 0, 3 * h / 4, w, h, bottom);
	assert(top[1] > 130 && top[1] > top[0] + 60);   // green
	assert(bottom[0] > 190 && bottom[2] > 190);	   // white
	free(rgb);

	// The other way round: a picture far taller than the panel loses its top
	// and its bottom and keeps its middle.
	char column[1024];
	snprintf(column, sizeof(column), "%s/column.jpg", folder);
	static const uint8_t three[3][3] = {{220, 20, 20}, {20, 200, 20}, {20, 20, 220}};
	assert(write_bands(column, 240, 960, three, 3));
	assert(bootpic_install("column.jpg", 240, 320) == BOOTPIC_OK);

	rgb = decode(dest, &w, &h);
	assert(rgb);
	double middle[3];
	mean(rgb, w, 0, h / 3, w, 2 * h / 3, middle);
	assert(middle[1] > 130 && middle[1] > middle[0] + 60); // the green band, and not the red one
	free(rgb);

	// A picture smaller than the panel is enlarged rather than refused: a
	// boot screen is one picture at one size, and a 100x100 one is a picture
	// somebody chose.
	char small[1024];
	snprintf(small, sizeof(small), "%s/small.jpg", folder);
	assert(write_bands(small, 100, 100, bands, 2));
	assert(bootpic_install("small.jpg", 240, 320) == BOOTPIC_OK);
	assert(jpeg_info_read(dest, &info) && info.width == 240 && info.height == 320);
}

/* ---- what cannot be made into a boot screen ------------------------------- */

static void check_refusals(void) {
	char folder[600];
	snprintf(folder, sizeof(folder), "%s/Bootlogo", root);

	// A name that is no longer in the folder: a card that has been through a
	// computer since the page listed it.
	assert(bootpic_install("gone.jpg", 240, 320) == BOOTPIC_NO_FILE);

	// A file that is not a picture, wearing a picture's name. The one the
	// page cannot see coming, because the list is names and not decodes.
	char junk[1024];
	snprintf(junk, sizeof(junk), "%s/junk.jpg", folder);
	assert(write_file(junk, "this is not a JPEG at all"));
	assert(bootpic_install("junk.jpg", 240, 320) == BOOTPIC_UNREADABLE);

	// A directory with a picture's name: stat() says what it is, and a
	// decode of one would not.
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/folder.jpg", folder);
	assert(mkdir(dir, 0755) == 0);
	assert(bootpic_install("folder.jpg", 240, 320) == BOOTPIC_NO_FILE);

	// Names that are not names of files in the folder.
	assert(bootpic_install("../outside.jpg", 240, 320) == BOOTPIC_NO_FILE);
	assert(bootpic_install("", 240, 320) == BOOTPIC_NO_FILE);
	assert(bootpic_install(NULL, 240, 320) == BOOTPIC_NO_FILE);
	assert(bootpic_install("wide.jpg", 0, 0) == BOOTPIC_NO_FILE);

	// A file past the size this will read.
	char huge[1024];
	snprintf(huge, sizeof(huge), "%s/huge.jpg", folder);
	FILE *f = fopen(huge, "wb");
	assert(f);
	assert(fseek(f, 13 * 1024 * 1024 - 1, SEEK_SET) == 0);
	assert(fputc('x', f) == 'x');
	fclose(f);
	assert(bootpic_install("huge.jpg", 240, 320) == BOOTPIC_TOO_LARGE);

	// Every refusal has a message of its own: they are all things the user
	// can put right, so they all have to say which one it was.
	const char *const keys[] = {bootpic_result_key(BOOTPIC_NO_CARD),	 bootpic_result_key(BOOTPIC_NO_FILE),
								bootpic_result_key(BOOTPIC_UNREADABLE),	 bootpic_result_key(BOOTPIC_TOO_LARGE),
								bootpic_result_key(BOOTPIC_NO_MEMORY),	 bootpic_result_key(BOOTPIC_WRITE_FAILED)};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		assert(keys[i] && keys[i][0]);
		for (size_t j = i + 1; j < sizeof(keys) / sizeof(keys[0]); j++) {
			assert(strcmp(keys[i], keys[j]) != 0);
		}
	}

	// A copy is already there from check_copy() above, and none of the
	// refusals took it away: a picture that cannot be replaced keeps the one
	// that could.
	assert(bootpic_present());

	// No card at all.
	bootpic_set_root("");
	assert(bootpic_install("wide.jpg", 240, 320) == BOOTPIC_NO_CARD);
	assert(bootpic_present()); // the copy lives on the device, not on the card
	bootpic_set_root(root);
}

static void check_size(void) {
	int w = 0, h = 0;
	assert(bootpic_size("wide.jpg", &w, &h) && w == 640 && h == 480);
	assert(!bootpic_size("notes.txt", &w, &h));
	assert(!bootpic_size("gone.jpg", &w, &h));
	assert(!bootpic_size(NULL, &w, &h));

	bootpic_set_root("");
	assert(!bootpic_size("wide.jpg", &w, &h)); // no card, no answer
	bootpic_set_root(root);
}

int main(void) {
	char tmp[] = "/tmp/sonix-bootpic-test-XXXXXX";
	assert(mkdtemp(tmp));
	snprintf(root, sizeof(root), "%s/card", tmp);
	snprintf(dest, sizeof(dest), "%s/bootlogo_custom.jpeg", tmp);
	assert(mkdir(root, 0755) == 0);
	assert(setenv("SONIX_BOOTPIC_DEST", dest, 1) == 0);

	// No copy before one is made, and no card named until one is.
	bootpic_set_root(NULL);
	assert(!bootpic_present());
	assert(strcmp(bootpic_dir(), "") == 0);
	bootpic_set_root(root);

	check_folder();
	check_copy();
	check_refusals();
	check_size();

	printf("PASS boot pictures (folder rules, the copy at 240x320, the refusals)\n");
	return 0;
}
