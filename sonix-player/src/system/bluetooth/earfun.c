#include "earfun.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"

// ---------------------------------------------------------------------------
// The socket
//
// RFCOMM, written out here rather than taken from bluetooth/rfcomm.h for the
// same reason airpods.c writes out l2cap.h: libbluetooth's headers are not in
// the cross toolchain's sysroot, and this is the whole of what would be used
// from them -- one address family, one protocol number and the four-field
// address structure.
// ---------------------------------------------------------------------------

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_RFCOMM 3

struct sockaddr_rc {
	sa_family_t rc_family;
	uint8_t rc_bdaddr[6];
	uint8_t rc_channel;
};

#define MAC_LEN 32
#define NAME_LEN 128

// ---------------------------------------------------------------------------
// The frames
//
// Every request is eight bytes of header and then the payload:
//
//   FF  SOP
//   04  version (03 is accepted by the same readers; this writes 04)
//   00  flags: no checksum, which is the only value these earbuds send
//   nn  payload length, and the frame is nn + 8 bytes long
//   00 0A  vendor id, GAIA
//   cc cc  command id, big endian
//
// A reply carries the same command id with bit 15 set. There is no sequence
// number and no acknowledgement, so a caller that waits for an answer waits for
// that one id and takes the first one that arrives.
// ---------------------------------------------------------------------------

#define FRAME_SOP 0xFF
#define FRAME_VERSION 0x04
#define FRAME_FLAGS 0x00
#define FRAME_VENDOR 0x000A
#define FRAME_HEADER 8
#define FRAME_MAX 64

// Bit 15 of the command id: this frame is the answer to one.
#define FRAME_REPLY 0x8000

#define READ_BUF 512
#define POLL_MS 250
#define BATTERY_EVERY_MS 15000 // three requests, so not on every tick
#define SETTINGS_EVERY_MS 20000
#define RETRY_MS 6000	   // between attempts to open a session
#define GIVE_UP_AFTER 3	   // consecutive failures for one address
#define CHANNEL_MAX 30	   // the RFCOMM channel range SDP may name
#define CONNECT_TIMEOUT_MS 400
#define PROBE_TIMEOUT_MS 900

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool stopping;

static char want_mac[MAC_LEN]; // the device to talk to, "" for none
static char want_name[NAME_LEN];
static int failures; // consecutive connect failures for the address above
static char failed_mac[MAC_LEN];
static earfun_state_t g_state;

// A command asked for on the interface thread, waiting for the session thread
// to write it. Small and bounded: four commands is more than any one tap can
// produce, and a queue that overflowed would mean the interface is stuck.
#define PENDING_MAX 8
static uint8_t pending_frames[PENDING_MAX][FRAME_MAX];
static size_t pending_lengths[PENDING_MAX];
static int pending_head, pending_count;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

