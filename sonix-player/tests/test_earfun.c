// A host-side check of the EarFun frame encoder and decoders, with the sockets
// stubbed out: the worker is never started, so nothing here touches Bluetooth.
// It drives earfun_feed_frame() with the replies the earbuds would send.
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/system/bluetooth/earfun.h"

// --- the stubs: nothing in this test opens a socket -----------------------
int socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; return -1; }
int connect(int fd, const struct sockaddr *addr, socklen_t length) { (void)fd; (void)addr; (void)length; return -1; }
int getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
	(void)fd; (void)level; (void)name; (void)value; (void)length;
	return -1;
}
int close(int fd) { (void)fd; return 0; }
int fcntl(int fd, int command, ...) { (void)fd; (void)command; return 0; }
int poll(struct pollfd *fds, nfds_t count, int timeout) { (void)fds; (void)count; (void)timeout; return 0; }

long config_get_int(const char *s, const char *k, long f) { (void)s; (void)k; return f; }
void config_set_int(const char *s, const char *k, long v) { (void)s; (void)k; (void)v; }
bool config_save(void) { return true; }

// --- the checks -----------------------------------------------------------

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "  FAIL: %s\n", what);
		failures++;
	}
}

static void frame(uint8_t *out, uint16_t command, uint8_t a, uint8_t b) {
	uint8_t payload[2] = {a, b};
	size_t length = earfun_encode_request(command, payload, 2, out, 64);
	assert(length == 10);
}

int main(void) {
	uint8_t encoded[64];

	// The request shape, byte for byte: FF 04 00 len 00 0A cmd(2).
	size_t length = earfun_encode_request(EARFUN_CMD_VERSION, NULL, 0, encoded, sizeof(encoded));
	check(length == 8, "version request is eight bytes");
	check(encoded[0] == 0xFF && encoded[1] == 0x04 && encoded[2] == 0x00 && encoded[3] == 0x00,
		  "header");
	check(encoded[4] == 0x00 && encoded[5] == 0x0A, "GAIA vendor");
	check(encoded[6] == 0x03 && encoded[7] == 0x09, "version command");

	uint8_t request[64];
	uint8_t payload = 1;
	length = earfun_encode_request(EARFUN_CMD_NOISE_SET, &payload, 1, request, sizeof(request));
	check(length == 9 && request[3] == 1 && request[8] == 1, "a set carries its one byte");

	// A reply is the same command with bit 15 set, and the value is the byte
	// after the status byte. Each of these is the exact frame the earbuds send.
	frame(encoded, EARFUN_CMD_BATTERY_LEFT | 0x8000, 0x00, 100);
	earfun_feed_frame(encoded, 10);
	frame(encoded, EARFUN_CMD_BATTERY_RIGHT | 0x8000, 0x00, 42);
	earfun_feed_frame(encoded, 10);
	frame(encoded, EARFUN_CMD_BATTERY_CASE | 0x8000, 0x00, 7);
	earfun_feed_frame(encoded, 10);

	frame(encoded, EARFUN_CMD_NOISE_GET | 0x8000, 0x00, 0x01); // 1 = cancellation
	earfun_feed_frame(encoded, 10);
	frame(encoded, EARFUN_CMD_ANC_GET | 0x8000, 0x00, 0x02);
	earfun_feed_frame(encoded, 10);
	frame(encoded, EARFUN_CMD_GAME_GET | 0x8000, 0x00, 0x01);
	earfun_feed_frame(encoded, 10);
	frame(encoded, EARFUN_CMD_IN_EAR_GET | 0x8000, 0x00, 0x00); // 0 = enabled
	earfun_feed_frame(encoded, 10);

	earfun_state_t state;
	earfun_get(&state);
	check(state.left == 100, "left battery");
	check(state.right == 42, "right battery");
	check(state.case_level == 7, "case battery");
	check(state.have_battery, "battery seen");
	check(state.noise == EARFUN_NOISE_CANCELLATION, "noise mode");
	check(state.anc_level == 2, "ANC strength");
	check(state.game_mode_known && state.game_mode, "game mode");
	check(state.in_ear_known && state.in_ear, "in-ear detection");

	// The version reply is text after the status byte.
	const uint8_t version[] = {0xFF, 0x04, 0x00, 0x0C, 0x00, 0x0A, 0x83, 0x09,
							   0x00, 'T',  'W',  '2',  '0',  '1',  '_',  '1',  '.',  '0',  '.',  '5'};
	earfun_feed_frame(version, sizeof(version));
	earfun_get(&state);
	check(strcmp(state.version, "TW201_1.0.5") == 0, "version text");

	// A battery reply of zero means "not known", and it must not overwrite a
	// reading that was known with one that is not.
	frame(encoded, EARFUN_CMD_BATTERY_LEFT | 0x8000, 0x00, 0x00);
	earfun_feed_frame(encoded, 10);
	earfun_get(&state);
	check(state.left == 100, "a zero reading is ignored");

	// A request echoed back (no reply bit) is not a reading.
	frame(encoded, EARFUN_CMD_NOISE_GET, 0x00, 0x02);
	earfun_feed_frame(encoded, 10);
	earfun_get(&state);
	check(state.noise == EARFUN_NOISE_CANCELLATION, "an echo is not a reply");

	// Nothing to send with no session, and the setters must survive that.
	earfun_set_noise(EARFUN_NOISE_TRANSPARENCY);
	earfun_set_game_mode(false);
	earfun_get(&state);
	check(state.noise == EARFUN_NOISE_TRANSPARENCY, "the mode asked for is taken as done");

	// Which names this module is willing to talk to at all.
	// Only the EarFun Air Pro 4 is accepted; other EarFun devices are not.
	check(earfun_name_supported("EarFun Air Pro 4"), "the pair itself");
	check(earfun_name_supported("earfun air pro 4"), "all lowercase");
	check(earfun_name_supported("EarFun Air Pro 4 Left"), "renamed with suffix");
	check(!earfun_name_supported("EarFun Air Pro 3"), "different model");
	check(!earfun_name_supported("EarFun Air"), "different EarFun product");
	check(!earfun_name_supported("earfun air pro 4i"), "model 4i is not model 4");
	check(!earfun_name_supported("WH-1000XM4"), "somebody else's headphones");
	check(!earfun_name_supported(""), "no name");

	if (failures) {
		fprintf(stderr, "earfun: %d check(s) failed\n", failures);
		return 1;
	}
	printf("earfun: frame encoder and decoders ok\n");
	return 0;
}
