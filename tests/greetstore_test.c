// the login screen's look per person: what flamesettings writes as you,
// flamegreet reads back, and nothing it shouldn't believe. make test
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../tools/flamegreet/store.h"

static int failures;
static char root[256], me[256], mine[512];

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void put(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
}

static struct greet_look read_mine(void) {
	struct greet_look look;
	store_read(me, getuid(), &look);
	return look;
}

// nothing yet: the login screen's own wallpaper, blurred
static void test_defaults(void) {
	struct greet_look look = read_mine();
	CHECK(!look.wallpaper[0]);
	CHECK(look.blur);
}

static void test_written_read_back(void) {
	char picture[600];
	snprintf(picture, sizeof(picture), "%s/sea.jpg", root);
	put(picture, "not really a jpeg");
	const char *error = NULL;
	CHECK(store_set_wallpaper(picture, &error));
	CHECK(store_set_blur(false, &error));
	struct greet_look look = read_mine();
	CHECK(!strncmp(look.wallpaper, mine, strlen(mine)));
	CHECK(!look.blur);
	struct stat st;
	CHECK(stat(look.wallpaper, &st) == 0 && (st.st_mode & 0777) == 0644);
	CHECK(stat(mine, &st) == 0 && (st.st_mode & 0777) == 0755);
	// another one: a new name, the old one gone, the blur kept
	char first[4096];
	snprintf(first, sizeof(first), "%s", look.wallpaper);
	usleep(1000);
	CHECK(store_set_wallpaper(picture, &error));
	look = read_mine();
	CHECK(look.wallpaper[0] && strcmp(look.wallpaper, first));
	CHECK(access(first, F_OK) != 0);
	CHECK(!look.blur);
	// back to none
	CHECK(store_set_wallpaper(NULL, &error));
	look = read_mine();
	CHECK(!look.wallpaper[0]);
	CHECK(!look.blur);
}

// whose folder it isn't (a uid that isn't its owner's): none of it
static void test_not_theirs(void) {
	struct greet_look look;
	store_read(me, getuid() + 1, &look);
	CHECK(look.blur);
	CHECK(!look.wallpaper[0]);
}

// anyone could have written it (group or world writable): not believed
static void test_writable_by_others(void) {
	const char *error = NULL;
	CHECK(store_set_blur(false, &error));
	chmod(mine, 0775);
	CHECK(read_mine().blur);
	chmod(mine, 0755);
	CHECK(!read_mine().blur);
}

// the look pointing anywhere else, or a link where a picture should be
static void test_pictures_only_theirs(void) {
	char look[600], link[600];
	snprintf(look, sizeof(look), "%s/look", mine);
	put(look, "wallpaper = ../../etc/passwd\nblur = true\n");
	CHECK(!read_mine().wallpaper[0]);
	snprintf(link, sizeof(link), "%s/wallpaper-1", mine);
	CHECK(symlink("/etc/passwd", link) == 0);
	put(look, "wallpaper = wallpaper-1\nblur = true\n");
	CHECK(!read_mine().wallpaper[0]);
	unlink(link);
}

int main(void) {
	snprintf(root, sizeof(root), "/tmp/greetstore.XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}
	setenv("FLAMEGREET_DIR", root, 1);
	struct passwd *pw = getpwuid(getuid());
	snprintf(me, sizeof(me), "%s", pw ? pw->pw_name : "");
	snprintf(mine, sizeof(mine), "%s/%s", root, me);
	umask(077); // whatever the umask, the login screen can read it
	test_defaults();
	test_written_read_back();
	test_not_theirs();
	test_writable_by_others();
	test_pictures_only_theirs();
	char rm[600];
	snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
	if (system(rm) != 0) {
		failures++;
	}
	if (failures) {
		fprintf(stderr, "greetstore: %d failed\n", failures);
		return 1;
	}
	printf("greetstore: all good\n");
	return 0;
}
