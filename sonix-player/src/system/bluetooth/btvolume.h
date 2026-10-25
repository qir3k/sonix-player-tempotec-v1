#ifndef BTVOLUME_H
#define BTVOLUME_H

#include <stdbool.h>

// The bridge between the player's volume and the volume of a pair of Bluetooth
// headphones, over bluealsa's D-Bus interface.
//
// What is on the other side. bluealsa exports one object per stream:
//
//     /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink
//
// with two writable properties on org.bluealsa.PCM1 that decide everything
// here. Volume is a 16-bit word -- the left channel in the upper byte, the
// right in the lower, the top bit of each a mute switch and the remaining seven
// a level from 0 to 127. SoftVolume says who applies it:
//
//   SoftVolume = false  bluealsa passes the level on as an AVRCP absolute
//                       volume, so it lands on the headphones themselves and
//                       whatever they do to their own volume comes back;
//   SoftVolume = true   bluealsa attenuates the stream itself and the
//                       headphones never hear about it, so the two levels are
//                       independent gain stages.
//
// Those are exactly the two halves of the "synchronised volume" setting, which
// is why this module has no policy of its own beyond them: synchronised means
// native volume and following what arrives back, separate means software
// attenuation and ignoring it.
//
// Why D-Bus and not bluealsa-cli. Following the headphones through the command
// line means asking every second and a half for as long as the music plays,
// which is a fork and an exec each time on a device with one slow core. The bus
// says so itself, through PropertiesChanged, the moment it happens. The command
// bluetooth.c falls back to a PCM-property write over btstack's separate
// D-Bus connection if this bridge is unavailable.
//
// Threading: one worker owns every call, so no other thread ever blocks on the
// bus. Signals arrive on the dbuslite reader thread and are only recorded.

// Starts the worker. It connects only while there is a stream to talk about,
// and retries quietly.
void btvolume_init(void);

// Stops the worker and closes the connection.
void btvolume_stop(void);

// The stream to bridge, as the address of the connected device (":" or "-"
// separated, either way), or NULL when there is none. Cheap to call on every
// round of a poll: the same address twice does nothing.
void btvolume_set_device(const char *mac);

// The same bridge, for the other direction: a phone or computer sending to this
// player's A2DP sink (receiver mode). `mac` is the sender, NULL or empty while
// the mode is not running.
//
// The phone's volume rocker arrives as an AVRCP absolute volume on the sink
// PCM, and the player follows it -- the DAC's own level moves, so the keys work
// without disabling "Bluetooth volume sync" on the phone. The player's volume
// keys write the same property, which bluealsa passes back to the phone, so its
// slider follows this device's level. Until this bridge existed the receiving
// PCM's level was a gain stage nobody applied locally, which is why the phone's
// keys changed nothing audible and why the phone had to be told to stop using
// absolute volume.
void btvolume_set_receiver(const char *mac);

// Whether the volume is synchronised with the headphones' own.
void btvolume_set_sync(bool on);

// The player's volume has moved and the stream has to follow. Returns at once;
// the write happens on the worker.
void btvolume_notify_local(int percent);

// Whether the bus path is up and can carry a volume. False means the caller
// should use the independent PCM-property write path.
bool btvolume_available(void);

#endif /* BTVOLUME_H */
