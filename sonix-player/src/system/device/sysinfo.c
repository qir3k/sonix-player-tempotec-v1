#include "sysinfo.h"

#include "src/system/core/respath.h"

#include "src/system/device/system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#define SYSINFO_FILE SONIX_RESOURCE_DIR "/components/system-info.json"

static char os_version[64];
static char build_version[64];
static char device_name[64];
static char dac_info[96];
static char ota_repo[128];
static char forked_by[96];
static char kernel_version[96];

// The one player this binary knows how to be: the TempoTec Variations V1.
//
// The table used to hold the HiBy R3 Pro II and R1 beside it, chosen by the
// device-name in system-info.json. Those builds are not made from this tree
// any more -- the packer ships a V1 image and nothing else -- so the choice is
// gone with them, and every flag below is the V1's answer rather than a
// branch the compiler had to keep.
static const sysinfo_model_t MODELS[] = {
	{
		// Two CS43131s and both the 3.5 mm and the 4.4 mm outputs: the
		// single-DAC shortcut is not taken, because the regular route logic is
		// the only path that can select the balanced socket.
		.name = "TempoTec V1",
		.panel_width = 240,
		.panel_height = 320,
		.update_stem = "v1",
		.serial_prefix = "V1",
		.pmic_charger = true,
		.sgm31324_led = true,
		.tap_wake = false,
		.one_flank = true,
		// The V1 kernel labels the physical next and previous keys in the
		// opposite order, like the R3 Pro II. Without this correction the
		// physical next key is dispatched through the previous-key handler;
		// after the restart threshold that handler seeks back to zero instead
		// of advancing the queue.
		.media_keys_swapped = true,
		// Measured on the device: /sys/class/switch holds only `headset`, no
		// `balance`, and its state reads 1 with both sockets empty. Read as
		// the HiBy nodes are, that is a 3.5 mm plug that is never pulled out.
		.headset_switch_idle_one = true,
	},
};

// ---------------------------------------------------------------------------
// the version file
// ---------------------------------------------------------------------------

// Case-insensitive strstr. Needed because the key name is typed by hand by
// whoever assembles the firmware image, and "os_version" instead of
// "OS_version" would leave the page showing two dashes and no explanation.
// (strcasestr exists but is a GNU extension needing a feature-test macro; ten
// lines cost less.)
static const char *find_nocase(const char *haystack, const char *needle) {
	size_t n = strlen(needle);
	if (n == 0) {
		return haystack;
	}
	for (const char *p = haystack; *p; p++) {
		size_t i = 0;
		while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return p;
		}
	}
	return NULL;
}

// A miniature JSON reader, deliberately so: it looks for "key", then the colon,
// then the string that follows. It handles no numbers, nesting or escape
// sequences, and does not need to -- the file is defined here and holds two
// strings. All it has to do is survive a badly written file: on anything
// unexpected the field stays empty and the page shows a dash.
static bool json_string_field(const char *text, const char *key, char *out, size_t out_size) {
	out[0] = '\0';

	// The key in quotes, so "Build_version" is not matched inside a longer key
	// that contains it.
	char quoted[64];
	if (snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= (int)sizeof(quoted)) {
		return false;
	}

	const char *p = find_nocase(text, quoted);
	if (!p) {
		return false;
	}
	p += strlen(quoted);

	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != ':') {
		return false;
	}
	p++;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;

	const char *end = strchr(p, '"');
	if (!end) {
		return false;
	}

	size_t len = (size_t)(end - p);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

void sysinfo_load(void) {
	os_version[0] = '\0';
	build_version[0] = '\0';
	device_name[0] = '\0';
	dac_info[0] = '\0';
	ota_repo[0] = '\0';
	forked_by[0] = '\0';

	// The kernel the player is running on. uname() is a syscall, not a file:
	// no open, no parse, and it answers the same on the simulator as on the
	// device. The release is what the About page shows -- "4.4.94", the number
	// the vendor's kernel prints -- and the rest of the string (the build's own
	// suffix) is carried too when it is there.
	struct utsname u;
	if (uname(&u) == 0 && u.release[0]) {
		snprintf(kernel_version, sizeof(kernel_version), "%s", u.release);
	}

	FILE *f = fopen(SYSINFO_FILE, "rb");
	if (!f) {
		printf("sysinfo: %s missing, versions not available\n", SYSINFO_FILE);
		return;
	}

	// The file is a handful of lines; anything longer is not the expected file
	// and gets truncated without ceremony.
	char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	// The names the file uses, matched case-insensitively by find_nocase.
	json_string_field(buf, "OS_version", os_version, sizeof(os_version));
	json_string_field(buf, "Build_version", build_version, sizeof(build_version));
	json_string_field(buf, "device-name", device_name, sizeof(device_name));
	json_string_field(buf, "dac-info", dac_info, sizeof(dac_info));
	json_string_field(buf, "ota-repo", ota_repo, sizeof(ota_repo));
	json_string_field(buf, "forked-by", forked_by, sizeof(forked_by));

	printf("sysinfo: device '%s', system '%s', build '%s'\n", device_name, os_version, build_version);
}

