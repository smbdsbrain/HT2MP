#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
listen="${HT2MP_LISTEN:-0.0.0.0:28020}"
if [[ $# -gt 1 ]]; then
  echo "usage: ./start-server.sh [listen-address:port]" >&2
  exit 2
fi
if [[ $# -eq 1 ]]; then
  listen="$1"
fi

server="$script_dir/bin/ht2mp-coordinator"
if [[ ! -x "$server" ]]; then
  server="$script_dir/ht2mp-coordinator"
fi

echo "HT2MP server"
echo "Endpoint: $listen"
echo "Profile:  steam-8138acee"
echo
echo "A new access token will be printed below. Send it only to players you trust."
echo "Keep this terminal open. Press Ctrl+C to stop the server."
echo

exec "$server" --listen "$listen" --profile steam-8138acee
