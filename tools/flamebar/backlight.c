#include "backlight.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

// how bright the screen is, two ways:
//   a laptop's panel: its backlight in /sys/class/backlight, set through
//     logind (no root, no udev rule)
//   a monitor on a cable: asked over the cable itself (ddc/ci, vcp 0x10),
//     on the graphics card's i2c lines only (never the board's smbus, that's
//     memory and sensors). slow (tens of ms a word, a rest between), so on
//     a thread of its own: held keys go to the last level asked for

#define SYSFS "/sys/class/backlight"
#define DDC 0x37 // where a monitor answers ddc/ci
#define EDID 0x50 // where it says what it is
#define BRIGHTNESS 0x10
#define MONITORS 4
#define REST_US 50000 // a monitor's rest between two words

struct monitor {
	int fd;
	int max;
};

struct backlight {
	sd_bus *bus;
	char name[256]; // the panel's, "" for monitors
	long max;
	double level; // what it was set to last
	double set_at;

	// monitors: what the thread found, what's asked of it
	struct monitor monitors[MONITORS];
	int len;
	pthread_t thread;
	bool thread_up;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	bool found; // looked for: len says how many
	bool pending, stop;
	double want;
};

static double now_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// ---- a laptop's panel ----

static long read_number(const char *name, const char *file) {
	char path[512];
	snprintf(path, sizeof(path), SYSFS "/%s/%s", name, file);
	FILE *f = fopen(path, "r");
	long value = -1;
	if (f) {
		if (fscanf(f, "%ld", &value) != 1) {
			value = -1;
		}
		fclose(f);
	}
	return value;
}

// the panel's own (firmware) over the platform's over a gpu's raw one,
// the way systemd picks
static int rank(const char *name) {
	char path[512], type[32] = "";
	snprintf(path, sizeof(path), SYSFS "/%s/type", name);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%31s", type) != 1) {
			type[0] = '\0';
		}
		fclose(f);
	}
	return !strcmp(type, "firmware") ? 3 : !strcmp(type, "platform") ? 2 : 1;
}

static bool find_panel(struct backlight *b) {
	DIR *dir = opendir(SYSFS);
	if (!dir) {
		return false;
	}
	int best_rank = 0;
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.' || read_number(e->d_name, "max_brightness") <= 0) {
			continue;
		}
		int r = rank(e->d_name);
		if (r > best_rank) {
			best_rank = r;
			snprintf(b->name, sizeof(b->name), "%s", e->d_name);
		}
	}
	closedir(dir);
	b->max = *b->name ? read_number(b->name, "max_brightness") : 0;
	return *b->name;
}

// ---- monitors, ddc/ci ----

// a word to the monitor: what's given and its checksum (the xor of all of
// it, the monitor's address in it too)
static bool ddc_send(int fd, const uint8_t *message, size_t len) {
	uint8_t out[16];
	uint8_t sum = DDC << 1;
	for (size_t i = 0; i < len; i++) {
		out[i] = message[i];
		sum ^= message[i];
	}
	out[len] = sum;
	return write(fd, out, len + 1) == (ssize_t)(len + 1);
}

static bool ddc_get(int fd, uint8_t vcp, int *current, int *max) {
	const uint8_t ask[] = {0x51, 0x82, 0x01, vcp};
	if (!ddc_send(fd, ask, sizeof(ask))) {
		return false;
	}
	usleep(40000); // it takes its time to answer
	// its answer: who from, length, 02 for a vcp reply, 00 for fine, the
	// code, its type, the most it goes to and where it is, a checksum
	uint8_t reply[11];
	if (read(fd, reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
		return false;
	}
	uint8_t sum = 0x50;
	for (size_t i = 0; i < sizeof(reply) - 1; i++) {
		sum ^= reply[i];
	}
	if (reply[2] != 0x02 || reply[3] != 0x00 || reply[4] != vcp || sum != reply[10]) {
		return false;
	}
	*max = reply[6] << 8 | reply[7];
	*current = reply[8] << 8 | reply[9];
	return *max > 0;
}

static bool ddc_set(int fd, uint8_t vcp, int value) {
	const uint8_t set[] = {0x51, 0x84, 0x03, vcp, (uint8_t)(value >> 8), (uint8_t)value};
	return ddc_send(fd, set, sizeof(set));
}

// an i2c line of a graphics card (its parent's a display controller):
// the only ones a monitor's on
static bool display_line(const char *name) {
	char path[512], class[16] = "";
	snprintf(path, sizeof(path), "/sys/class/i2c-dev/%s/device/../class", name);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%15s", class) != 1) {
			class[0] = '\0';
		}
		fclose(f);
	}
	return !strncmp(class, "0x03", 4);
}

