#ifndef CPUPOWER_H
#define CPUPOWER_H

#include <stdbool.h>
#include <stdint.h>

// CPU Power, Frequency and Overpower/Lockup Governor
//
// Ingenic X1000 is a single-core MIPS32 SoC running at up to 1.0 GHz.
// Without power budgeting, running unconstrained at peak clock under heavy
// loads (screen on, Wi-Fi downloading, Bluetooth streaming, image decoding)
// draws excessive power from the PMIC rail, causing battery voltage sag
// (brownout reset), thermal throttling, and CPU lockups/freezes when threads
// compete for the single core without yielding.
//
// This module provides:
//   1. Dynamic CPU frequency and governor scaling (interactive UI vs pocket audio vs deep standby).
//   2. Overpower / voltage-sag prevention on low battery (caps max frequency to prevent brownouts).
//   3. Anti-freeze kernel sysctl guards (disabling panic on hung tasks / soft lockups).
//   4. Cooperative CPU load budgeting for background worker threads to guarantee UI and audio headroom.

typedef enum {
	CPU_PROFILE_ACTIVE = 0,    // Screen on: full dynamic performance for smooth 60 FPS UI
	CPU_PROFILE_AUDIO_STANDBY, // Screen off with audio playing: power-budgeted frequency
	CPU_PROFILE_DEEP_STANDBY,  // Screen off, no audio: lowest frequency / powersave
	CPU_PROFILE_LOW_BATTERY    // Battery <= 15%: voltage-sag / overpower prevention
} cpupower_profile_t;

// Initialize CPU power governor, frequency limits, and kernel panic / soft-lockup guards.
void cpupower_init(void);

// Set profile explicitly or notify of hardware/system state changes.
void cpupower_set_profile(cpupower_profile_t profile);
void cpupower_screen_state(bool screen_on, bool audio_active);
void cpupower_battery_state(int percent, bool charging);

// Cooperative throttling helper for heavy background threads (coverloader, scanner, ytmusic, waveform).
// If CPU load is elevated or UI is actively interacting, yields CPU (sleeps 10-15ms)
// to prevent starving audio/UI, and avoid SoC overpower and lockups.
void cpupower_yield_if_stressed(void);

// Returns true if the system CPU is under high stress / elevated load.
bool cpupower_is_stressed(void);

// Current measured CPU load percentage (0 - 100).
int cpupower_get_load_percent(void);

// Returns the active CPU profile.
cpupower_profile_t cpupower_get_profile(void);

#endif // CPUPOWER_H
