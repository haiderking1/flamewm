#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOOK "look" // the file saying what the look is, in the person's folder
#define PICTURE "wallpaper-" // a picture's name starts so, a new name every time

static const char *dir(void) {
	const char *d = getenv("FLAMEGREET_DIR");
	return d && *d ? d : "/var/lib/flamegreet";
}

bool store_available(void) {
	return access(dir(), W_OK | X_OK) == 0;
}

// theirs and only they can change it: owned by uid, not a link, nobody
// else writes it
static bool trusted(const struct stat *st, uid_t uid, mode_t kind) {
	return (st->st_mode & S_IFMT) == kind && st->st_uid == uid && !(st->st_mode & 022);
}

static void defaults(struct greet_look *look) {
	look->wallpaper[0] = '\0';
	look->blur = true;
}

// look's text into look: "key = value" lines
static void parse(const char *text, struct greet_look *look, char *picture, size_t size) {
	const char *line = text;
	while (*line) {
		size_t len = strcspn(line, "\n");
		char copy[512];
		snprintf(copy, sizeof(copy), "%.*s", (int)(len < sizeof(copy) ? len : sizeof(copy) - 1),
			line);
		char *eq = strchr(copy, '=');
		if (eq) {
			*eq = '\0';
			char *key = copy, *value = eq + 1;
			while (*key == ' ') {
				key++;
			}
			for (char *e = key + strlen(key); e > key && e[-1] == ' '; e--) {
				e[-1] = '\0';
			}
			while (*value == ' ') {
				value++;
			}
			if (!strcmp(key, "blur")) {
				look->blur = strcmp(value, "false") != 0;
			} else if (!strcmp(key, "wallpaper")) {
				snprintf(picture, size, "%s", value);
			}
		}
		line += len + (line[len] == '\n');
	}
}

void store_read(const char *user, uid_t uid, struct greet_look *look) {
	defaults(look);
	char path[4096];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", dir(), user);
	if (strchr(user, '/') || !*user || lstat(path, &st) < 0 || !trusted(&st, uid, S_IFDIR)) {
		return;
	}
	snprintf(path, sizeof(path), "%s/%s/" LOOK, dir(), user);
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	char text[1024];
	ssize_t n = fstat(fd, &st) == 0 && trusted(&st, uid, S_IFREG)
		? read(fd, text, sizeof(text) - 1) : -1;
	close(fd);
	if (n < 0) {
		return;
	}
	text[n] = '\0';
	char picture[256] = "";
	parse(text, look, picture, sizeof(picture));
	// a picture of theirs next to it, nothing anywhere else
	if (strncmp(picture, PICTURE, strlen(PICTURE)) || strchr(picture, '/')) {
		return;
	}
	snprintf(path, sizeof(path), "%s/%s/%s", dir(), user, picture);
	if (lstat(path, &st) == 0 && trusted(&st, uid, S_IFREG)) {
		snprintf(look->wallpaper, sizeof(look->wallpaper), "%s", path);
	}
}

// ---- writing, as whoever runs this ----

// their folder, made if it isn't there. false when it's not theirs
static bool own_dir(char *path, size_t size, const char **error) {
	struct passwd *pw = getpwuid(getuid());
	if (!pw) {
		*error = "who are you";
		return false;
	}
	snprintf(path, size, "%s/%s", dir(), pw->pw_name);
	if (mkdir(path, 0755) < 0 && errno != EEXIST) {
		*error = errno == ENOENT ? "flamegreet isn't installed" : "can't make its folder";
		return false;
	}
	// readable by the login screen whatever the umask made it
	struct stat st;
	if (lstat(path, &st) < 0 || (st.st_mode & S_IFMT) != S_IFDIR || st.st_uid != getuid()
			|| chmod(path, 0755) < 0) {
		*error = "its folder isn't yours";
		return false;
	}
	return true;
}

// what's in folder's look now (the defaults when nothing)
static void current(const char *folder, struct greet_look *look, char *picture, size_t size) {
	defaults(look);
	picture[0] = '\0';
	char path[4096];
	snprintf(path, sizeof(path), "%s/" LOOK, folder);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char text[1024];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	parse(text, look, picture, size);
}

// a file into folder whole or not at all: written beside it, then renamed
static bool write_whole(const char *folder, const char *name, int from_fd, const char *text) {
	char tmp[4096], path[4096];
	snprintf(tmp, sizeof(tmp), "%s/.new-XXXXXX", folder);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		return false;
	}
	bool ok = fchmod(fd, 0644) == 0;
	if (text) {
		size_t len = strlen(text);
		ok = ok && write(fd, text, len) == (ssize_t)len;
	} else {
		char buffer[65536];
		ssize_t n;
		while (ok && (n = read(from_fd, buffer, sizeof(buffer))) > 0) {
			ok = write(fd, buffer, (size_t)n) == n;
		}
		ok = ok && n == 0;
	}
	ok = close(fd) == 0 && ok;
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static bool write_look(const char *folder, const struct greet_look *look, const char *picture) {
	char text[512];
	snprintf(text, sizeof(text), "wallpaper = %s\nblur = %s\n", picture,
		look->blur ? "true" : "false");
	return write_whole(folder, LOOK, -1, text);
}

// every picture there but keep (NULL: all of them)
static void drop_pictures(const char *folder, const char *keep) {
	DIR *d = opendir(folder);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, PICTURE, strlen(PICTURE)) && (!keep || strcmp(e->d_name, keep))) {
			unlinkat(dirfd(d), e->d_name, 0);
		}
	}
	closedir(d);
}

bool store_set_wallpaper(const char *from, const char **error) {
	char folder[4096];
	if (!own_dir(folder, sizeof(folder), error)) {
		return false;
	}
	struct greet_look look;
	char old[256];
	current(folder, &look, old, sizeof(old));
	char picture[256] = "";
	if (from) {
		int fd = open(from, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			*error = "can't read the picture";
			return false;
		}
		// a name never used before: whatever had the last one cached
		// (thumbnails) doesn't show it for this
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		snprintf(picture, sizeof(picture), PICTURE "%lld%09ld", (long long)now.tv_sec,
			now.tv_nsec);
		bool ok = write_whole(folder, picture, fd, NULL);
		close(fd);
		if (!ok) {
			*error = "can't copy the picture";
			return false;
		}
	}
	if (!write_look(folder, &look, picture)) {
		*error = "can't save it";
		return false;
	}
	drop_pictures(folder, from ? picture : NULL);
	return true;
}

bool store_set_blur(bool blur, const char **error) {
	char folder[4096];
	if (!own_dir(folder, sizeof(folder), error)) {
		return false;
	}
	struct greet_look look;
	char picture[256];
	current(folder, &look, picture, sizeof(picture));
	look.blur = blur;
	if (!write_look(folder, &look, picture)) {
		*error = "can't save it";
		return false;
	}
	return true;
}
