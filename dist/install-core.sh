#!/bin/sh
# flame alone, no apps: bring your own bar, launcher and locker. on arch:
#   curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install-core.sh | sh
# later, flame-update gets the newest
set -eu
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/flame-update \
	| sh -s -- --core
