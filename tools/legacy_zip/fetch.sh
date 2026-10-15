#!/bin/sh
#
# Fetch the real ZIP members the shrink, reduce and implode methods are tested
# on.
#
# They are other projects' test files and are not committed: thirteen small
# archives from zip-rs/zip2 (MIT), apache/commons-compress (Apache-2.0) and
# adamhathcock/sharpcompress (MIT), each at a pinned commit, listed with its
# licence, size, SHA-256 and URL in tools/legacy_zip/MANIFEST. They are read
# here and never redistributed. `make test` never needs the network: the test
# that uses them reports a missing set and says how to fetch it, and fails
# instead of skipping when GHOTI_CORPUS_REQUIRED=1.
#
# Usage:  tools/legacy_zip/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/legacy_zip"
mkdir -p "$dest"

grep -v '^#' "$root/tools/legacy_zip/MANIFEST" | while IFS="	" read -r tag bytes sha licence project url; do
  out="$dest/$tag.zip"
  if [ -s "$out" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ]; then
    printf 'have    %s\n' "$tag"
    continue
  fi
  printf 'fetch   %s (%s, %s)\n' "$tag" "$licence" "$project"
  curl --fail --silent --show-error --location --output "$out.partial" "$url"
  got=$(sha256sum "$out.partial" | cut -d' ' -f1)
  if [ "$got" != "$sha" ]; then
    rm -f "$out.partial"
    echo "$tag: sha256 $got, expected $sha" >&2
    exit 1
  fi
  mv "$out.partial" "$out"
done
