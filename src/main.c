#include <stdio.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/priority.h"
#include "core/server.h"

static void usage(FILE *out, const char *name) {
	fprintf(out,
		"usage: %s [-s command] [-d] [-v]\n"
		"  -s command  run command once the compositor is up\n"
		"  -d          debug logging\n"
		"  -v          print version\n",
		name);
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	enum wlr_log_importance verbosity = WLR_INFO;

	int opt;
	while ((opt = getopt(argc, argv, "s:dvh")) != -1) {
		switch (opt) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			verbosity = WLR_DEBUG;
			break;
		case 'v':
			puts("flamewm " FLAME_VERSION);
			return 0;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		usage(stderr, argv[0]);
		return 1;
	}

	wlr_log_init(verbosity, NULL);
	priority_raise();

	// a failed init just exits, the kernel cleans up after us faster than
	// unwinding a half built server would
	struct flame_server server = {0};
	if (!server_init(&server, &flame_default_config)
			|| !server_start(&server, startup_cmd)) {
		return 1;
	}

	server_run(&server);
	server_finish(&server);
	return 0;
}
