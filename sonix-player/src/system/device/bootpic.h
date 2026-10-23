#ifndef BOOTPIC_H
#define BOOTPIC_H

#include <stdbool.h>
#include <stddef.h>

// The Bootlogo folder: a picture of the user's own as the boot screen.
//
// Settings > Appearance > Boot screen offers the four pictures the firmware
// carries (see bootlogo.h) and a fifth, "Custom", which is a picture from the
// card. The folder's rules are the Screensaver folder's, with one difference
// in kind:
//
//   * a folder named Bootlogo in the root of the card, beside Screensaver
//     and Fonts;
//   * JPEG (.jpg, .jpeg) or PNG (.png), of any size, and not hidden -- a name
//     starting with a dot is never a picture somebody put there, and a Mac
//     leaves one beside every file it copies;
//   * picked from the list the page shows, never chosen silently: which of
//     four pictures boots is a setting, which picture of the user's own it is
//     is a choice only the user can make.
//
// Nothing here draws anything, and nothing here runs at boot. The boot script
// cannot read the card -- it runs long before the card is mounted, and the
// marker it reads is fourteen bytes of raw NAND, which is room for WHICH
// picture and not for a path to one. So the picture chosen here is decoded,
// fitted to the panel and written as one baseline JPEG into the writable
// partition, at a fixed path the boot script can name without being told it
// (see the packer's choice block). That copy is what the next power-on draws.
//
// One path, and it is the device's own: SONIX_BOOTPIC_DEST moves it for the
// headless tests, the way SONIX_CONFIG moves the settings for them.
//
// Which is why it is a copy and not a link. A boot screen that comes and goes
// with the card is a boot screen nobody can rely on, and one whose file has
// since been deleted or renamed on a computer still boots. The other half of
// the bargain is that the copy is a snapshot: changing the file on the card
// means choosing it again, which is a tap.
//
// The picture is scaled to FILL the panel and cropped where it overflows,
// rather than letterboxed: a boot screen with black bands above and below it
// reads as a picture that did not fit, and a panel is filled edge to edge by
// everything else that is drawn on it.

// The longest file name the folder is read for.
#define BOOTPIC_NAME_MAX 255

// Why a picture could not be made into the boot screen. Every one of these is
// something the user can put right, so every one of them is said: see
// bootpic_result_key().
typedef enum {
	BOOTPIC_OK = 0,
	BOOTPIC_NO_CARD,	// no card mounted, so no folder to read
	BOOTPIC_NO_FILE,	// the name is not in the folder any more
	BOOTPIC_UNREADABLE, // neither decoder could read it
	BOOTPIC_TOO_LARGE,	// a file bigger than the decode budget allows
	BOOTPIC_NO_MEMORY,	// the decode or the encode did not fit
	BOOTPIC_WRITE_FAILED, // the copy could not be written to the partition
} bootpic_result_t;

// Where the card is mounted. Called at startup and again when a card is
// attached; passing NULL or "" leaves the folder unreachable, which is the
// right answer with no card in.
void bootpic_set_root(const char *card_root);

// The folder itself, or "" when there is no card. Shown on the page, so a
// folder that is simply in the wrong place can be found.
const char *bootpic_dir(void);

// The path the boot script draws: the copy in the writable partition. A fixed
// name, which is what lets the boot script find it without being told.
const char *bootpic_path(void);

// Whether that copy is there. What the boot screen choice is measured
// against: "Custom" with no copy behind it means nothing, and the boot script
// would draw the stock picture in its place.
bool bootpic_present(void);

// The pictures in the folder, as file names, alphabetical and without their
// path, up to `max` of them. Returns how many were found; zero when there is
// no card, no folder, or nothing usable in it. A file that cannot be decoded
// is still listed -- whether it can be is only known when it is chosen, and a
// picture the list hides is a picture nobody can ask about.
int bootpic_list(char names[][BOOTPIC_NAME_MAX + 1], int max);

// The picture's own size in pixels, read from its headers, for the caption
// under its name. False when the file cannot be opened or is not a picture
// these decoders name a size for.
bool bootpic_size(const char *name, int *width, int *height);

// Makes one named file in the folder into the boot screen: decodes it, fits
// it to `panel_w` x `panel_h`, writes it as a baseline JPEG to the path above
// and returns BOOTPIC_OK, or the reason it could not.
//
// Slow enough to be worth its own thread -- a decode of a camera-sized JPEG
// is most of a second on this core -- and safe to call off the interface
// thread: it touches no state but its own.
bootpic_result_t bootpic_install(const char *name, int panel_w, int panel_h);

// The translation tag for a result, for the toast after a failed pick. OK
// maps to no message at all: a pick that worked says so with the picture's
// name rather than with a sentence.
const char *bootpic_result_key(bootpic_result_t result);

#endif /* BOOTPIC_H */