// "AA:BB:CC:DD:EE:FF" into the six bytes the kernel wants, least significant
// first -- the reverse of how it is written.
static bool parse_mac(const char *text, uint8_t out[6]) {
	int values[6];
	if (!text ||
		sscanf(text, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4],
			   &values[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		if (values[i] < 0 || values[i] > 255) {
			return false;
		}
		out[5 - i] = (uint8_t)values[i];
	}
	return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

size_t earfun_encode_request(uint16_t command, const uint8_t *payload, size_t length, uint8_t *out,
							 size_t out_size) {
	if (!out || length > FRAME_MAX - FRAME_HEADER || out_size < length + FRAME_HEADER ||
		length > 0xFF) {
		return 0;
	}
	out[0] = FRAME_SOP;
	out[1] = FRAME_VERSION;
	out[2] = FRAME_FLAGS;
	out[3] = (uint8_t)length;
	out[4] = (uint8_t)(FRAME_VENDOR >> 8);
	out[5] = (uint8_t)(FRAME_VENDOR & 0xFF);
	out[6] = (uint8_t)(command >> 8);
	out[7] = (uint8_t)(command & 0xFF);
	if (length) {
		memcpy(out + FRAME_HEADER, payload, length);
	}
	return length + FRAME_HEADER;
}

// ---------------------------------------------------------------------------
// Parsing
//
// Frames are parsed under the lock, because a caller reading the state and a
// socket read land on different threads.
// ---------------------------------------------------------------------------

static void set_battery(int *slot, const uint8_t *payload, size_t length) {
	// The byte after the status byte, and zero means "I do not know".
	if (length < 2 || payload[1] == 0) {
		return;
	}
	*slot = payload[1] > 100 ? 100 : payload[1];
	g_state.have_battery = true;
	g_state.serial++;
}

// The version reply is text -- hardware and firmware, often separated by an
// underscore -- from the byte after the status. It is kept as it came: the page
// shows what the earbuds said rather than a string this module made up.
static void set_version(const uint8_t *payload, size_t length) {
	if (length < 2) {
		return;
	}
	size_t n = 0;
	for (size_t i = 1; i < length && n + 1 < sizeof(g_state.version); i++) {
		if (payload[i] >= 0x20 && payload[i] < 0x7F) {
			g_state.version[n++] = (char)payload[i];
		}
	}
	g_state.version[n] = '\0';
	g_state.serial++;
}

static void handle_frame(const uint8_t *frame, size_t length) {
	if (length < FRAME_HEADER) {
		return;
	}
	uint16_t command = (uint16_t)((frame[6] << 8) | frame[7]);
	if (!(command & FRAME_REPLY)) {
		return; // something the player itself wrote, echoed back
	}
	command &= (uint16_t)~FRAME_REPLY;
	const uint8_t *payload = frame + FRAME_HEADER;
	size_t payload_length = frame[3];

	pthread_mutex_lock(&lock);
	switch (command) {
	case EARFUN_CMD_BATTERY_LEFT:
		set_battery(&g_state.left, payload, payload_length);
		break;
	case EARFUN_CMD_BATTERY_RIGHT:
		set_battery(&g_state.right, payload, payload_length);
		break;
	case EARFUN_CMD_BATTERY_CASE:
		set_battery(&g_state.case_level, payload, payload_length);
		break;
	case EARFUN_CMD_VERSION:
		set_version(payload, payload_length);
		break;
	case EARFUN_CMD_NOISE_GET:
	case EARFUN_CMD_NOISE_SET:
		if (payload_length >= 2 && payload[1] <= 2) {
			g_state.noise = (earfun_noise_t)(payload[1] + 1); // 0 off, 1 ANC, 2 transparency
			g_state.serial++;
		}
		break;
	case EARFUN_CMD_ANC_GET:
	case EARFUN_CMD_ANC_SET:
		if (payload_length >= 2) {
			g_state.anc_level = payload[1];
			g_state.serial++;
		}
		break;
	case EARFUN_CMD_TRANSPARENCY_GET:
	case EARFUN_CMD_TRANSPARENCY_SET:
		if (payload_length >= 2) {
			g_state.transparency_mode = payload[1];
			g_state.serial++;
		}
		break;
	case EARFUN_CMD_GAME_GET:
	case EARFUN_CMD_GAME_SET:
		if (payload_length >= 2) {
			g_state.game_mode = payload[1] == 0x01;
			g_state.game_mode_known = true;
			g_state.serial++;
		}
		break;
	case EARFUN_CMD_IN_EAR_GET:
	case EARFUN_CMD_IN_EAR_SET:
		// Zero is enabled, one is disabled: the earbuds say it backwards to
		// how the switch reads, and this is the one place that is undone.
		if (payload_length >= 2) {
			g_state.in_ear = payload[1] == 0x00;
			g_state.in_ear_known = true;
			g_state.serial++;
		}
		break;
	default:
		// Touch gestures, multi-device and the rest. They answer whether or
		// not anyone asked, and stepping over them is the whole of what this
		// needs to do with them.
		break;
	}
	pthread_mutex_unlock(&lock);
}

void earfun_feed_frame(const uint8_t *data, unsigned length) {
	if (!data || length < FRAME_HEADER) {
		return;
	}
	handle_frame(data, length);
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t *data, size_t length) {
	while (length) {
		ssize_t n = write(fd, data, length);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		data += n;
		length -= (size_t)n;
	}
	return true;
}

static bool send_command(int fd, uint16_t command, const uint8_t *payload, size_t length) {
	uint8_t frame[FRAME_MAX];
	size_t frame_length = earfun_encode_request(command, payload, length, frame, sizeof(frame));
	return frame_length > 0 && send_all(fd, frame, frame_length);
}

// One channel, or -1. A closed channel refuses the connection immediately on
// BlueZ, which is what makes walking the range cheap enough to be worth doing
// instead of carrying an SDP client for one number.
static int rfcomm_open(const char *mac, int channel) {
	struct sockaddr_rc addr;
	memset(&addr, 0, sizeof(addr));
	addr.rc_family = AF_BLUETOOTH;
	addr.rc_channel = (uint8_t)channel;
	if (!parse_mac(mac, addr.rc_bdaddr)) {
		return -1;
	}

	int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (fd < 0) {
		return -1;
	}
	// Non-blocking for the connect itself: a channel that is open but silent
	// must not take the worker down with it for the socket's own timeout.
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (result == 0) {
		// Back to blocking for the session: a write that came back EAGAIN
		// would read as a dead connection.
		if (flags >= 0) {
			fcntl(fd, F_SETFL, flags);
		}
		return fd;
	}
	if (errno != EINPROGRESS && errno != EAGAIN) {
		close(fd);
		return -1;
	}
	struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
	if (poll(&pfd, 1, CONNECT_TIMEOUT_MS) != 1) {
		close(fd);
		return -1;
	}
	int error = 0;
	socklen_t error_size = sizeof(error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) {
		close(fd);
		return -1;
	}
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags);
	}
	return fd;
}

