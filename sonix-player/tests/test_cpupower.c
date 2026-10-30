#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/system/device/cpupower.h"

static void test_profile_transitions(void) {
	cpupower_init();

	// Screen on, no audio, full battery -> active performance
	cpupower_battery_state(100, false);
	cpupower_screen_state(true, false);
	assert(cpupower_get_profile() == CPU_PROFILE_ACTIVE);

	// Screen off with audio -> audio standby (budgeted frequency)
	cpupower_screen_state(false, true);
	assert(cpupower_get_profile() == CPU_PROFILE_AUDIO_STANDBY);

	// Screen off with no audio -> deep standby
	cpupower_screen_state(false, false);
	assert(cpupower_get_profile() == CPU_PROFILE_DEEP_STANDBY);

	// Wake up -> active performance
	cpupower_screen_state(true, false);
	assert(cpupower_get_profile() == CPU_PROFILE_ACTIVE);

	printf("ok  profile transitions (active / audio standby / deep standby)\n");
}

static void test_low_battery_overpower_protection(void) {
	cpupower_init();

	// Normal battery on screen -> active
	cpupower_battery_state(50, false);
	cpupower_screen_state(true, false);
	assert(cpupower_get_profile() == CPU_PROFILE_ACTIVE);

	// Battery sags <= 15% -> low battery profile enforced to prevent voltage drop brownout
	cpupower_battery_state(15, false);
	assert(cpupower_get_profile() == CPU_PROFILE_LOW_BATTERY);

	cpupower_battery_state(8, false);
	assert(cpupower_get_profile() == CPU_PROFILE_LOW_BATTERY);

	// When plugged into charger, cable carries load, overpower protection can relax
	cpupower_battery_state(8, true);
	assert(cpupower_get_profile() == CPU_PROFILE_ACTIVE);

	// Unplugged at low battery -> re-arms overpower protection
	cpupower_battery_state(8, false);
	assert(cpupower_get_profile() == CPU_PROFILE_LOW_BATTERY);

	// Charged back above 15% -> normal profile restored
	cpupower_battery_state(25, false);
	assert(cpupower_get_profile() == CPU_PROFILE_ACTIVE);

	printf("ok  low battery overpower & voltage-sag protection\n");
}

static void test_load_and_yielding(void) {
	cpupower_init();

	// Verify load query returns valid range (0 - 100%)
	int load = cpupower_get_load_percent();
	assert(load >= 0 && load <= 100);

	// Cooperative yielding should not crash or stall
	cpupower_yield_if_stressed();

	printf("ok  load monitoring and cooperative yielding\n");
}

int main(void) {
	test_profile_transitions();
	test_low_battery_overpower_protection();
	test_load_and_yielding();

	printf("All cpupower tests passed!\n");
	return 0;
}
