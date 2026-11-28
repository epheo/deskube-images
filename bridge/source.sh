#!/usr/bin/env bash
# Gathers the corresponding source of the pinned moonlight-web-stream
# release in one archive, and the license files of all it is made of.
# source.sh <pins> <archive directory> <notices directory>
# The image is upstream's binary, so this is the source its tag names:
# nothing here proves the binary was built from it.
set -euo pipefail
PINS=$1; OUT=$2; NOTICES=$3
read -r _ TAG COMMIT REPO < <(grep '^tag ' "$PINS")
NAME=moonlight-web-stream-$TAG-source
mkdir -p "$OUT" "$NOTICES" "/tmp/$NAME"
cd "/tmp/$NAME"

# A commit, not a tag or a branch: neither can then move under the pin.
# The history stays behind: the source is the tree.
clone() {
  git init -q "$2"
  git -C "$2" fetch -q --depth 1 "$1" "$3"
  git -C "$2" checkout -q FETCH_HEAD
  git -C "$2" submodule update -q --init --recursive --depth 1
  # shellcheck disable=SC2016 # git expands these in each submodule
  { echo "$2 $1 $(git -C "$2" rev-parse HEAD)"
    git -C "$2" submodule foreach -q --recursive 'echo "$displaypath $(git config remote.origin.url) $sha1"' | sed "s|^|$2/|"
  } >> COMMITS
  find "$2" -name .git -prune -exec rm -rf {} +
}
clone "$REPO" moonlight-web-stream "$COMMIT"
# The web client's WebAssembly is built from this second repository, at
# the commit the first names. Cargo vendors only the one crate of it
# the server links.
COMMON=$(sed -n 's/^ *repo: *//p' moonlight-web-stream/ubrn.config.yaml)
clone "$COMMON" moonlight-common-rust "$(sed -n 's/^ *branch: *//p' moonlight-web-stream/ubrn.config.yaml)"

# Both lock files: the server's crates and the WebAssembly's.
# The config makes cargo build from vendor/ in both trees. With the
# network off cargo must still resolve each lock file, or a crate is missing.
mkdir .cargo
# The config is cargo's standard output, which -q would silence too.
cargo vendor --locked --versioned-dirs --manifest-path moonlight-web-stream/Cargo.toml \
  --sync moonlight-common-rust/Cargo.toml vendor > .cargo/config.toml 2> /tmp/vendor.log \
  || { cat /tmp/vendor.log >&2; exit 1; }
for tree in moonlight-web-stream moonlight-common-rust; do
  CARGO_HOME=/tmp/empty-cargo-home cargo metadata --offline --locked --format-version 1 \
    --manifest-path "$tree/Cargo.toml" > /dev/null
done

# The registry's own archives, checked as npm checks them. No npm runs:
# an install would run the packages' scripts.
jq -r '.packages | to_entries[] | select(.key != "") | .value | "\(.resolved) \(.integrity)"' \
  moonlight-web-stream/package-lock.json | sort -u > npm.list
cut -d' ' -f1 npm.list | sed 's|^https://registry.npmjs.org/\(.*\)$|url = "&"\noutput = "npm/\1"|' \
  | curl -fsSL --create-dirs --parallel --parallel-max 8 --retry 5 -K -
while read -r url sri; do
  file=npm/${url#https://registry.npmjs.org/}
  want=$(printf %s "${sri#sha512-}" | base64 -d | od -An -vtx1 | tr -d ' \n')
  [ "$(sha512sum < "$file" | cut -d' ' -f1)" = "$want" ] \
    || { echo "$url: not downloaded, or not the sha512 of package-lock.json" >&2; exit 1; }
done < npm.list
rm npm.list

cat > README <<EOF
The source of moonlight-web-stream $TAG, for the deskube-bridge image.
That image holds upstream's own release archive, unmodified.

moonlight-web-stream/    upstream at the tag's commit
moonlight-common-rust/   the Moonlight protocol crate, and the source of the web client's WebAssembly
vendor/                  the Rust crates of both Cargo.lock files, as cargo vendor lays them out
.cargo/config.toml       makes cargo build both trees from vendor/, with no network
npm/                     the npm packages of package-lock.json, as the registry serves them
COMMITS                  the commit of each repository and submodule

How upstream builds its release: moonlight-web-stream/README.md, "Building",
and moonlight-web-stream/.github/workflows/ci.yml.
EOF

# A notice must travel with the binary, so the license files go into the
# bridge image too. All crates and packages, not only those linked or
# bundled: more than needed, and no way to miss one.
notices() {
  mkdir -p "$2"
  find "$1" -mindepth 1 -maxdepth 1 \( -iname 'licen[cs]e*' -o -iname 'copying*' -o -iname 'copyright*' \
    -o -iname 'notice*' -o -iname 'unlicense*' -o -iname 'patents*' \) -exec cp -r {} "$2/" \;
}
for crate in vendor/*/; do
  notices "$crate" "$NOTICES/crates/$(basename "$crate")"
  echo "crate $(basename "$crate"): $(sed -n 's/^license\(-file\)\{0,1\} = "\(.*\)"$/\2/p' "$crate/Cargo.toml" | head -1)"
done > "$NOTICES/INDEX"
find npm -name '*.tgz' | sort | while read -r tgz; do
  mkdir /tmp/package && tar -xzf "$tgz" -C /tmp/package --strip-components=1
  id=$(jq -r '"\(.name)-\(.version)"' /tmp/package/package.json | tr / _)
  notices /tmp/package "$NOTICES/npm/$id"
  echo "npm $id: $(jq -r '.license // .licenses // "see its files" | tostring' /tmp/package/package.json)"
  rm -rf /tmp/package
done >> "$NOTICES/INDEX"
# Upstream links Rust's standard library and musl statically. These are
# Fedora's copies of their license texts: the same terms, not the same builds.
notices /usr/share/licenses/rust "$NOTICES/rust"
notices /usr/share/licenses/musl-libc "$NOTICES/musl"
{ echo "rust: the standard library in the binary and in the WebAssembly, MIT OR Apache-2.0"
  echo "musl: the C library in the binary, MIT"; } >> "$NOTICES/INDEX"
cp COMMITS "$NOTICES/COMMITS"
cat > "$NOTICES/README" <<EOF
License files of what moonlight-web-stream $TAG is built from:
the Rust crates of its lock files, the npm packages of its package-lock.json,
Rust's standard library and musl.
More than the binary links and the web client bundles: every crate and package, for every platform.
INDEX names the license of each.
A crate or package with no files here ships none.
The text of its license is at https://spdx.org/licenses/, under the name INDEX gives.
COMMITS names the commit of each repository the source archive holds.
EOF

cd /tmp && tar -cf - "$NAME" | zstd -q -T0 -12 -o "$OUT/$NAME.tar.zst"
