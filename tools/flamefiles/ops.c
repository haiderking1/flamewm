#include "ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---- quick ones ----

// app (a flame app: next to flamefiles first, then $PATH; NULL: xdg-open)
// run on path, twice forked: it isn't ours to wait for, and never a zombie
static void run(const char *app, const char *path) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			if (app) {
				char self[PATH_MAX];
				ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
				char *slash = n > 0 ? (self[n] = '\0', strrchr(self, '/')) : NULL;
				if (slash) {
					snprintf(slash + 1, sizeof(self) - (size_t)(slash + 1 - self), "%s", app);
					execl(self, app, path, (char *)NULL);
				}
				execlp(app, app, path, (char *)NULL);
			}
			execlp("xdg-open", "xdg-open", path, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

void ops_open(const char *path) {
	run(NULL, path);
}

void ops_open_with(const char *app, const char *path) {
	run(app, path);
}

static bool exists(const char *path) {
	struct stat st;
	return lstat(path, &st) == 0;
}

// name's stem and extension: "photo.jpg" is "photo" and ".jpg", a folder's
// or a dotfile's all stem
static size_t stem_len(const char *name, bool folder) {
	const char *dot = folder ? NULL : strrchr(name, '.');
	return dot && dot != name ? (size_t)(dot - name) : strlen(name);
}

// a name in dir nothing has: name, then with copy (if asked) and numbers
static bool free_name(const char *dir, const char *name, bool folder, bool copy, char *out,
		size_t size) {
	size_t stem = stem_len(name, folder);
	for (int n = 1; n < 10000; n++) {
		char number[16] = "";
		if (n > 1) {
			snprintf(number, sizeof(number), " %d", n);
		}
		if (!copy && n == 1) {
			snprintf(out, size, "%s", name);
		} else {
			snprintf(out, size, "%.*s%s%s%s", (int)stem, name, copy ? " copy" : "", number,
				name + stem);
		}
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", dir, out);
		if (!exists(path)) {
			return true;
		}
	}
	return false;
}

bool ops_new_folder(const char *dir, char *out, size_t size, const char **error) {
	char path[PATH_MAX];
	for (int tries = 0; tries < 5; tries++) {
		if (!free_name(dir, "untitled folder", true, false, out, size)) {
			break;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, out);
		if (mkdir(path, 0777) == 0) {
			return true;
		}
		if (errno != EEXIST) {
			*error = strerror(errno);
			return false;
		}
	}
	*error = "no free name";
	return false;
}

bool ops_rename(const char *dir, const char *from, const char *to, const char **error) {
	if (!*to || strchr(to, '/') || !strcmp(to, ".") || !strcmp(to, "..")) {
		*error = "a name can't be empty or have a /";
		return false;
	}
	if (!strcmp(from, to)) {
		return true;
	}
	char a[PATH_MAX], b[PATH_MAX];
	snprintf(a, sizeof(a), "%s/%s", dir, from);
	snprintf(b, sizeof(b), "%s/%s", dir, to);
	// only the case changing on a filesystem that ignores case is the same file
	struct stat sa, sb;
	if (lstat(b, &sb) == 0 && !(lstat(a, &sa) == 0 && sa.st_ino == sb.st_ino
			&& sa.st_dev == sb.st_dev)) {
		*error = "something's already called that";
		return false;
	}
	if (rename(a, b) < 0) {
		*error = strerror(errno);
		return false;
	}
	return true;
}

// ---- the trash ----

static void data_home(char *out, size_t size) {
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg == '/') {
		snprintf(out, size, "%s", xdg);
	} else {
		snprintf(out, size, "%s/.local/share", home ? home : "");
	}
}

static void home_trash(char *out, size_t size) {
	char data[PATH_MAX];
	data_home(data, sizeof(data));
	snprintf(out, size, "%s/Trash", data);
}

static bool mkdirs(const char *path, mode_t mode);

void ops_trash_dir(char *out, size_t size) {
	char trash[PATH_MAX];
	home_trash(trash, sizeof(trash));
	snprintf(out, size, "%s/files", trash);
	mkdirs(out, 0700);
}

static bool mkdirs(const char *path, mode_t mode) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = '\0';
			if (mkdir(p, mode) < 0 && errno != EEXIST) {
				return false;
			}
			*s = '/';
		}
	}
	return mkdir(p, mode) == 0 || errno == EEXIST;
}

// the trash spec's Path: percent escapes for all but the plain bytes
static void url_escape(const char *in, char *out, size_t size) {
	size_t n = 0;
	for (const unsigned char *c = (const unsigned char *)in; *c && n + 4 < size; c++) {
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
				|| strchr("/-_.~", *c)) {
			out[n++] = (char)*c;
		} else {
			n += (size_t)snprintf(out + n, size - n, "%%%02X", *c);
		}
	}
	out[n] = '\0';
}

