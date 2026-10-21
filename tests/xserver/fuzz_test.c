// throws garbage at flame's X server: random requests to every opcode and
// extension minor, bad lengths, real ids mixed with made up ones. the server
// must answer or drop the client, never crash (the harness runs it under
// asan and ubsan). FUZZ_ROUNDS sets how many requests, FUZZ_SEED the seed.
// FUZZ_CORPUS=DIR replays what real apps sent instead (xrecord made it),
// their ids moved into this connection's range, some requests mangled
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static uint64_t state;

static uint32_t rnd(void) {
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return (uint32_t)state;
}

static int connect_x(const char *display) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/.X11-unix/X%s", display + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		return -1;
	}
	return fd;
}

static uint32_t base, mask;
static uint32_t ids[64];
static int ids_len;

// connection setup, little endian, no auth. false: refused
static bool setup(int fd) {
	uint8_t hello[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	if (write(fd, hello, sizeof(hello)) != sizeof(hello)) {
		return false;
	}
	uint8_t head[8];
	if (read(fd, head, 8) != 8 || head[0] != 1) {
		return false;
	}
	uint16_t extra;
	memcpy(&extra, head + 6, 2);
	uint8_t *rest = malloc((size_t)extra * 4);
	size_t got = 0;
	while (got < (size_t)extra * 4) {
		ssize_t n = read(fd, rest + got, (size_t)extra * 4 - got);
		if (n <= 0) {
			free(rest);
			return false;
		}
		got += (size_t)n;
	}
	memcpy(&base, rest + 4, 4);
	memcpy(&mask, rest + 8, 4);
	free(rest);
	return true;
}

// what the server says back doesn't matter, it just mustn't pile up
static bool drain(int fd) {
	uint8_t buf[65536];
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, 0) <= 0) {
			return true;
		}
		if (p.revents & (POLLHUP | POLLERR)) {
			return false;
		}
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			return n < 0 && errno == EAGAIN;
		}
	}
}

static uint32_t some_id(void) {
	switch (rnd() % 6) {
	case 0:
		return rnd();
	case 1:
		return 0x100 + rnd() % 4; // root, colormap, flame's window
	case 2:
		return 0;
	default:
		return ids_len ? ids[rnd() % ids_len] : base;
	}
}

static uint32_t new_id(void) {
	uint32_t id = base | (rnd() & mask & 0xfff);
	if (ids_len < 64) {
		ids[ids_len++] = id;
	} else {
		ids[rnd() % 64] = id;
	}
	return id;
}

// mostly real-looking: ids where ids go, small numbers, sometimes junk
static void fill(uint8_t *req, size_t len) {
	for (size_t i = 4; i + 4 <= len; i += 4) {
		uint32_t v;
		switch (rnd() % 8) {
		case 0: case 1: case 2:
			v = some_id();
			break;
		case 3:
			v = new_id();
			break;
		case 4:
			v = rnd() % 64;
			break;
		case 5:
			v = (rnd() % 2000) | (rnd() % 2000) << 16;
			break;
		default:
			v = rnd();
			break;
		}
		memcpy(req + i, &v, 4);
	}
}

// ---- replaying real apps ----

struct recording {
	uint8_t *data;
	size_t len;
};

static struct recording corpus[256];
static int corpus_len;

static void load_corpus(const char *dir) {
	for (int i = 0; i < 4096 && corpus_len < 256; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/conn-%d.bin", dir, i);
		FILE *f = fopen(path, "rb");
		if (!f) {
			continue;
		}
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (len > 12) {
			uint8_t *data = malloc((size_t)len);
			if (data && fread(data, 1, (size_t)len, f) == (size_t)len) {
				corpus[corpus_len++] = (struct recording){data, (size_t)len};
			} else {
				free(data);
			}
		}
		fclose(f);
	}
}

static size_t pad4(size_t n) {
	return (n + 3) & ~(size_t)3;
}

// the id range the recorded client had: what its first create made
static uint32_t recorded_base(const uint8_t *req, size_t len) {
	for (size_t at = 0; at + 8 <= len;) {
		uint16_t units;
		memcpy(&units, req + at + 2, 2);
		size_t size = (size_t)units * 4;
		if (!units) {
			uint32_t big;
			memcpy(&big, req + at + 4, 4);
			size = (size_t)big * 4;
		}
		uint8_t op = req[at];
		if ((op == 1 || op == 53 || op == 55) && at + 8 <= len) {
			uint32_t id;
			memcpy(&id, req + at + 4, 4);
			return id & ~0x1fffffu;
		}
		if (size < 4) {
			break;
		}
		at += size;
	}
	return 0;
}

