#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the information page has to show: the two version numbers, and how much
// space is left on the card.

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------
//
// Not compiled into the binary but read from /usr/resource/sonix/components/system-info.json, a
// text file next to the language directory:
//
//     {
//         "device-name": "HiBy R3 Pro II",
//         "dac-info": "Dual Cirrus Logic CS43198",
//         "OS_version": "1.0",
//         "Build_version": "182",
//         "ota-repo": "Jepl4r/sonix-player",
//         "forked-by": "..."
//     }
//
// "OS_version" is the version the update check compares release tags against,
// so an image that leaves it as a word rather than a number can never be told
// apart from the newest release. A build that cuts releases of its own writes
// the release's version here.
//
// This way updating the build number does not mean rebuilding, and whoever
// assembles a firmware image can write it from the build script. A missing or
// unreadable file is not an error: the entries show a dash.
//
// "device-name" is printed on the About page. It no longer decides anything:
// this build is the TempoTec V1's, and sysinfo_model() answers with that model
// whatever the file says (see below).

// Rereads the file. Called once at startup; the functions below do not touch
// the disk.
void sysinfo_load(void);

// "" when the file is missing or does not carry the key.
const char *sysinfo_os_version(void);
const char *sysinfo_build_version(void);

// The name of the player, verbatim from the file -- "TempoTec V1" in the
// images this tree builds. "" when the file does not say.
const char *sysinfo_device_name(void);

// The converter, verbatim from the file, for the information page to print.
// The player never decides anything by it.
const char *sysinfo_dac_info(void);

// The GitHub repository the update from the internet looks in, "owner/name",
// verbatim from the file (ota.h). "" when the file does not say.
const char *sysinfo_ota_repo(void);

// Who forked this build, verbatim from the file, for the information page to
// print. "" when the file does not say, which is every image that does not
// write the key: those show the upstream fork's credit.
const char *sysinfo_forked_by(void);

// The running kernel's release, from uname() -- "4.4.94" on the device, the
// host's own on the simulator. "" only when uname() fails.
const char *sysinfo_kernel_version(void);

// ---------------------------------------------------------------------------
// the model: the TempoTec V1, and nothing else
// ---------------------------------------------------------------------------
//
// One entry, returned by sysinfo_model() whatever system-info.json names. The
// HiBy R3 Pro II and R1 entries that used to sit beside it are gone with their
// builds.
//
// The panel is written here rather than read from the framebuffer: the pages
// subtract from screen_height in unsigned arithmetic, and LVGL's fbdev driver
// reports its size only after its file is set.
typedef struct {
	const char *name;		 // as written in system-info.json
	int panel_width;		 // the panel, on the device and on the simulator alike
	int panel_height;
	const char *update_stem; // "<stem>.upt" is the update file this player accepts
	const char *serial_prefix; // what the number on the box starts with
	bool pmic_charger;		   // charged by the AXP2101 alone, no MP2731 (axpcharge.h)
	bool sgm31324_led;		   // the status LED is an SGM31324 RGB controller on I2C bus 0 (led.h)
	bool tap_wake;			   // the touch controller can wake the screen (power.h)
	bool one_flank;			   // every button on the right flank (remap.c)
	bool media_keys_swapped;   // KEY_NEXTSONG is the upper skip key, not next (system.c)
	bool headset_switch_idle_one; // the one jack switch reads 1 with the sockets empty (alsa-controls.c)
} sysinfo_model_t;

// The V1's entry. Never NULL.
const sysinfo_model_t *sysinfo_model(void);

// The V1's entry when width x height is its panel (240x320), NULL otherwise.
// Kept for the one caller that sizes the interface from a framebuffer whose
// reported size the model name cannot be trusted for.
const sysinfo_model_t *sysinfo_model_by_panel(int width, int height);

// The device serial number: the model's prefix plus the first eight hex digits
// (upper case) of the SoC efuse chip id (/proc/jz/efuse/efuse_chip_id, line
// "CHIP_ID: <32 hex>") -- the number printed on the box. "" when the node is
// absent (the host build, or a firmware without the efuse module). Read once
// and kept: efuses do not change.
const char *sysinfo_serial_number(void);

// ---------------------------------------------------------------------------
// microSD card
// ---------------------------------------------------------------------------

typedef struct {
	bool present;	 // false when no card is mounted
	uint64_t total;	 // bytes
	uint64_t used;	 // bytes
	uint64_t free;	 // bytes available to this process, not counting reserved
	int used_percent; // 0..100, rounded
} sysinfo_storage_t;

// false when there is no card or it cannot be queried.
bool sysinfo_sd_usage(sysinfo_storage_t *out);

// "12.3 GB", "512 MB", "48 KB". Decimal point rather than comma: it is the same
// string in all five languages and does not pass through tr().
void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size);

#endif /* SYSINFO_H */
