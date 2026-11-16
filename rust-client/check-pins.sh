#!/usr/bin/env sh
# The client decodes the module's replies into the node's own types, and its
# generated layer is written by the builder's logos-lidl-gen. Both only hold
# while the Cargo manifests pin the revisions flake.lock pins:
#   - every logos-blockchain crate at the node revision the module is built with,
#   - logos-rust-sdk at the revision of the builder's logos-rust-sdk input.
# Exits 1 naming each manifest line that pins something else.
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
lock="$root/flake.lock"

command -v jq >/dev/null 2>&1 || { echo "error: jq is required" >&2; exit 255; }

node_rev="$(jq -er '.nodes[.nodes[.root].inputs["logos-blockchain"]].locked.rev' "$lock")"
sdk_rev="$(jq -er '
  .nodes[.nodes[.root].inputs["logos-module-builder"]].inputs["logos-rust-sdk"] as $key
  | .nodes[$key].locked.rev' "$lock")"

failed=0
check() { # check <manifest> <git url pattern> <expected rev> <what>
  manifest="$1"; pattern="$2"; expected="$3"; what="$4"
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    rev="$(printf '%s\n' "$line" | sed -n 's/.*rev *= *"\([^"]*\)".*/\1/p')"
    if [ "$rev" != "$expected" ]; then
      echo "error: ${manifest#"$root"/}: $what is pinned to '${rev:-<no rev>}', flake.lock has $expected" >&2
      echo "         $line" >&2
      failed=1
    fi
  done <<LINES
$(grep -E "$pattern" "$manifest" || true)
LINES
}

for manifest in \
  "$root/rust-client/Cargo.toml" \
  "$root/rust-client/example-cargo/Cargo.toml" \
  "$root/rust-client/example-module/rust-lib/Cargo.toml"; do
  check "$manifest" 'github\.com/logos-blockchain/logos-blockchain(\.git)?"' "$node_rev" "the node"
  check "$manifest" 'github\.com/logos-co/logos-rust-sdk(\.git)?"' "$sdk_rev" "logos-rust-sdk"
done

if ! grep -q "$node_rev" "$root/rust-client/Cargo.toml"; then
  echo "error: rust-client/Cargo.toml does not pin the node at all" >&2
  failed=1
fi

if [ "$failed" -ne 0 ]; then
  echo "Bump the manifests to those revisions, refresh the Cargo.lock files, and run 'nix run .#generate'." >&2
  exit 1
fi
echo "client pins match flake.lock (node $node_rev, logos-rust-sdk $sdk_rev)"
