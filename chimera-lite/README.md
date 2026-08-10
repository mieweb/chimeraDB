# chimera-lite

Exploration of an embeddable "chimera at the edge" (mobile / browser) — rationale,
findings, and open questions live in [../mobile_idea.md](../mobile_idea.md).

- `wasm-poc/` — go-mysql-server compiled to `js/wasm`: an interactive SQL REPL in the
  browser (↑/↓ history kept across reloads) with `todos` persisted to OPFS.
  `./wasm-poc/serve.sh` builds (vendoring + patching deps as needed) and serves on
  http://localhost:8765.
