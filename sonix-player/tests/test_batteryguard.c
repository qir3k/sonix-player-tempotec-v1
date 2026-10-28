// Headless checks of the battery guard: when the player warns, when it switches
// itself off, and what it does with a reading that is not a percentage.
//
// The rule is driven directly -- no LVGL, no sysfs, no clock -- because that is
// how it is written: power.c owns the reading and the two things that happen,
// and batteryguard.c owns when they happen. The state machine is the part that
// can be wrong quietly, so each case here is one the device would show as a
// warning that never came, a warning every tick, or a player switched off for a
// sysfs hiccup.

#include <assert.h>
#include <stdio.h>

#include "src/system/device/batteryguard.h"

static batteryguard_action_t step(batteryguard_t *state, int percent, bool on_charger) {
	return batteryguard_step(state, percent, on_charger);
}

static void check_warns_at_five_and_not_before(void) {
	batteryguard_t st = {0};

	assert(step(&st, 100, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 20, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 6, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 5, false) == BATTERYGUARD_WARN);

	// Once, however often the same reading comes back.
	assert(step(&st, 5, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 4, false) == BATTERYGUARD_NOTHING);

	// And the shutdown is still to come, at one.
	assert(step(&st, 2, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 1, false) == BATTERYGUARD_SHUTDOWN);
	assert(step(&st, 1, false) == BATTERYGUARD_NOTHING); // asked for once
	assert(step(&st, 0, false) == BATTERYGUARD_NOTHING);

	printf("ok  5%% warns once, 1%% switches off once\n");
}

static void check_shuts_down_without_warning(void) {
	// A reading that falls from six to one between two polls, or a boot on a
	// nearly flat battery: there is no time for a warning, and none is needed.
	batteryguard_t st = {0};
	assert(step(&st, 1, false) == BATTERYGUARD_SHUTDOWN);
	printf("ok  1%% switches off even when 5%% was never seen\n");
}

static void check_charger_suspends_the_guard(void) {
	batteryguard_t st = {0};

	assert(step(&st, 4, true) == BATTERYGUARD_NOTHING);
	assert(step(&st, 1, true) == BATTERYGUARD_NOTHING);
	assert(step(&st, 0, true) == BATTERYGUARD_NOTHING);
	// The cable runs the player: a battery held at the charge limit reads low
	// and is not going anywhere.
	assert(!batteryguard_holds_awake(4, true));
	assert(!batteryguard_holds_awake(1, true));
	printf("ok  nothing happens on the charger\n");
}

static void check_rearms_after_a_charge(void) {
	batteryguard_t st = {0};

	assert(step(&st, 4, false) == BATTERYGUARD_WARN);
	// Plugged in at 4%: no second warning while the charge is below the re-arm
	// level, even if it falls back off the cable.
	assert(step(&st, 6, true) == BATTERYGUARD_NOTHING);
	assert(step(&st, 5, false) == BATTERYGUARD_NOTHING);

	// Charged past 7%: the next discharge is a new one and warns again.
	assert(step(&st, 40, true) == BATTERYGUARD_NOTHING);
	assert(step(&st, 5, false) == BATTERYGUARD_WARN);

	// A charge that only reaches six does not re-arm.
	batteryguard_t st2 = {0};
	assert(step(&st2, 5, false) == BATTERYGUARD_WARN);
	assert(step(&st2, 6, true) == BATTERYGUARD_NOTHING);
	assert(step(&st2, 5, false) == BATTERYGUARD_NOTHING);
	printf("ok  a real charge re-arms the warning, a top-up does not\n");
}

static void check_unknown_reading_is_not_a_flat_battery(void) {
	batteryguard_t st = {0};

	// The status bar's cache holds "!!" until its first poll, which power.c
	// hands over as -1.
	assert(step(&st, -1, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, -1, false) == BATTERYGUARD_NOTHING);
	assert(step(&st, 101, false) == BATTERYGUARD_NOTHING);
	assert(!batteryguard_holds_awake(-1, false));
	assert(!batteryguard_holds_awake(101, false));

	// ... and a real reading after it still works.
	assert(step(&st, 5, false) == BATTERYGUARD_WARN);
	printf("ok  no reading warns nobody and switches nothing off\n");
}

static void check_holds_awake_only_when_low_and_off_the_cable(void) {
	batteryguard_t st = {0};
	(void)st;

	assert(!batteryguard_holds_awake(11, false));
	assert(batteryguard_holds_awake(10, false));
	assert(batteryguard_holds_awake(5, false));
	assert(batteryguard_holds_awake(1, false));
	assert(batteryguard_holds_awake(0, false));

	// Charging or unknown: standby is not the thing to give up.
	assert(!batteryguard_holds_awake(3, true));
	printf("ok  a low battery is kept out of standby, a charged or unknown one is not\n");
}

int main(void) {
	check_warns_at_five_and_not_before();
	check_shuts_down_without_warning();
	check_charger_suspends_the_guard();
	check_rearms_after_a_charge();
	check_unknown_reading_is_not_a_flat_battery();
	check_holds_awake_only_when_low_and_off_the_cable();

	printf("PASS battery guard (warning at five, shutdown at one, standby kept awake)\n");
	return 0;
}