// True when the other end answered like an EarFun pair: any well-formed frame
// will do, and the version request is the one every implementation opens with.
static bool probe(int fd) {
	if (!send_command(fd, EARFUN_CMD_VERSION, NULL, 0)) {
		return false;
	}
	uint8_t buf[READ_BUF];
	size_t filled = 0;
	uint32_t deadline = now_ms() + PROBE_TIMEOUT_MS;
	while ((int32_t)(deadline - now_ms()) > 0) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&pfd, 1, 100);
		if (ready <= 0) {
			continue;
		}
		ssize_t n = read(fd, buf + filled, sizeof(buf) - filled);
		if (n <= 0) {
			return false;
		}
		filled += (size_t)n;
		for (size_t i = 0; i + FRAME_HEADER <= filled; i++) {
			if (buf[i] == FRAME_SOP && buf[i + 1] == FRAME_VERSION && buf[i + 2] == FRAME_FLAGS &&
				(size_t)buf[i + 3] + FRAME_HEADER <= filled - i) {
				return true;
			}
		}
	}
	return false;
}

// The channel the session runs on: the one this player found last time when it
// still answers, otherwise the first of the range that does.
static int find_channel(const char *mac) {
	long remembered = config_get_int("wireless", "earfun_channel", 0);
	if (remembered >= 1 && remembered <= CHANNEL_MAX) {
		int fd = rfcomm_open(mac, (int)remembered);
		if (fd >= 0) {
			if (probe(fd)) {
				return fd;
			}
			close(fd);
		}
	}
	for (int channel = 1; channel <= CHANNEL_MAX; channel++) {
		if (channel == (int)remembered) {
			continue; // just tried
		}
		int fd = rfcomm_open(mac, channel);
		if (fd < 0) {
			continue;
		}
		if (probe(fd)) {
			config_set_int("wireless", "earfun_channel", channel);
			config_save();
			printf("earfun: control channel %d\n", channel);
			return fd;
		}
		close(fd);
	}
	return -1;
}

