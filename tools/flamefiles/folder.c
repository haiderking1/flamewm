#include "folder.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILES_MAX 50000

static const struct {
	const char *ext;
	enum file_kind kind;
	const char *name; // NULL: the extension, upper case, and the kind's word
} kinds[] = {
	{"jpg", KIND_IMAGE, "JPEG image"}, {"jpeg", KIND_IMAGE, "JPEG image"},
	{"png", KIND_IMAGE, NULL}, {"webp", KIND_IMAGE, NULL}, {"gif", KIND_IMAGE, NULL},
	{"bmp", KIND_IMAGE, NULL}, {"svg", KIND_IMAGE, NULL}, {"tif", KIND_IMAGE, "TIFF image"},
	{"tiff", KIND_IMAGE, NULL}, {"heic", KIND_IMAGE, NULL}, {"avif", KIND_IMAGE, NULL},
	{"jxl", KIND_IMAGE, NULL}, {"ico", KIND_IMAGE, NULL}, {"xcf", KIND_IMAGE, "GIMP image"},
	{"mp4", KIND_VIDEO, NULL}, {"mkv", KIND_VIDEO, NULL}, {"webm", KIND_VIDEO, NULL},
	{"mov", KIND_VIDEO, NULL}, {"avi", KIND_VIDEO, NULL}, {"m4v", KIND_VIDEO, NULL},
	{"mp3", KIND_AUDIO, NULL}, {"flac", KIND_AUDIO, NULL}, {"ogg", KIND_AUDIO, NULL},
	{"opus", KIND_AUDIO, NULL}, {"wav", KIND_AUDIO, NULL}, {"m4a", KIND_AUDIO, NULL},
	{"aac", KIND_AUDIO, NULL},
	{"zip", KIND_ARCHIVE, NULL}, {"tar", KIND_ARCHIVE, NULL}, {"gz", KIND_ARCHIVE, NULL},
	{"xz", KIND_ARCHIVE, NULL}, {"zst", KIND_ARCHIVE, NULL}, {"bz2", KIND_ARCHIVE, NULL},
	{"7z", KIND_ARCHIVE, NULL}, {"rar", KIND_ARCHIVE, NULL}, {"tgz", KIND_ARCHIVE, NULL},
	{"deb", KIND_ARCHIVE, "Debian package"}, {"rpm", KIND_ARCHIVE, "RPM package"},
	{"iso", KIND_ARCHIVE, "Disk image"}, {"img", KIND_ARCHIVE, "Disk image"},
	{"c", KIND_CODE, "C source"}, {"h", KIND_CODE, "C header"}, {"cpp", KIND_CODE, "C++ source"},
	{"cc", KIND_CODE, "C++ source"}, {"hpp", KIND_CODE, "C++ header"},
	{"py", KIND_CODE, "Python script"}, {"js", KIND_CODE, "JavaScript"},
	{"ts", KIND_CODE, "TypeScript"}, {"rs", KIND_CODE, "Rust source"},
	{"go", KIND_CODE, "Go source"}, {"java", KIND_CODE, "Java source"},
	{"lua", KIND_CODE, "Lua script"}, {"sh", KIND_CODE, "Shell script"},
	{"fish", KIND_CODE, "Fish script"}, {"rb", KIND_CODE, "Ruby script"},
	{"php", KIND_CODE, "PHP script"}, {"html", KIND_CODE, "HTML"}, {"css", KIND_CODE, "CSS"},
	{"json", KIND_CODE, "JSON"}, {"xml", KIND_CODE, "XML"}, {"yml", KIND_CODE, "YAML"},
	{"yaml", KIND_CODE, "YAML"}, {"toml", KIND_CODE, "TOML"}, {"qml", KIND_CODE, "QML"},
	{"mk", KIND_CODE, "Makefile"},
	{"txt", KIND_TEXT, "Plain text"}, {"md", KIND_TEXT, "Markdown"}, {"log", KIND_TEXT, "Log"},
	{"conf", KIND_TEXT, "Config"}, {"ini", KIND_TEXT, "Config"}, {"cfg", KIND_TEXT, "Config"},
	{"csv", KIND_TEXT, "CSV"},
	{"pdf", KIND_PDF, "PDF document"},
	{"odt", KIND_DOCUMENT, "Document"}, {"docx", KIND_DOCUMENT, "Word document"},
	{"doc", KIND_DOCUMENT, "Word document"}, {"rtf", KIND_DOCUMENT, "Rich text"},
	{"ods", KIND_DOCUMENT, "Spreadsheet"}, {"xlsx", KIND_DOCUMENT, "Excel spreadsheet"},
	{"odp", KIND_DOCUMENT, "Presentation"}, {"pptx", KIND_DOCUMENT, "PowerPoint"},
	{"epub", KIND_DOCUMENT, "EPUB book"},
	{"appimage", KIND_PROGRAM, "AppImage"}, {"desktop", KIND_PROGRAM, "App launcher"},
	{"exe", KIND_PROGRAM, "Windows program"},
};

