#!/usr/bin/env bash
# Build the chimera-lite wasm PoC and serve it on http://localhost:${1:-8765}
set -euo pipefail
cd "$(dirname "$0")"

# vendor/ is gitignored: regenerate it, then re-apply the js/wasm patch
# (vitess uses syscall.SIGHUP, undefined on js/wasm; 1 is SIGHUP everywhere).
if [ ! -d vendor ]; then
  go mod vendor
fi
sed -i '' 's/syscall\.SIGHUP/syscall.Signal(0x1)/g' \
  vendor/github.com/dolthub/vitess/go/mysql/auth_server_static.go

GOOS=js GOARCH=wasm go build -tags gms_pure_go -ldflags="-s -w" -o main.wasm .
# wasm_exec.js moved from misc/wasm to lib/wasm in newer Go releases;
# install -m: the module-cache source is mode 444, plain cp can't overwrite it
install -m 0644 "$(go env GOROOT)/lib/wasm/wasm_exec.js" . 2>/dev/null \
  || install -m 0644 "$(go env GOROOT)/misc/wasm/wasm_exec.js" .
ls -lh main.wasm

exec python3 -m http.server "${1:-8765}"