static bool should_stop(const char *mac) {
	pthread_mutex_lock(&lock);
	bool stop = stopping || strcmp(want_mac, mac) != 0;
	pthread_mutex_unlock(&lock);
	return stop;
}

// The settings the page shows, asked for in one go. The order is the one the
// Gadgetbridge implementation polls in, minus the touch gestures and the
// multi-device switch this player has no page for yet.
static void poll_settings(int fd) {
	send_command(fd, EARFUN_CMD_BATTERY_LEFT, NULL, 0);
	send_command(fd, EARFUN_CMD_BATTERY_RIGHT, NULL, 0);
	send_command(fd, EARFUN_CMD_BATTERY_CASE, NULL, 0);
	send_command(fd, EARFUN_CMD_NOISE_GET, NULL, 0);
	send_command(fd, EARFUN_CMD_ANC_GET, NULL, 0);
	send_command(fd, EARFUN_CMD_TRANSPARENCY_GET, NULL, 0);
	send_command(fd, EARFUN_CMD_GAME_GET, NULL, 0);
	send_command(fd, EARFUN_CMD_IN_EAR_GET, NULL, 0);
}

// Runs the session until it ends. The socket is read through poll() rather than
// blocked on, so a disconnection or a shutdown is noticed within a tick.
static void run_session(int fd, const char *mac) {
	uint8_t rx[READ_BUF * 2];
	size_t filled = 0;

	pthread_mutex_lock(&lock);
	g_state.connected = true;
	// What Bluetooth calls the pair, so the row that opens this page can name
	// it rather than the module's own name for it.
	snprintf(g_state.name, sizeof(g_state.name), "%.31s", want_name);
	g_state.serial++;
	pthread_mutex_unlock(&lock);
	printf("earfun: session open with %s\n", mac);

	uint32_t battery_at = 0;
	uint32_t settings_at = 0;
	while (!should_stop(mac)) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&pfd, 1, POLL_MS);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (ready > 0) {
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
				break;
			}
			ssize_t n = read(fd, rx + filled, sizeof(rx) - filled);
			if (n == 0) {
				break; // the other end closed
			}
			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN) {
					continue;
				}
				break;
			}
			filled += (size_t)n;

			// Frames have their length in byte three, so one pass is enough:
			// anything incomplete is left for the next read, and anything in
			// front of a start byte is dropped.
			size_t start = 0;
			while (start + FRAME_HEADER <= filled) {
				if (rx[start] != FRAME_SOP) {
					start++;
					continue;
				}
				size_t frame_length = (size_t)rx[start + 3] + FRAME_HEADER;
				if (start + frame_length > filled) {
					break;
				}
				handle_frame(rx + start, frame_length);
				start += frame_length;
			}
			if (start > 0) {
				memmove(rx, rx + start, filled - start);
				filled -= start;
			}
			if (filled == sizeof(rx)) {
				filled = 0; // nothing in it was a frame: start again
			}
		}

		// A command asked for on the interface thread goes out here, on the
		// thread that owns the socket.
		for (;;) {
			uint8_t frame[FRAME_MAX];
			size_t frame_length = 0;
			pthread_mutex_lock(&lock);
			if (pending_count > 0 && pending_head < PENDING_MAX) {
				frame_length = pending_lengths[pending_head];
				memcpy(frame, pending_frames[pending_head], frame_length);
				pending_head = (pending_head + 1) % PENDING_MAX;
				pending_count--;
			}
			pthread_mutex_unlock(&lock);
			if (frame_length == 0) {
				break;
			}
			if (!send_all(fd, frame, frame_length)) {
				goto closed;
			}
		}

		uint32_t now = now_ms();
		if (battery_at == 0 || (int32_t)(now - battery_at) >= BATTERY_EVERY_MS) {
			battery_at = now;
			if (!send_command(fd, EARFUN_CMD_BATTERY_LEFT, NULL, 0) ||
				!send_command(fd, EARFUN_CMD_BATTERY_RIGHT, NULL, 0) ||
				!send_command(fd, EARFUN_CMD_BATTERY_CASE, NULL, 0)) {
				break;
			}
		}
		if (settings_at == 0 || (int32_t)(now - settings_at) >= SETTINGS_EVERY_MS) {
			settings_at = now;
			poll_settings(fd);
		}
	}