static void url_unescape(const char *in, char *out, size_t size) {
	size_t n = 0;
	for (const char *c = in; *c && n + 1 < size; c++) {
		unsigned v;
		if (*c == '%' && sscanf(c + 1, "%2x", &v) == 1) {
			out[n++] = (char)v;
			c += 2;
		} else {
			out[n++] = *c;
		}
	}
	out[n] = '\0';
}

// the top of the filesystem path is on
static void mount_top(const char *path, char *out, size_t size) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s", path);
	struct stat st;
	if (lstat(p, &st) < 0) {
		snprintf(out, size, "/");
		return;
	}
	dev_t dev = st.st_dev;
	for (;;) {
		char *slash = strrchr(p, '/');
		if (!slash || slash == p) {
			struct stat root;
			snprintf(out, size, "%s", stat("/", &root) == 0 && root.st_dev == dev ? "/" : p);
			return;
		}
		*slash = '\0';
		struct stat up;
		if (stat(p, &up) < 0 || up.st_dev != dev) {
			*slash = '/';
			snprintf(out, size, "%.*s", (int)(slash - p), p);
			return;
		}
	}
}

// path into the trash at trash (its files and info), Path written as shown
static bool trash_into(const char *trash, const char *path, const char *shown,
		const char **error) {
	char files[PATH_MAX], info[PATH_MAX];
	snprintf(files, sizeof(files), "%s/files", trash);
	snprintf(info, sizeof(info), "%s/info", trash);
	if (!mkdirs(files, 0700) || !mkdirs(info, 0700)) {
		*error = "the trash can't be made";
		return false;
	}
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	struct stat st;
	bool folder = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
	char escaped[PATH_MAX * 3], date[32];
	url_escape(shown, escaped, sizeof(escaped));
	time_t now = time(NULL);
	struct tm t;
	localtime_r(&now, &t);
	strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &t);
	// the info file claims the name first: two trashing at once don't collide
	for (int n = 1; n < 10000; n++) {
		char as[NAME_MAX + 1], info_path[PATH_MAX], file_path[PATH_MAX];
		if (n == 1) {
			snprintf(as, sizeof(as), "%s", name);
		} else {
			size_t stem = stem_len(name, folder);
			snprintf(as, sizeof(as), "%.*s.%d%s", (int)stem, name, n, name + stem);
		}
		snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", info, as);
		snprintf(file_path, sizeof(file_path), "%s/%s", files, as);
		if (exists(file_path)) {
			continue;
		}
		int fd = open(info_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0) {
			if (errno == EEXIST) {
				continue;
			}
			*error = strerror(errno);
			return false;
		}
		dprintf(fd, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped, date);
		close(fd);
		if (rename(path, file_path) < 0) {
			int err = errno;
			unlink(info_path);
			*error = err == EXDEV ? "it's on another drive" : strerror(err);
			return false;
		}
		return true;
	}
	*error = "the trash is full of that name";
	return false;
}

bool ops_trash(const char *path, const char **error) {
	char trash[PATH_MAX];
	home_trash(trash, sizeof(trash));
	struct stat st, home_st;
	char data[PATH_MAX];
	data_home(data, sizeof(data));
	mkdirs(data, 0700);
	if (lstat(path, &st) < 0) {
		*error = strerror(errno);
		return false;
	}
	if (stat(data, &home_st) == 0 && home_st.st_dev == st.st_dev) {
		return trash_into(trash, path, path, error);
	}
	// another drive: its own trash at its top, Path from there
	char top[PATH_MAX], drive_trash[PATH_MAX];
	mount_top(path, top, sizeof(top));
	snprintf(drive_trash, sizeof(drive_trash), "%s/.Trash-%u", strcmp(top, "/") ? top : "",
		(unsigned)getuid());
	if (mkdir(drive_trash, 0700) < 0 && errno != EEXIST) {
		*error = "that drive has no trash it lets us make";
		return false;
	}
	size_t tl = strlen(top);
	const char *relative = !strcmp(top, "/") ? path + 1 : path + tl + 1;
	return trash_into(drive_trash, path, relative, error);
}