static const char *const kind_words[] = {
	[KIND_FOLDER] = "folder", [KIND_IMAGE] = "image", [KIND_VIDEO] = "video",
	[KIND_AUDIO] = "audio", [KIND_ARCHIVE] = "archive", [KIND_CODE] = "source",
	[KIND_TEXT] = "text", [KIND_PDF] = "document", [KIND_DOCUMENT] = "document",
	[KIND_PROGRAM] = "program", [KIND_OTHER] = "file",
};

static int kind_index(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name || !dot[1]) {
		return -1;
	}
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		if (!strcasecmp(dot + 1, kinds[i].ext)) {
			return (int)i;
		}
	}
	return -1;
}

enum file_kind kind_of_name(const char *name) {
	int i = kind_index(name);
	return i < 0 ? KIND_OTHER : kinds[i].kind;
}

static void describe(struct file *file, bool executable) {
	if (file->kind == KIND_FOLDER) {
		snprintf(file->kind_name, sizeof(file->kind_name), "Folder");
		return;
	}
	int i = kind_index(file->name);
	if (i >= 0) {
		file->kind = kinds[i].kind;
		if (kinds[i].name) {
			snprintf(file->kind_name, sizeof(file->kind_name), "%s", kinds[i].name);
		} else {
			char ext[12];
			size_t n = 0;
			for (const char *c = kinds[i].ext; *c && n < sizeof(ext) - 1; c++) {
				ext[n++] = (char)toupper((unsigned char)*c);
			}
			ext[n] = '\0';
			snprintf(file->kind_name, sizeof(file->kind_name), "%s %s", ext,
				kind_words[kinds[i].kind]);
		}
		return;
	}
	if (executable) {
		file->kind = KIND_PROGRAM;
		snprintf(file->kind_name, sizeof(file->kind_name), "Program");
		return;
	}
	file->kind = KIND_OTHER;
	snprintf(file->kind_name, sizeof(file->kind_name), "Document");
}

void folder_init(struct folder *folder) {
	*folder = (struct folder){.inotify = -1, .watch = -1};
	folder->inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
}

static void files_free(struct file *files, size_t len) {
	for (size_t i = 0; i < len; i++) {
		free(files[i].name);
	}
	free(files);
}

void folder_finish(struct folder *folder) {
	files_free(folder->files, folder->len);
	if (folder->inotify >= 0) {
		close(folder->inotify);
	}
	*folder = (struct folder){.inotify = -1, .watch = -1};
}

// how many things a folder has, not looking inside them
static off_t count_in(int dir_fd, const char *name) {
	int fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
	if (!d) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	off_t n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
	}
	closedir(d);
	return n;
}

bool folder_list(struct folder *folder, const char *path) {
	DIR *d = opendir(path);
	if (!d) {
		return false;
	}
	int dir_fd = dirfd(d);
	struct file *files = NULL;
	size_t len = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d)) && len < FILES_MAX) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		struct stat st, lst;
		if (fstatat(dir_fd, e->d_name, &lst, AT_SYMLINK_NOFOLLOW) < 0) {
			continue;
		}
		// a link shows what it points at, a broken one itself
		if (fstatat(dir_fd, e->d_name, &st, 0) < 0) {
			st = lst;
		}
		if (len == cap) {
			cap = cap ? cap * 2 : 128;
			struct file *grown = realloc(files, cap * sizeof(*files));
			if (!grown) {
				break;
			}
			files = grown;
		}
		struct file *f = &files[len];
		*f = (struct file){
			.name = strdup(e->d_name),
			.hidden = e->d_name[0] == '.',
			.link = S_ISLNK(lst.st_mode),
			.mtime = st.st_mtime,
			.kind = S_ISDIR(st.st_mode) ? KIND_FOLDER : KIND_OTHER,
		};
		if (!f->name) {
			continue;
		}
		f->size = f->kind == KIND_FOLDER ? count_in(dir_fd, e->d_name) : st.st_size;
		describe(f, S_ISREG(st.st_mode) && (st.st_mode & 0111));
		len++;
	}
	closedir(d);
	files_free(folder->files, folder->len);
	folder->files = files;
	folder->len = len;
	if (folder->path != path) {
		snprintf(folder->path, sizeof(folder->path), "%s", path);
	}
	size_t pl = strlen(folder->path);
	while (pl > 1 && folder->path[pl - 1] == '/') {
		folder->path[--pl] = '\0';
	}
	if (folder->inotify >= 0) {
		if (folder->watch >= 0) {
			inotify_rm_watch(folder->inotify, folder->watch);
		}
		folder->watch = inotify_add_watch(folder->inotify, folder->path, IN_CREATE | IN_DELETE
			| IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF
			| IN_MOVE_SELF | IN_ONLYDIR);
	}
	return true;
}

