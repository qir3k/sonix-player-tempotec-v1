#ifndef BATTERYGUARD_H
#define BATTERYGUARD_H

#include <stdbool.h>

// The two ends of a battery's last few percent.
//
// A player that runs until the cell is empty does not switch itself off: the
// regulator drops out from under it and the card is left with a database
// half-written and no clean shutdown to show for it. So the last few percent
// are managed here instead:
//
//   * at 5% the user is warned, once for each discharge, and the panel is
//     brought back up if it was dark -- a warning nobody sees is not one;
//   * at 1% the player goes down the ordinary way, through power_shutdown(),
//     which writes the position, closes the databases and unmounts the card.
//
// Neither happens on the charger: the cable is running the player and nothing
// is coming out of the cell.
//
// The thresholds are on the reading the status bar already polls, so this costs
// no sysfs traffic of its own and works with the screen off, in standby. The
// one state it cannot see is a device already suspended to RAM: nothing runs
// there at all. What keeps the shutdown reachable is that a player below
// BATTERYGUARD_AWAKE_PERCENT is never allowed to suspend in the first place
// (see suspend_if_idle() in power.c) -- the sleep that saves a few percent of a
// full battery would cost the clean shutdown at one.

// Warn here, shut down there. A reading at or below WARN_PERCENT that is not
// followed by a charge warns once; REARM_PERCENT is the level the battery has
// to reach again before another discharge can warn again, which is what stops
// a charger plugged in for a minute from producing a second warning.
#define BATTERYGUARD_WARN_PERCENT 5
#define BATTERYGUARD_OFF_PERCENT 1
#define BATTERYGUARD_REARM_PERCENT 7

// Below this, and off the charger, the device stays awake: see above.
#define BATTERYGUARD_AWAKE_PERCENT 10

typedef enum {
	BATTERYGUARD_NOTHING = 0,
	BATTERYGUARD_WARN,	// tell the user, once per discharge
	BATTERYGUARD_SHUTDOWN, // power_shutdown(), once
} batteryguard_action_t;

typedef struct {
	bool warned;	  // the warning has been given since the battery was last charged
	bool off_ordered; // a shutdown has been asked for; it does not come back
} batteryguard_t;

// One reading, from the battery poll. `percent` is the state of charge, or -1
// when there is no reading to judge by -- an unset gauge, a card that is being
// unmounted, the moment before the first poll. `on_charger` is the cable, not
// "a charge is going in": a battery held at its charge limit still has the
// cable running the player.
//
// Returns what the caller has to do about it. The state is updated here, so
// calling this every tick is safe and produces the warning and the shutdown
// exactly once each per discharge.
batteryguard_action_t batteryguard_step(batteryguard_t *state, int percent, bool on_charger);

// Whether the device must stay awake at this reading, so that the shutdown at
// one percent can happen at all. False while charging and when there is no
// reading: neither is a reason to keep a player out of standby.
bool batteryguard_holds_awake(int percent, bool on_charger);

#endif /* BATTERYGUARD_H */