static long replay(const struct recording *rec, long budget) {
	size_t setup_len = 12 + pad4((size_t)(rec->data[6] | rec->data[7] << 8))
		+ pad4((size_t)(rec->data[8] | rec->data[9] << 8));
	if (setup_len >= rec->len) {
		return 0;
	}
	int fd = connect_x(getenv("DISPLAY"));
	if (fd < 0 || !setup(fd)) {
		return -1;
	}
	const uint8_t *req = rec->data + setup_len;
	size_t len = rec->len - setup_len;
	uint32_t old_base = recorded_base(req, len);
	long sent = 0;
	for (size_t at = 0; at + 4 <= len && sent < budget; sent++) {
		uint16_t units;
		memcpy(&units, req + at + 2, 2);
		size_t size = (size_t)units * 4;
		if (!units && at + 8 <= len) {
			uint32_t big;
			memcpy(&big, req + at + 4, 4);
			size = (size_t)big * 4;
		}
		if (size < 4 || at + size > len) {
			break;
		}
		uint8_t *copy = malloc(size);
		memcpy(copy, req + at, size);
		at += size;
		// its ids, in our range
		for (size_t i = 4; old_base && i + 4 <= size; i += 4) {
			uint32_t v;
			memcpy(&v, copy + i, 4);
			if ((v & ~0x1fffffu) == old_base) {
				v = base | (v & 0x1fffff);
				memcpy(copy + i, &v, 4);
			}
		}
		if (rnd() % 8 == 0) {
			switch (rnd() % 5) {
			case 0:
				if (size > 4) {
					copy[4 + rnd() % (size - 4)] ^= (uint8_t)(1u << (rnd() % 8));
				}
				break;
			case 1:
				if (size >= 8) {
					uint32_t v = rnd() % 2 ? rnd() : some_id();
					memcpy(copy + 4 + 4 * (rnd() % ((size - 4) / 4)), &v, 4);
				}
				break;
			case 2:
				copy[1] = (uint8_t)rnd();
				break;
			case 3:
				if (units > 1) { // say it's shorter than it is
					uint16_t less = (uint16_t)(1 + rnd() % units);
					memcpy(copy + 2, &less, 2);
					size = (size_t)less * 4;
				}
				break;
			default:
				free(copy);
				continue; // left out
			}
		}
		bool ok = write(fd, copy, size) == (ssize_t)size;
		free(copy);
		if (!ok || (sent % 16 == 0 && !drain(fd))) {
			break;
		}
	}
	drain(fd);
	close(fd);
	return sent;
}

int main(void) {
	const char *display = getenv("DISPLAY");
	const char *rounds_env = getenv("FUZZ_ROUNDS");
	const char *seed_env = getenv("FUZZ_SEED");
	long rounds = rounds_env ? atol(rounds_env) : 200000;
	state = seed_env ? strtoull(seed_env, NULL, 0) : 0x9e3779b97f4a7c15ull;
	if (!display) {
		return 2;
	}
	signal(SIGPIPE, SIG_IGN); // the server dropping us is fine
	const char *corpus_dir = getenv("FUZZ_CORPUS");
	long sent = 0, connections = 0;
	if (corpus_dir) {
		load_corpus(corpus_dir);
		if (!corpus_len) {
			fprintf(stderr, "fuzz: nothing in %s\n", corpus_dir);
			return 2;
		}
		while (sent < rounds) {
			long n = replay(&corpus[rnd() % (uint32_t)corpus_len], rounds - sent);
			if (n < 0) {
				fprintf(stderr, "fuzz: the server's gone after %ld requests\n", sent);
				return 1;
			}
			sent += n ? n : 1;
			connections++;
		}
		printf("fuzz: %ld replayed requests over %ld connections, still standing\n", sent,
			connections);
		return 0;
	}
	// the extension opcodes flame has: 128 to 148
	while (sent < rounds) {
		int fd = connect_x(display);
		if (fd < 0 || !setup(fd)) {
			fprintf(stderr, "fuzz: the server's gone after %ld requests\n", sent);
			return 1;
		}
		connections++;
		ids_len = 0;
		int per = 200 + (int)(rnd() % 2000);
		for (int i = 0; i < per && sent < rounds; i++, sent++) {
			uint8_t req[1024] = {0};
			uint8_t opcode = rnd() % 3 ? (uint8_t)(1 + rnd() % 127) : (uint8_t)(128 + rnd() % 21);
			uint16_t units = (uint16_t)(1 + rnd() % 32);
			if (rnd() % 50 == 0) {
				units = (uint16_t)(rnd() % 256); // a length that lies
			}
			size_t len = units ? (size_t)units * 4 : 8;
			if (len > sizeof(req)) {
				len = sizeof(req);
			}
			fill(req, len);
			req[0] = opcode;
			req[1] = (uint8_t)(rnd() % (opcode >= 128 ? 64 : 256));
			memcpy(req + 2, &units, 2);
			if (write(fd, req, len) < 0) {
				break; // dropped us, fine
			}
			if (i % 16 == 0 && !drain(fd)) {
				break;
			}
		}
		close(fd);
	}
	printf("fuzz: %ld requests over %ld connections, still standing\n", sent, connections);
	return 0;
}