// a monitor on this line: it says what it is (an edid's first bytes), and
// how bright it is over ddc/ci
static bool monitor_on(const char *name, struct monitor *m, double *level) {
	char path[64];
	snprintf(path, sizeof(path), "/dev/%s", name);
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	uint8_t edid[8], at = 0;
	bool there = ioctl(fd, I2C_SLAVE, EDID) >= 0 && write(fd, &at, 1) == 1
		&& read(fd, edid, sizeof(edid)) == (ssize_t)sizeof(edid)
		&& !memcmp(edid, header, sizeof(header));
	int current, max;
	if (there && ioctl(fd, I2C_SLAVE, DDC) >= 0 && ddc_get(fd, BRIGHTNESS, &current, &max)) {
		m->fd = fd;
		m->max = max;
		*level = (double)current / max;
		return true;
	}
	close(fd);
	return false;
}

static void *monitors(void *data) {
	struct backlight *b = data;
	// what's there, looked for once
	double level = -1;
	DIR *dir = opendir("/sys/class/i2c-dev");
	struct dirent *e;
	int len = 0;
	while (dir && (e = readdir(dir)) && len < MONITORS) {
		double at;
		if (e->d_name[0] != '.' && display_line(e->d_name)
				&& monitor_on(e->d_name, &b->monitors[len], &at)) {
			level = level < 0 ? at : level;
			len++;
			usleep(REST_US);
		}
	}
	if (dir) {
		closedir(dir);
	}
	pthread_mutex_lock(&b->lock);
	b->len = len;
	b->found = true;
	if (!b->pending && level >= 0) {
		b->level = level;
	}
	// then what's asked: the last of it, a rest after each
	while (!b->stop) {
		if (!b->pending) {
			pthread_cond_wait(&b->wake, &b->lock);
			continue;
		}
		double want = b->want;
		b->pending = false;
		pthread_mutex_unlock(&b->lock);
		for (int i = 0; i < len; i++) {
			ddc_set(b->monitors[i].fd, BRIGHTNESS, (int)lround(want * b->monitors[i].max));
		}
		usleep(REST_US);
		pthread_mutex_lock(&b->lock);
	}
	pthread_mutex_unlock(&b->lock);
	return NULL;
}

// ---- either ----

struct backlight *backlight_start(sd_bus *system) {
	struct backlight *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->level = -1;
	if (find_panel(b)) {
		b->bus = system ? sd_bus_ref(system) : NULL;
		return b;
	}
	pthread_mutex_init(&b->lock, NULL);
	pthread_cond_init(&b->wake, NULL);
	// FLAMEBAR_NO_DDC=1: monitors left alone (one that misbehaves over
	// ddc/ci, the tests: never the machine's own screen)
	const char *no = getenv("FLAMEBAR_NO_DDC");
	if (no && *no && strcmp(no, "0")) {
		b->found = true;
		return b;
	}
	b->thread_up = pthread_create(&b->thread, NULL, monitors, b) == 0;
	return b;
}

void backlight_stop(struct backlight *b) {
	if (!b) {
		return;
	}
	if (b->thread_up) {
		pthread_mutex_lock(&b->lock);
		b->stop = true;
		pthread_cond_signal(&b->wake);
		pthread_mutex_unlock(&b->lock);
		pthread_join(b->thread, NULL);
		for (int i = 0; i < b->len; i++) {
			close(b->monitors[i].fd);
		}
	}
	if (!*b->name) {
		pthread_mutex_destroy(&b->lock);
		pthread_cond_destroy(&b->wake);
	}
	sd_bus_unref(b->bus);
	free(b);
}

bool backlight_there(struct backlight *b) {
	if (!b) {
		return false;
	}
	if (*b->name) {
		return true;
	}
	pthread_mutex_lock(&b->lock);
	bool there = b->found && b->len > 0;
	pthread_mutex_unlock(&b->lock);
	return there;
}

double backlight_get(struct backlight *b) {
	if (!b) {
		return 0;
	}
	if (!*b->name) {
		pthread_mutex_lock(&b->lock);
		double level = b->pending ? b->want : b->level;
		pthread_mutex_unlock(&b->lock);
		return fmax(level, 0);
	}
	// what was just set (logind's still at it), else what it is
	if (b->level >= 0 && now_seconds() - b->set_at < 2) {
		return b->level;
	}
	long value = read_number(b->name, "brightness");
	return value >= 0 ? (double)value / (double)b->max : 0;
}

void backlight_set(struct backlight *b, double level) {
	if (!b) {
		return;
	}
	level = fmax(0, fmin(level, 1));
	if (!*b->name) {
		// a monitor's lowest is still lit: all the way down is fine
		pthread_mutex_lock(&b->lock);
		b->want = b->level = level;
		b->pending = true;
		pthread_cond_signal(&b->wake);
		pthread_mutex_unlock(&b->lock);
		return;
	}
	// a panel never quite off: at 0 it's black, nothing to turn it up with
	level = fmax(0.01, level);
	uint32_t value = (uint32_t)fmax(1, lround(level * (double)b->max));
	if (b->bus) {
		sd_bus_call_method_async(b->bus, NULL, "org.freedesktop.login1",
			"/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session",
			"SetBrightness", NULL, NULL, "ssu", "backlight", b->name, value);
	}
	b->level = level;
	b->set_at = now_seconds();
}