closed:
	close(fd);
	printf("earfun: session closed\n");
}

static void clear_state(void) {
	pthread_mutex_lock(&lock);
	uint32_t serial = g_state.serial + 1;
	memset(&g_state, 0, sizeof(g_state));
	g_state.serial = serial;
	g_state.left = -1;
	g_state.right = -1;
	g_state.case_level = -1;
	g_state.anc_level = -1;
	g_state.transparency_mode = -1;
	// Anything asked for goes with the session it was asked of: a command left
	// queued would go out to whichever pair connects next.
	pending_head = 0;
	pending_count = 0;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static void *worker_main(void *arg) {
	(void)arg;
	char mac[MAC_LEN];

	for (;;) {
		pthread_mutex_lock(&lock);
		if (stopping) {
			pthread_mutex_unlock(&lock);
			break;
		}
		if (!want_mac[0]) {
			pthread_cond_wait(&wake, &lock);
			pthread_mutex_unlock(&lock);
			continue;
		}
		snprintf(mac, sizeof(mac), "%s", want_mac);
		// A name that is not an EarFun one is not a device to open a socket
		// to: a battery reading cannot come from a speaker or a car kit, and
		// asking would be a socket every few seconds for nothing.
		bool wanted = earfun_name_supported(want_name);
		bool exhausted = strcmp(mac, failed_mac) == 0 && failures >= GIVE_UP_AFTER;
		pthread_mutex_unlock(&lock);

		if (!wanted || exhausted) {
			pthread_mutex_lock(&lock);
			if (!stopping && strcmp(want_mac, mac) == 0) {
				pthread_cond_wait(&wake, &lock);
			}
			pthread_mutex_unlock(&lock);
			continue;
		}

		int fd = find_channel(mac);
		if (fd < 0) {
			pthread_mutex_lock(&lock);
			if (strcmp(mac, failed_mac) != 0) {
				snprintf(failed_mac, sizeof(failed_mac), "%s", mac);
				failures = 0;
			}
			failures++;
			bool announce = failures == GIVE_UP_AFTER;
			pthread_mutex_unlock(&lock);
			if (announce) {
				printf("earfun: %s does not answer on the serial port profile; not asking again\n", mac);
			}
			sleep_ms(RETRY_MS);
			continue;
		}

		pthread_mutex_lock(&lock);
		failures = 0;
		pthread_mutex_unlock(&lock);

		// What the page needs first: the version names the earbuds, and the
		// rest of the settings follow it.
		send_command(fd, EARFUN_CMD_VERSION, NULL, 0);
		poll_settings(fd);
		run_session(fd, mac);
		clear_state();
		sleep_ms(2000);
	}

	return NULL;
}

void earfun_init(void) {
	if (worker_running) {
		return;
	}
	clear_state();
	if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		fprintf(stderr, "earfun: could not start the worker thread\n");
		return;
	}
	pthread_detach(worker);
	worker_running = true;
}

void earfun_stop(void) {
	pthread_mutex_lock(&lock);
	stopping = true;
	want_mac[0] = '\0';
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
}

