#!/bin/sh
# just the window manager, barebones (none of the apps). on arch:
#   curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/install-core.sh | sh
# to update flame to the newest version: flame-update
set -eu
curl -fsSL https://raw.githubusercontent.com/haiderking1/flamewm/main/dist/flame-update \
	| sh -s -- --core
