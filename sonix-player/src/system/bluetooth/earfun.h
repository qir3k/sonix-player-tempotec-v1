#ifndef EARFUN_H
#define EARFUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What a pair of EarFun earbuds says about itself.
//
// EarFun's Qualcomm-based earbuds -- the Air Pro 4 among them, on the QCC3091
// -- answer on the standard Bluetooth serial port profile (SPP, UUID 0x1101)
// with the packet shape Qualcomm's GAIA protocol uses:
//
//   FF | 04 | 00 | len | vendor (2) | command (2) | payload (len)
//
// There is no checksum, no sequence number and no application handshake: the
// bond the player's own pairing screen created is the whole of the gate. A
// reply is the same command with bit 15 set (0x0306 asked, 0x8306 answered),
// and a reply's first payload byte is a status the protocol's own readers step
// over -- the value is the byte after it.
//
// This follows the Gadgetbridge implementation of the Air Pro 4
// (EarFunAirPro4Protocol and EarFunPacket in gadgetbridge), which is where the
// command numbers below come from. The earbuds are found by name: only a device whose Bluetooth name begins
// with "EarFun Air Pro 4" is ever offered this protocol, so the player never
// opens a socket to other EarFun products or unrelated headphones.
//
// Threading: one worker owns the socket, as in airpods.h. Everything a caller
// sees is a copy taken under a lock.

typedef enum {
	EARFUN_NOISE_UNKNOWN = 0,
	EARFUN_NOISE_OFF,
	EARFUN_NOISE_CANCELLATION,
	EARFUN_NOISE_TRANSPARENCY,
} earfun_noise_t;

typedef struct {
	bool connected; // the protocol session is up
	bool have_battery;
	int left;  // percent, -1 when the earbud has not said
	int right;
	int case_level; // the charging case, -1 when it has not said

	earfun_noise_t noise;	// which mode is on now
	int anc_level;			// 0 strong, 1 balanced, 2 AI, -1 not said
	int transparency_mode;	// 0 default, 1 natural, -1 not said
	bool game_mode_known;
	bool game_mode;			// low latency, "Game Mode"
	bool in_ear_known;
	bool in_ear; // in-ear detection

	char version[48]; // what the earbuds answer for firmware/hardware, verbatim
	char name[32];	  // what they call themselves, for the page's heading
	uint32_t serial;  // bumps on every change, so a poll can skip a redraw
} earfun_state_t;

// Whether this device's Bluetooth name identifies it as an EarFun Air Pro 4.
// The match is case-insensitive and anchored at the start of the name, so a
// user-renamed unit is still accepted. Only the Air Pro 4 is matched: other
// EarFun products use a different protocol and must not be probed by this module.
bool earfun_name_supported(const char *name);

// Starts the worker. It sits idle until there is a device to talk to.
void earfun_init(void);

// Stops the worker and closes the session.
void earfun_stop(void);

// The connected audio device, or NULL when there is none. Same contract as
// airpods_set_device(): the address changing is how a connect, a disconnect and
// a switch between two devices all reach this module.
void earfun_set_device(const char *mac, const char *name);

// A copy of the state, and whether the session is up. The state is cleared when
// a session ends, so a caller that only draws while it returns true never shows
// the numbers of earbuds that have gone.
bool earfun_get(earfun_state_t *out);

// The setters queue a command and return at once; it goes out on the session's
// own thread, and what the earbuds answer is what the state above then holds.
// Nothing happens with no session, and a mode the earbuds never named is not
// asked for.
void earfun_set_noise(earfun_noise_t mode);
void earfun_set_anc_level(int level);
void earfun_set_transparency_mode(int mode);
void earfun_set_game_mode(bool on);
void earfun_set_in_ear(bool on);

// Hands the parsers one frame, as if it had arrived on the socket. The seam for
// driving the protocol without earbuds: on a desktop the replies have to come
// from somewhere else.
void earfun_feed_frame(const uint8_t *data, unsigned length);

// Encodes one request, for the same purpose: the bytes the worker would write.
// Returns the frame length, 0 when `out` is too small.
size_t earfun_encode_request(uint16_t command, const uint8_t *payload, size_t length, uint8_t *out,
							 size_t out_size);

// The command numbers, exposed so a test can name what it feeds back.
#define EARFUN_CMD_BATTERY_LEFT 0x0306
#define EARFUN_CMD_BATTERY_RIGHT 0x0307
#define EARFUN_CMD_BATTERY_CASE 0x0317
#define EARFUN_CMD_VERSION 0x0309
#define EARFUN_CMD_NOISE_GET 0x0315
#define EARFUN_CMD_NOISE_SET 0x0314
#define EARFUN_CMD_ANC_GET 0x033B
#define EARFUN_CMD_ANC_SET 0x033A
#define EARFUN_CMD_TRANSPARENCY_GET 0x033D
#define EARFUN_CMD_TRANSPARENCY_SET 0x033C
#define EARFUN_CMD_GAME_GET 0x0313
#define EARFUN_CMD_GAME_SET 0x0312
#define EARFUN_CMD_IN_EAR_GET 0x034A
#define EARFUN_CMD_IN_EAR_SET 0x0349

#endif /* EARFUN_H */