void earfun_set_device(const char *mac, const char *name) {
	pthread_mutex_lock(&lock);
	const char *new_mac = mac ? mac : "";
	bool changed = strcmp(want_mac, new_mac) != 0;
	snprintf(want_mac, sizeof(want_mac), "%s", new_mac);
	snprintf(want_name, sizeof(want_name), "%s", name ? name : "");
	if (changed) {
		failures = 0;
		failed_mac[0] = '\0';
		// The address changing is a new session or none at all: what the last
		// pair said is not this pair's reading.
		if (!new_mac[0]) {
			uint32_t serial = g_state.serial + 1;
			memset(&g_state, 0, sizeof(g_state));
			g_state.serial = serial;
			g_state.left = -1;
			g_state.right = -1;
			g_state.case_level = -1;
			g_state.anc_level = -1;
			g_state.transparency_mode = -1;
			pending_head = 0;
			pending_count = 0;
		}
	}
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
}

bool earfun_get(earfun_state_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&lock);
	*out = g_state;
	pthread_mutex_unlock(&lock);
	return out->connected;
}

bool earfun_name_supported(const char *name) {
	// Case-insensitively, because the earbuds' own name is "EarFun Air Pro 4"
	// and a renamed pair keeps the prefix its owner typed over.
	if (!name) {
		return false;
	}
	static const char PREFIX[] = "earfun";
	for (size_t i = 0; i < sizeof(PREFIX) - 1; i++) {
		if (!name[i] || tolower((unsigned char)name[i]) != PREFIX[i]) {
			return false;
		}
	}
	return true;
}

// Queues one command for the session thread. Nothing is queued with no session:
// the frame would be written to whichever pair connects next.
static void queue(uint16_t command, const uint8_t *payload, size_t length) {
	uint8_t frame[FRAME_MAX];
	size_t frame_length = earfun_encode_request(command, payload, length, frame, sizeof(frame));
	if (frame_length == 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (!g_state.connected || pending_count >= PENDING_MAX) {
		pthread_mutex_unlock(&lock);
		return;
	}
	int slot = (pending_head + pending_count) % PENDING_MAX;
	memcpy(pending_frames[slot], frame, frame_length);
	pending_lengths[slot] = frame_length;
	pending_count++;
	pthread_mutex_unlock(&lock);
}

void earfun_set_noise(earfun_noise_t mode) {
	if (mode == EARFUN_NOISE_UNKNOWN) {
		return;
	}
	uint8_t payload = (uint8_t)(mode - 1); // 0 off, 1 cancellation, 2 transparency
	queue(EARFUN_CMD_NOISE_SET, &payload, 1);
	pthread_mutex_lock(&lock);
	if (g_state.noise != mode) {
		// Taken as done straight away, so the pill lights under the finger
		// instead of waiting for an answer. What the earbuds report next wins
		// over it: this is what was asked for, and the frame is what happened.
		g_state.noise = mode;
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);
}

void earfun_set_anc_level(int level) {
	if (level < 0 || level > 0xFF) {
		return;
	}
	uint8_t payload = (uint8_t)level;
	queue(EARFUN_CMD_ANC_SET, &payload, 1);
}

void earfun_set_transparency_mode(int mode) {
	if (mode < 0 || mode > 0xFF) {
		return;
	}
	uint8_t payload = (uint8_t)mode;
	queue(EARFUN_CMD_TRANSPARENCY_SET, &payload, 1);
}

void earfun_set_game_mode(bool on) {
	uint8_t payload = on ? 0x01 : 0x00;
	queue(EARFUN_CMD_GAME_SET, &payload, 1);
	pthread_mutex_lock(&lock);
	if (g_state.game_mode != on) {
		g_state.game_mode = on;
		g_state.game_mode_known = true;
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);
}

void earfun_set_in_ear(bool on) {
	// Zero enabled them, one disabled them: the write goes back the same way
	// the read comes in.
	uint8_t payload = on ? 0x00 : 0x01;
	queue(EARFUN_CMD_IN_EAR_SET, &payload, 1);
	pthread_mutex_lock(&lock);
	if (g_state.in_ear != on) {
		g_state.in_ear = on;
		g_state.in_ear_known = true;
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);
}