const char *sysinfo_os_version(void) { return os_version; }
const char *sysinfo_build_version(void) { return build_version; }
const char *sysinfo_device_name(void) { return device_name; }
const char *sysinfo_dac_info(void) { return dac_info; }
const char *sysinfo_ota_repo(void) { return ota_repo; }
const char *sysinfo_kernel_version(void) { return kernel_version; }
const char *sysinfo_forked_by(void) { return forked_by; }

// Matched without regard to case or to the spelling of "HiBy": the name is
// typed by hand into a text file, and "Hiby R1" is the same player as
// "HiBy R1".
const sysinfo_model_t *sysinfo_model(void) {
	// Not keyed on device_name any more: this build runs on the V1 and on
	// nothing else, so a system-info.json written by an older packer (or by
	// hand) cannot leave the player without the panel, the update stem and the
	// hardware flags it needs. The name the file carries is still what the
	// About page prints; it just does not decide anything.
	return &MODELS[0];
}

const sysinfo_model_t *sysinfo_model_by_panel(int width, int height) {
	if (width == MODELS[0].panel_width && height == MODELS[0].panel_height) {
		return &MODELS[0];
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// space on the card
// ---------------------------------------------------------------------------

bool sysinfo_sd_usage(sysinfo_storage_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	const char *root = storage_sd_root();
	if (!root) {
		return false;
	}

	struct statvfs st;
	if (statvfs(root, &st) != 0) {
		return false;
	}

	// f_frsize is the block size f_blocks is counted in; f_bsize is the
	// preferred I/O size and not the same thing, even though they match on many
	// filesystems. Some leave f_frsize at zero.
	uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
	if (unit == 0 || st.f_blocks == 0) {
		return false;
	}

	out->present = true;
	out->total = (uint64_t)st.f_blocks * unit;
	// f_bfree counts reserved blocks too, f_bavail only those an ordinary
	// process can really use. For "how much is left" f_bavail is the honest
	// answer; used space is measured against f_bfree, or the figures would not
	// add up to the total.
	out->free = (uint64_t)st.f_bavail * unit;
	out->used = out->total - (uint64_t)st.f_bfree * unit;

	if (out->used > out->total) {
		out->used = out->total;
	}
	out->used_percent = (int)((out->used * 100 + out->total / 2) / out->total);
	if (out->used_percent > 100) {
		out->used_percent = 100;
	}
	return true;
}

void sysinfo_format_size(uint64_t bytes, char *out, size_t out_size) {
	const uint64_t KB = 1024;
	const uint64_t MB = KB * 1024;
	const uint64_t GB = MB * 1024;

	if (bytes >= GB) {
		// One decimal: "58.9 GB" says what is needed, "58.94 GB" does not.
		unsigned long long whole = bytes / GB;
		unsigned long long tenths = ((bytes % GB) * 10) / GB;
		snprintf(out, out_size, "%llu.%llu GB", whole, tenths);
	} else if (bytes >= MB) {
		snprintf(out, out_size, "%llu MB", (unsigned long long)((bytes + MB / 2) / MB));
	} else {
		snprintf(out, out_size, "%llu KB", (unsigned long long)((bytes + KB / 2) / KB));
	}
}

// ---------------------------------------------------------------------------
// serial number
// ---------------------------------------------------------------------------

const char *sysinfo_serial_number(void) {
	// Read once and kept: the efuses are burned at the factory.
	static char serial[24];
	static bool tried;
	if (tried) {
		return serial;
	}
	tried = true;
	serial[0] = '\0';

	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return serial; // no prefix to build it from
	}

	FILE *f = fopen("/proc/jz/efuse/efuse_chip_id", "r");
	if (!f) {
		return serial; // build host, or firmware without the efuse module
	}
	char line[96] = "";
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return serial;
	}
	fclose(f);

	// "CHIP_ID: 90a70a428496c4012f146c0404000001" -- the serial printed on the
	// box is the model's prefix plus the first eight hex digits, uppercased
	// (verified on a real R3 Pro II).
	const char *hex = strchr(line, ':');
	hex = hex ? hex + 1 : line;
	while (*hex == ' ' || *hex == '\t') {
		hex++;
	}

	char id[9];
	int n = 0;
	while (n < 8 && isxdigit((unsigned char)hex[n])) {
		id[n] = (char)toupper((unsigned char)hex[n]);
		n++;
	}
	id[n] = '\0';
	if (n == 8) {
		snprintf(serial, sizeof(serial), "%s%s", model->serial_prefix, id);
	}
	return serial;
}
