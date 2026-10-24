#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/priority.h"
#include "core/server.h"

static void usage(FILE *out, const char *name) {
	fprintf(out,
		"usage: %s [-c config] [-s command] [-g greeter] [-d] [-v]\n"
		"  -c config   the lua config, instead of ~/.config/flame/init.lua\n"
		"  -s command  run command once the compositor is up\n"
		"  -g greeter  the login screen: just greeter (flamegreet under greetd),\n"
		"              flame quits once it does\n"
		"  -d          debug logging\n"
		"  -v          print version\n",
		name);
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	const char *greeter_cmd = NULL;
	const char *config_path = NULL;
	enum wlr_log_importance verbosity = WLR_INFO;

	int opt;
	while ((opt = getopt(argc, argv, "c:s:g:dvh")) != -1) {
		switch (opt) {
		case 'c':
			config_path = optarg;
			break;
		case 's':
			startup_cmd = optarg;
			break;
		case 'g':
			greeter_cmd = optarg;
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
	char *error = NULL;
	struct flame_config *config = config_load(&server, config_path, &error);
	if (!config) {
		return 1;
	}
	if (error) {
		// a broken config doesn't stop flame: what loaded stays, the rest is
		// defaults, and the error is in the log
		wlr_log(WLR_ERROR, "config: %s", error);
		free(error);
	}
	server.config_path = config_path;
	if (!server_init(&server, config) || !server_start(&server, startup_cmd, greeter_cmd)) {
		return 1;
	}

	server_run(&server);
	server_finish(&server);
	config_free((struct flame_config *)server.config);
	return 0;
}
