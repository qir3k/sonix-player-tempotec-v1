#include "cpupower.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// Sysfs cpufreq paths
#define SYSFS_CPU0_CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq"
#define SYSFS_GEN_CPUFREQ  "/sys/devices/system/cpu/cpufreq"

static char g_cpufreq_dir[128] = "";
static bool g_cpufreq_available = false;

static unsigned long g_max_freq = 1008000;
static unsigned long g_min_freq = 240000;
static unsigned long g_audio_freq = 648000;
static unsigned long g_low_bat_freq = 600000;
static char g_best_governor[32] = "ondemand";

static cpupower_profile_t g_current_profile = CPU_PROFILE_ACTIVE;
static bool g_screen_on = true;
static bool g_audio_active = false;
static int g_battery_percent = 100;
static bool g_charging = false;

static pthread_mutex_t g_power_mutex = PTHREAD_MUTEX_INITIALIZER;

// CPU load tracking cache
static int g_cached_load_percent = 0;
static uint64_t g_last_load_check_ms = 0;
static unsigned long long g_prev_active_jiffies = 0;
static unsigned long long g_prev_total_jiffies = 0;

static uint64_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static bool write_node(const char *path, const char *val) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	size_t len = strlen(val);
	bool ok = (write(fd, val, len) == (ssize_t)len);
	close(fd);
	return ok;
}

static unsigned long read_node_ulong(const char *path, unsigned long fallback) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return fallback;
	}
	unsigned long v = fallback;
	if (fscanf(f, "%lu", &v) != 1) {
		v = fallback;
	}
	fclose(f);
	return v;
}

static void read_node_string(const char *path, char *out, size_t out_size) {
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (fgets(out, out_size, f)) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	fclose(f);
}

static void select_governor(void) {
	char path[160];
	snprintf(path, sizeof(path), "%s/scaling_available_governors", g_cpufreq_dir);
	char avail[256] = "";
	read_node_string(path, avail, sizeof(avail));

	if (strstr(avail, "ondemand")) {
		snprintf(g_best_governor, sizeof(g_best_governor), "ondemand");
	} else if (strstr(avail, "conservative")) {
		snprintf(g_best_governor, sizeof(g_best_governor), "conservative");
	} else if (strstr(avail, "interactive")) {
		snprintf(g_best_governor, sizeof(g_best_governor), "interactive");
	} else if (strstr(avail, "powersave")) {
		snprintf(g_best_governor, sizeof(g_best_governor), "powersave");
	} else {
		snprintf(g_best_governor, sizeof(g_best_governor), "userspace");
	}
}

static void apply_kernel_panic_guards(void) {
#ifndef HOST_BUILD
	// Prevent sudden kernel panics on hung tasks or warnings during heavy I/O or background processing
	write_node("/proc/sys/kernel/hung_task_panic", "0");
	write_node("/proc/sys/kernel/panic_on_warn", "0");
	write_node("/proc/sys/kernel/softlockup_panic", "0");
#endif
}

static void apply_profile(cpupower_profile_t profile) {
	if (!g_cpufreq_available) {
		g_current_profile = profile;
		return;
	}

	char path[160];
	char val[32];
	unsigned long target_max_freq = g_max_freq;
	const char *gov = g_best_governor;

	switch (profile) {
	case CPU_PROFILE_ACTIVE:
		target_max_freq = g_max_freq;
		gov = g_best_governor;
		break;
	case CPU_PROFILE_AUDIO_STANDBY:
		target_max_freq = g_audio_freq;
		gov = g_best_governor;
		break;
	case CPU_PROFILE_DEEP_STANDBY:
		target_max_freq = g_min_freq > 0 ? g_min_freq : 240000;
		gov = (strcmp(g_best_governor, "conservative") == 0) ? "conservative" : g_best_governor;
		break;
	case CPU_PROFILE_LOW_BATTERY:
		target_max_freq = g_low_bat_freq;
		gov = g_best_governor;
		break;
	}

	// Set scaling governor
	snprintf(path, sizeof(path), "%s/scaling_governor", g_cpufreq_dir);
	write_node(path, gov);

	// Set scaling max frequency
	snprintf(path, sizeof(path), "%s/scaling_max_freq", g_cpufreq_dir);
	snprintf(val, sizeof(val), "%lu", target_max_freq);
	write_node(path, val);

	// If governor is ondemand, tune up_threshold to prevent premature frequency thrashing
	if (strcmp(gov, "ondemand") == 0) {
		snprintf(path, sizeof(path), "%s/ondemand/up_threshold", SYSFS_GEN_CPUFREQ);
		write_node(path, "85");
		if (profile == CPU_PROFILE_AUDIO_STANDBY) {
			snprintf(path, sizeof(path), "%s/ondemand/powersave_bias", SYSFS_GEN_CPUFREQ);
			write_node(path, "150");
		} else {
			snprintf(path, sizeof(path), "%s/ondemand/powersave_bias", SYSFS_GEN_CPUFREQ);
			write_node(path, "0");
		}
	}

	if (g_current_profile != profile) {
		fprintf(stderr, "cpupower: profile changed to %d (max_freq=%lu kHz, gov=%s)\n",
				(int)profile, target_max_freq, gov);
	}
	g_current_profile = profile;
}