bool ops_put_back(const char *name, const char **error) {
	char trash[PATH_MAX], info_path[PATH_MAX], file_path[PATH_MAX];
	home_trash(trash, sizeof(trash));
	snprintf(info_path, sizeof(info_path), "%s/info/%s.trashinfo", trash, name);
	snprintf(file_path, sizeof(file_path), "%s/files/%s", trash, name);
	FILE *f = fopen(info_path, "re");
	if (!f) {
		*error = "it doesn't say where it came from";
		return false;
	}
	char line[PATH_MAX * 3 + 16], to[PATH_MAX] = "";
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "Path=", 5)) {
			line[strcspn(line, "\n")] = '\0';
			url_unescape(line + 5, to, sizeof(to));
		}
	}
	fclose(f);
	if (to[0] != '/') {
		*error = "it doesn't say where it came from";
		return false;
	}
	if (exists(to)) {
		*error = "something else is there now";
		return false;
	}
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", to);
	char *slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
		mkdirs(parent, 0777);
	}
	if (rename(file_path, to) < 0) {
		*error = strerror(errno);
		return false;
	}
	unlink(info_path);
	return true;
}

static bool remove_tree(int dir_fd, const char *name) {
	struct stat st;
	if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		return errno == ENOENT;
	}
	if (S_ISDIR(st.st_mode)) {
		int fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
		if (!d) {
			if (fd >= 0) {
				close(fd);
			}
			return false;
		}
		bool ok = true;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
				ok &= remove_tree(dirfd(d), e->d_name);
			}
		}
		closedir(d);
		return ok && unlinkat(dir_fd, name, AT_REMOVEDIR) == 0;
	}
	return unlinkat(dir_fd, name, 0) == 0;
}

bool ops_delete(const char *path, const char **error) {
	if (!remove_tree(AT_FDCWD, path)) {
		*error = strerror(errno);
		return false;
	}
	// a trashed one takes its info with it
	char files[PATH_MAX];
	ops_trash_dir(files, sizeof(files));
	size_t fl = strlen(files);
	if (!strncmp(path, files, fl) && path[fl] == '/' && !strchr(path + fl + 1, '/')) {
		char info[PATH_MAX];
		snprintf(info, sizeof(info), "%.*s/info/%s.trashinfo", (int)(fl - 6), files, path + fl + 1);
		unlink(info);
	}
	return true;
}

// ---- copying and moving ----

static void job_error(struct ops_job *job, const char *what, const char *name, int err) {
	pthread_mutex_lock(&job->lock);
	if (!job->error[0]) {
		snprintf(job->error, sizeof(job->error), "%s %s: %s", what, name, strerror(err));
	}
	pthread_mutex_unlock(&job->lock);
}

static bool cancelled(struct ops_job *job) {
	pthread_mutex_lock(&job->lock);
	bool c = job->cancel;
	pthread_mutex_unlock(&job->lock);
	return c;
}

static void progress(struct ops_job *job, uint64_t bytes) {
	pthread_mutex_lock(&job->lock);
	job->done_bytes += bytes;
	pthread_mutex_unlock(&job->lock);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
	if (now - job->last_wake > 0.1) {
		job->last_wake = now;
		fui_wake(job->ui);
	}
}

static uint64_t tree_size(const char *path) {
	struct stat st;
	if (lstat(path, &st) < 0) {
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		return S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
	}
	uint64_t total = 0;
	DIR *d = opendir(path);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
			char sub[PATH_MAX];
			snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
			total += tree_size(sub);
		}
	}
	if (d) {
		closedir(d);
	}
	return total;
}

static bool copy_file(struct ops_job *job, const char *from, const char *to,
		const struct stat *st) {
	int in = open(from, O_RDONLY | O_CLOEXEC);
	int out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 07777)
		: -1;
	if (in < 0 || out < 0) {
		job_error(job, "can't copy", from, errno);
		if (in >= 0) {
			close(in);
		}
		return false;
	}
	char *buf = job->buf;
	bool ok = true;
	for (;;) {
		if (cancelled(job)) {
			ok = false;
			break;
		}
		ssize_t n = read(in, buf, 1 << 20);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			ok = n == 0;
			if (!ok) {
				job_error(job, "can't read", from, errno);
			}
			break;
		}
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR) {
					continue;
				}
				job_error(job, "can't write", to, errno);
				ok = false;
				break;
			}
			off += w;
		}
		if (!ok) {
			break;
		}
		progress(job, (uint64_t)n);
	}
	if (ok) {
		struct timespec times[2] = {st->st_atim, st->st_mtim};
		futimens(out, times);
	}
	close(in);
	if (close(out) < 0 && ok) {
		job_error(job, "can't write", to, errno);
		ok = false;
	}
	if (!ok) {
		unlink(to); // half a file is worse than none
	}
	return ok;
}