bool folder_changed(struct folder *folder) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool changed = false;
	ssize_t n;
	while (folder->inotify >= 0 && (n = read(folder->inotify, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			const struct inotify_event *ev = (const struct inotify_event *)p;
			// the last folder's, still on its way: not this one's
			changed |= ev->wd == folder->watch;
			p += sizeof(*ev) + ev->len;
		}
	}
	return changed;
}

// names the way people count: 2 before 10, any case
static int name_compare(const char *s, const char *t) {
	while (*s && *t) {
		if (isdigit((unsigned char)*s) && isdigit((unsigned char)*t)) {
			char *se, *te;
			unsigned long long m = strtoull(s, &se, 10), n = strtoull(t, &te, 10);
			if (m != n) {
				return m < n ? -1 : 1;
			}
			s = se;
			t = te;
			continue;
		}
		int c = tolower((unsigned char)*s) - tolower((unsigned char)*t);
		if (c) {
			return c;
		}
		s++;
		t++;
	}
	return *s ? 1 : *t ? -1 : 0;
}

static enum sort_key sort_by;
static bool sort_descending;

static int compare(const void *a, const void *b) {
	const struct file *x = a, *y = b;
	bool xf = x->kind == KIND_FOLDER, yf = y->kind == KIND_FOLDER;
	if (xf != yf) {
		return xf ? -1 : 1;
	}
	int c = 0;
	switch (sort_by) {
	case SORT_NAME:
		break;
	case SORT_DATE:
		c = x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0; // newest first
		break;
	case SORT_SIZE:
		c = x->size < y->size ? 1 : x->size > y->size ? -1 : 0; // biggest first
		break;
	case SORT_KIND:
		c = strcasecmp(x->kind_name, y->kind_name);
		break;
	}
	if (!c) {
		c = name_compare(x->name, y->name);
	}
	return sort_descending ? -c : c;
}

void folder_sort(struct folder *folder, enum sort_key key, bool descending) {
	sort_by = key;
	sort_descending = descending;
	qsort(folder->files, folder->len, sizeof(*folder->files), compare);
}

void format_size(const struct file *file, char *out, size_t size) {
	if (file->kind == KIND_FOLDER) {
		if (file->size < 0) {
			snprintf(out, size, "--");
		} else {
			snprintf(out, size, "%lld item%s", (long long)file->size, file->size == 1 ? "" : "s");
		}
		return;
	}
	// a thousand to a kilobyte, like the disk makers and Finder count
	double v = (double)file->size;
	const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
	int u = 0;
	while (v >= 1000 && u < 4) {
		v /= 1000;
		u++;
	}
	if (u == 0) {
		snprintf(out, size, "%lld byte%s", (long long)file->size, file->size == 1 ? "" : "s");
	} else {
		snprintf(out, size, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
	}
}

void format_date(time_t when, char *out, size_t size) {
	struct tm t, today;
	time_t now = time(NULL);
	localtime_r(&when, &t);
	localtime_r(&now, &today);
	time_t yesterday_at = now - 24 * 3600;
	struct tm y;
	localtime_r(&yesterday_at, &y);
	char hm[16];
	strftime(hm, sizeof(hm), "%H:%M", &t);
	if (t.tm_year == today.tm_year && t.tm_yday == today.tm_yday) {
		snprintf(out, size, "Today at %s", hm);
	} else if (t.tm_year == y.tm_year && t.tm_yday == y.tm_yday) {
		snprintf(out, size, "Yesterday at %s", hm);
	} else {
		char day[32];
		strftime(day, sizeof(day), "%e %b %Y", &t);
		snprintf(out, size, "%s at %s", day[0] == ' ' ? day + 1 : day, hm);
	}
}