static void update_profile_locked(void) {
	cpupower_profile_t target;
	if (g_battery_percent > 0 && g_battery_percent <= 15 && !g_charging) {
		// Low battery: enforce voltage-sag / overpower prevention
		target = CPU_PROFILE_LOW_BATTERY;
	} else if (g_screen_on) {
		target = CPU_PROFILE_ACTIVE;
	} else if (g_audio_active) {
		target = CPU_PROFILE_AUDIO_STANDBY;
	} else {
		target = CPU_PROFILE_DEEP_STANDBY;
	}
	apply_profile(target);
}

void cpupower_init(void) {
	apply_kernel_panic_guards();

	if (access(SYSFS_CPU0_CPUFREQ, F_OK) == 0) {
		snprintf(g_cpufreq_dir, sizeof(g_cpufreq_dir), "%s", SYSFS_CPU0_CPUFREQ);
		g_cpufreq_available = true;
	} else if (access(SYSFS_GEN_CPUFREQ, F_OK) == 0) {
		snprintf(g_cpufreq_dir, sizeof(g_cpufreq_dir), "%s", SYSFS_GEN_CPUFREQ);
		g_cpufreq_available = true;
	}

	if (g_cpufreq_available) {
		char path[160];
		snprintf(path, sizeof(path), "%s/cpuinfo_max_freq", g_cpufreq_dir);
		g_max_freq = read_node_ulong(path, 1008000);

		snprintf(path, sizeof(path), "%s/cpuinfo_min_freq", g_cpufreq_dir);
		g_min_freq = read_node_ulong(path, 240000);

		if (g_max_freq > g_min_freq) {
			// Budget audio standby at ~65% of max frequency (e.g. ~650 MHz on 1 GHz)
			g_audio_freq = g_min_freq + ((g_max_freq - g_min_freq) * 65) / 100;
			// Low battery frequency capped at ~60%
			g_low_bat_freq = g_min_freq + ((g_max_freq - g_min_freq) * 60) / 100;
		} else {
			g_audio_freq = g_max_freq;
			g_low_bat_freq = g_max_freq;
		}

		select_governor();
		fprintf(stderr, "cpupower: initialized (sysfs=%s, min=%lu, max=%lu, audio_standby=%lu, gov=%s)\n",
				g_cpufreq_dir, g_min_freq, g_max_freq, g_audio_freq, g_best_governor);
	} else {
		fprintf(stderr, "cpupower: cpufreq sysfs not available; software power throttling active\n");
	}

	pthread_mutex_lock(&g_power_mutex);
	update_profile_locked();
	pthread_mutex_unlock(&g_power_mutex);
}

void cpupower_set_profile(cpupower_profile_t profile) {
	pthread_mutex_lock(&g_power_mutex);
	apply_profile(profile);
	pthread_mutex_unlock(&g_power_mutex);
}

void cpupower_screen_state(bool screen_on, bool audio_active) {
	pthread_mutex_lock(&g_power_mutex);
	g_screen_on = screen_on;
	g_audio_active = audio_active;
	update_profile_locked();
	pthread_mutex_unlock(&g_power_mutex);
}

void cpupower_battery_state(int percent, bool charging) {
	pthread_mutex_lock(&g_power_mutex);
	g_battery_percent = percent;
	g_charging = charging;
	update_profile_locked();
	pthread_mutex_unlock(&g_power_mutex);
}

cpupower_profile_t cpupower_get_profile(void) {
	pthread_mutex_lock(&g_power_mutex);
	cpupower_profile_t p = g_current_profile;
	pthread_mutex_unlock(&g_power_mutex);
	return p;
}

int cpupower_get_load_percent(void) {
	uint64_t now = now_ms();
	pthread_mutex_lock(&g_power_mutex);

	// Throttle proc reading to once every 500 ms
	if (now - g_last_load_check_ms < 500 && g_last_load_check_ms > 0) {
		int cached = g_cached_load_percent;
		pthread_mutex_unlock(&g_power_mutex);
		return cached;
	}

	FILE *f = fopen("/proc/stat", "r");
	if (!f) {
		pthread_mutex_unlock(&g_power_mutex);
		return g_cached_load_percent;
	}

	char line[256];
	if (fgets(line, sizeof(line), f)) {
		unsigned long long user = 0, nice_val = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
		if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu",
				   &user, &nice_val, &sys, &idle, &iowait, &irq, &softirq) >= 4) {
			unsigned long long active = user + nice_val + sys + irq + softirq;
			unsigned long long total = active + idle + iowait;

			if (g_prev_total_jiffies > 0 && total > g_prev_total_jiffies) {
				unsigned long long delta_active = active - g_prev_active_jiffies;
				unsigned long long delta_total = total - g_prev_total_jiffies;
				g_cached_load_percent = (int)((delta_active * 100ULL) / delta_total);
				if (g_cached_load_percent > 100) {
					g_cached_load_percent = 100;
				}
			}
			g_prev_active_jiffies = active;
			g_prev_total_jiffies = total;
			g_last_load_check_ms = now;
		}
	}
	fclose(f);

	int load = g_cached_load_percent;
	pthread_mutex_unlock(&g_power_mutex);
	return load;
}

bool cpupower_is_stressed(void) {
	return cpupower_get_load_percent() >= 80;
}

void cpupower_yield_if_stressed(void) {
	if (cpupower_is_stressed()) {
		// Yield the single-core CPU for 12 ms to allow audio PCM writes and UI events to breathe,
		// cool down the SoC, and prevent overpowering the battery rail.
		usleep(12000);
	}
}