static bool copy_tree(struct ops_job *job, const char *from, const char *to) {
	struct stat st;
	if (lstat(from, &st) < 0) {
		job_error(job, "can't copy", from, errno);
		return false;
	}
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(from, target, sizeof(target) - 1);
		if (n < 0) {
			job_error(job, "can't copy", from, errno);
			return false;
		}
		target[n] = '\0';
		if (symlink(target, to) < 0) {
			job_error(job, "can't copy", from, errno);
			return false;
		}
		return true;
	}
	if (S_ISREG(st.st_mode)) {
		return copy_file(job, from, to, &st);
	}
	if (!S_ISDIR(st.st_mode)) {
		return true; // a fifo, a device: not a file to copy
	}
	if (mkdir(to, (st.st_mode & 07777) | 0700) < 0) {
		job_error(job, "can't make", to, errno);
		return false;
	}
	DIR *d = opendir(from);
	if (!d) {
		job_error(job, "can't read", from, errno);
		return false;
	}
	bool ok = true;
	struct dirent *e;
	while (ok && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		char a[PATH_MAX], b[PATH_MAX];
		snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
		snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
		ok = copy_tree(job, a, b);
	}
	closedir(d);
	chmod(to, st.st_mode & 07777);
	return ok;
}

static void *job_thread(void *data) {
	struct ops_job *job = data;
	uint64_t total = 0;
	for (size_t i = 0; i < job->len; i++) {
		total += tree_size(job->paths[i]);
	}
	pthread_mutex_lock(&job->lock);
	job->total = total;
	pthread_mutex_unlock(&job->lock);
	for (size_t i = 0; i < job->len && !cancelled(job); i++) {
		const char *from = job->paths[i];
		const char *slash = strrchr(from, '/');
		const char *name = slash ? slash + 1 : from;
		char from_dir[PATH_MAX];
		snprintf(from_dir, sizeof(from_dir), "%.*s", slash && slash != from
			? (int)(slash - from) : 1, slash && slash != from ? from : "/");
		bool same_dir = !strcmp(from_dir, job->dest);
		size_t fl = strlen(from);
		if (!strncmp(job->dest, from, fl) && (job->dest[fl] == '/' || !job->dest[fl])) {
			pthread_mutex_lock(&job->lock);
			snprintf(job->error, sizeof(job->error), "%s can't go inside itself", name);
			pthread_mutex_unlock(&job->lock);
			break;
		}
		if (job->move && same_dir) {
			continue; // already there
		}
		struct stat st;
		bool folder = lstat(from, &st) == 0 && S_ISDIR(st.st_mode);
		char as[NAME_MAX + 1], to[PATH_MAX];
		if (!free_name(job->dest, name, folder, same_dir, as, sizeof(as))) {
			job_error(job, "no free name for", name, EEXIST);
			break;
		}
		snprintf(to, sizeof(to), "%s/%s", job->dest, as);
		pthread_mutex_lock(&job->lock);
		snprintf(job->current, sizeof(job->current), "%s", name);
		if (!job->first[0]) {
			snprintf(job->first, sizeof(job->first), "%s", as);
		}
		pthread_mutex_unlock(&job->lock);
		if (job->move) {
			if (rename(from, to) == 0) {
				progress(job, tree_size(to));
				continue;
			}
			if (errno != EXDEV) {
				job_error(job, "can't move", name, errno);
				break;
			}
			// another drive: copied, then the original goes
			if (!copy_tree(job, from, to)) {
				break;
			}
			if (!remove_tree(AT_FDCWD, from)) {
				job_error(job, "copied but can't remove", name, errno);
				break;
			}
		} else if (!copy_tree(job, from, to)) {
			break;
		}
	}
	pthread_mutex_lock(&job->lock);
	job->finished = true;
	pthread_mutex_unlock(&job->lock);
	fui_wake(job->ui);
	return NULL;
}

struct ops_job *ops_job_start(struct fui *ui, char *const *paths, size_t len, const char *dest,
		bool move) {
	struct ops_job *job = calloc(1, sizeof(*job));
	if (!job || !(job->paths = calloc(len, sizeof(char *)))) {
		free(job);
		return NULL;
	}
	job->ui = ui;
	job->len = len;
	job->buf = malloc(1 << 20);
	if (!job->buf) {
		free(job->paths);
		free(job);
		return NULL;
	}
	job->move = move;
	snprintf(job->dest, sizeof(job->dest), "%s", dest);
	for (size_t i = 0; i < len; i++) {
		job->paths[i] = strdup(paths[i]);
	}
	pthread_mutex_init(&job->lock, NULL);
	if (pthread_create(&job->thread, NULL, job_thread, job)) {
		job->thread = 0;
		ops_job_free(job);
		return NULL;
	}
	return job;
}

void ops_job_free(struct ops_job *job) {
	if (job->thread) {
		pthread_mutex_lock(&job->lock);
		job->cancel = true;
		pthread_mutex_unlock(&job->lock);
		pthread_join(job->thread, NULL);
	}
	for (size_t i = 0; i < job->len; i++) {
		free(job->paths[i]);
	}
	free(job->paths);
	free(job->buf);
	pthread_mutex_destroy(&job->lock);
	free(job);
}
