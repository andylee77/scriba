#!/bin/bash
# Build and test every commit in FROM..BRANCH on its own, in a fresh clone:
# the CLI (warnings counted), test.sh, and the WASM build when an Emscripten
# SDK is available. Each commit of a pull request should pass on its own.
#
# Usage: dev/scripts/verify_branch.sh [BRANCH] [FROM]
#   BRANCH  defaults to the current branch
#   FROM    defaults to origin/master
#   EMSDK_DIR (environment) points at an emsdk checkout, default ~/emsdk;
#   without one the web build is skipped.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
BR=${1:-$(git -C "$REPO" rev-parse --abbrev-ref HEAD)}
FROM=${2:-origin/master}
EMSDK_DIR=${EMSDK_DIR:-$HOME/emsdk}

commits=$(git -C "$REPO" rev-list --reverse "$FROM..$BR") || exit 1
[ -n "$commits" ] || { echo "no commits in $FROM..$BR"; exit 0; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
git clone -q "$REPO" "$W/scriba"
cd "$W/scriba" || exit 1

web=0
if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
	# shellcheck disable=SC1091
	source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1 && web=1
fi
[ $web = 1 ] || echo "(no emsdk at $EMSDK_DIR: skipping the web build)"

for c in $commits; do
	git checkout -q "$c"
	subj=$(git log -1 --format=%s)
	make clean >/dev/null 2>&1
	if make LIBS=-lusb-1.0 > "$W/cli.log" 2>&1; then
		cli="cli ok, $(grep -c 'warning:' "$W/cli.log") warnings"
	else
		cli="CLI FAILED"; tail -15 "$W/cli.log"
	fi
	tests=$(./test.sh 2>&1 | tail -1)
	webres="web skipped"
	if [ $web = 1 ]; then
		rm -rf web/public/wasm web/dist
		if make -C web EMSDK_DIR="$EMSDK_DIR" > "$W/web.log" 2>&1; then
			webres="web ok, $(grep -cE 'warning:|error:' "$W/web.log") warnings"
		else
			webres="WEB FAILED"; tail -20 "$W/web.log"
		fi
	fi
	echo "$(git log -1 --format=%h) | $cli | $tests | $webres | $subj"
done
