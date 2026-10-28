#include "batteryguard.h"

// The rule, in one place and with nothing around it: no LVGL, no sysfs, no
// clock. power.c owns the reading and the two things that happen; this owns
// when they happen, which is the part that can be got wrong quietly -- a
// warning every tick, a shutdown retried for ever, a fresh warning every time
// the charger is wiggled.

// A reading that is not a percentage. The battery poll hands out "!!" before
// its first read, and a gauge can fail to answer at all; neither is a flat
// battery, and treating it as one would switch the player off for a sysfs
// hiccup.
static bool reading_is_usable(int percent) { return percent >= 0 && percent <= 100; }

batteryguard_action_t batteryguard_step(batteryguard_t *state, int percent, bool on_charger) {
	if (!state) {
		return BATTERYGUARD_NOTHING;
	}

	// On the charger there is nothing to guard against: the cable is carrying
	// the player. The state is wound back here rather than above the warning,
	// so that a battery charged past REARM_PERCENT warns again the next time
	// it is run down -- and so that one plugged in at 2% does not leave a
	// warned flag set for the rest of the session.
	if (on_charger) {
		if (!reading_is_usable(percent)) {
			state->warned = false;
		} else if (percent >= BATTERYGUARD_REARM_PERCENT) {
			state->warned = false;
			state->off_ordered = false;
		}
		return BATTERYGUARD_NOTHING;
	}

	if (!reading_is_usable(percent)) {
		return BATTERYGUARD_NOTHING;
	}

	// The shutdown first: at one percent it outranks the warning, and a device
	// that reaches it without having warned (a reading that fell from six to
	// one between two polls) must still go down.
	//
	// Asked once. power_shutdown() does not return on the device, but it can on
	// the host build, and a tick that keeps asking would run it again from the
	// top.
	if (percent <= BATTERYGUARD_OFF_PERCENT && !state->off_ordered) {
		state->off_ordered = true;
		return BATTERYGUARD_SHUTDOWN;
	}

	if (percent <= BATTERYGUARD_WARN_PERCENT && !state->warned) {
		state->warned = true;
		return BATTERYGUARD_WARN;
	}

	return BATTERYGUARD_NOTHING;
}

bool batteryguard_holds_awake(int percent, bool on_charger) {
	if (on_charger || !reading_is_usable(percent)) {
		return false;
	}
	return percent <= BATTERYGUARD_AWAKE_PERCENT;
}
