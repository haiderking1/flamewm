#!/bin/sh
# flame with all its apps (the bar, files, settings, the player, search, the
# lock and login screens), on arch:
#   curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install.sh | sh
# later, flame-update gets the newest
set -eu
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/flame-update \
	| sh -s -- --all
