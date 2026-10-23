#include "artfetch.h"

#include <curl/curl.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MOST (16 << 20) // a picture bigger than this isn't one
#define KEPT_DAYS 14 // pictures not looked at in this long go

enum { RUNNING, OK, FAILED, LET_GO };

struct art_fetch {
	atomic_int state;
	char url[PATH_MAX], path[PATH_MAX];
	void (*wake)(void *data);
	void *data;
};

bool art_is_web(const char *url) {
	return !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
}

static bool cache_dir(char *out, size_t size) {
	const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	int n = cache && *cache == '/' ? snprintf(out, size, "%s/flame/art", cache)
		: home ? snprintf(out, size, "%s/.cache/flame/art", home) : -1;
	return n > 0 && (size_t)n < size;
}

bool art_cached_path(const char *url, char *out, size_t size) {
	char dir[PATH_MAX];
	if (!cache_dir(dir, sizeof(dir))) {
		return false;
	}
	// its name: the url's fnv-1a
	uint64_t h = 0xcbf29ce484222325ull;
	for (const unsigned char *c = (const unsigned char *)url; *c; c++) {
		h = (h ^ *c) * 0x100000001b3ull;
	}
	int n = snprintf(out, size, "%s/%016llx", dir, (unsigned long long)h);
	return n > 0 && (size_t)n < size;
}

// the folder made (and its parents)
static bool make_dirs(char *path) {
	for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		bool ok = mkdir(path, 0700) == 0 || access(path, F_OK) == 0;
		*slash = '/';
		if (!ok) {
			return false;
		}
	}
	return mkdir(path, 0700) == 0 || access(path, F_OK) == 0;
}

// pictures nobody's looked at in a while: gone
static void prune(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	time_t old = time(NULL) - KEPT_DAYS * 86400;
	struct dirent *e;
	while ((e = readdir(d))) {
		char path[PATH_MAX];
		struct stat st;
		if (e->d_name[0] != '.' && snprintf(path, sizeof(path), "%s/%s", dir, e->d_name)
				< (int)sizeof(path) && stat(path, &st) == 0 && S_ISREG(st.st_mode)
				&& st.st_atime < old && st.st_mtime < old) {
			unlink(path);
		}
	}
	closedir(d);
}

struct sink {
	FILE *file;
	size_t written;
};

static size_t take(char *data, size_t size, size_t n, void *user) {
	struct sink *sink = user;
	size_t len = size * n;
	if (sink->written + len > MOST) {
		return 0; // too big: it stops
	}
	sink->written += len;
	return fwrite(data, 1, len, sink->file);
}

static bool fetch(const char *url, const char *path) {
	char part[PATH_MAX + 8];
	snprintf(part, sizeof(part), "%s.part", path);
	struct sink sink = {.file = fopen(part, "wb")};
	if (!sink.file) {
		return false;
	}
	CURL *curl = curl_easy_init();
	bool ok = false;
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MOST);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "flamebar");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, take);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
		ok = curl_easy_perform(curl) == CURLE_OK && sink.written > 0;
		curl_easy_cleanup(curl);
	}
	ok = fclose(sink.file) == 0 && ok;
	// there whole or not at all: what's read is never half a picture
	if (!ok || rename(part, path) < 0) {
		unlink(part);
		return false;
	}
	return true;
}

static void *run(void *data) {
	struct art_fetch *f = data;
	static atomic_bool pruned;
	char dir[PATH_MAX];
	bool ok = cache_dir(dir, sizeof(dir)) && make_dirs(dir);
	if (ok && !atomic_exchange(&pruned, true)) {
		prune(dir);
	}
	ok = ok && fetch(f->url, f->path);
	int was = RUNNING;
	if (!atomic_compare_exchange_strong(&f->state, &was, ok ? OK : FAILED)) {
		free(f); // let go of while it ran: nobody wants it
		return NULL;
	}
	f->wake(f->data);
	return NULL;
}

struct art_fetch *art_fetch_start(const char *url, void (*wake)(void *data), void *data) {
	static bool started;
	if (!started) {
		// before any thread: curl's setup isn't safe alongside them
		started = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
		if (!started) {
			return NULL;
		}
	}
	struct art_fetch *f = calloc(1, sizeof(*f));
	if (!f || !art_is_web(url) || strlen(url) >= sizeof(f->url)
			|| !art_cached_path(url, f->path, sizeof(f->path))) {
		free(f);
		return NULL;
	}
	snprintf(f->url, sizeof(f->url), "%s", url);
	f->wake = wake;
	f->data = data;
	atomic_init(&f->state, RUNNING);
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&thread, &attr, run, f) == 0;
	pthread_attr_destroy(&attr);
	if (!ok) {
		free(f);
		return NULL;
	}
	return f;
}

bool art_fetch_done(struct art_fetch *f, bool *ok) {
	int state = atomic_load(&f->state);
	*ok = state == OK;
	return state == OK || state == FAILED;
}

void art_fetch_free(struct art_fetch *f) {
	if (f && atomic_exchange(&f->state, LET_GO) != RUNNING) {
		free(f);
	}
}
