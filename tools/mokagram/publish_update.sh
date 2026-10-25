#!/usr/bin/env bash
# Usage: tools/mokagram/publish_update.sh [path/to/Telegram] ["notes"]
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
binary=${1:-$root/../build/bin/Telegram}
notes=${2:-}
key=${MOKAGRAM_UPDATE_KEY:-$HOME/.config/mokagram/update-key.pem}
remote=${MOKAGRAM_UPDATE_REMOTE:-git@github.com:ecronxy/mokagram.git}
branch=updates
platform=linux-x64
file=mokagram-$platform.xz
header=$root/Telegram/SourceFiles/mokagram/update/mokagram_update.h

version=$(sed -n 's/^constexpr auto kVersion = \([0-9]*\);$/\1/p' "$header")
title=$(sed -n 's/^constexpr auto kVersionTitle = "\(.*\)";$/\1/p' "$header")
if [[ -z "$version" || -z "$title" ]]; then
	echo "Не нашёл kVersion и kVersionTitle в $header" >&2
	exit 1
fi
if [[ ! -f "$binary" ]]; then
	echo "Нет сборки: $binary" >&2
	exit 1
fi
if [[ ! -f "$key" ]]; then
	echo "Нет ключа подписи: $key" >&2
	exit 1
fi
mode=$(stat -c %a "$key")
if [[ "$mode" != "600" && "$mode" != "400" ]]; then
	echo "Права на ключ $key: $mode. Нужны 600 (chmod 600 $key)." >&2
	exit 1
fi

published=$(curl -fsSL "https://raw.githubusercontent.com/ecronxy/mokagram/$branch/update.json" 2>/dev/null \
	| python3 -c 'import json, sys; print(json.load(sys.stdin)["version"])' 2>/dev/null || echo 0)
if (( version <= published )); then
	echo "В ветке $branch уже версия $published. Увеличьте kVersion в $header и пересоберите." >&2
	exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/repo"

echo "Упаковываю $binary..."
strip -o "$work/binary" "$binary"
xz -T0 -9 -c "$work/binary" > "$work/repo/$file"
size=$(stat -c %s "$work/repo/$file")
sha256=$(sha256sum "$work/repo/$file" | cut -d' ' -f1)

printf 'mokagram-update\n%s\n%s\n%s\n%s\n' "$platform" "$version" "$sha256" "$size" > "$work/message"
openssl pkeyutl -sign -rawin -inkey "$key" -in "$work/message" -out "$work/signature"
signature=$(base64 -w0 "$work/signature")

VERSION=$version TITLE=$title NOTES=$notes PLATFORM=$platform FILE=$file SIZE=$size \
SHA256=$sha256 SIGNATURE=$signature python3 - > "$work/repo/update.json" <<'EOF'
import json, os
e = os.environ
print(json.dumps({
    "version": int(e["VERSION"]),
    "title": e["TITLE"],
    "notes": e["NOTES"],
    e["PLATFORM"]: {
        "file": e["FILE"],
        "size": int(e["SIZE"]),
        "sha256": e["SHA256"],
        "signature": e["SIGNATURE"],
    },
}, ensure_ascii=False, indent=2))
EOF

cd "$work/repo"
git init -q -b "$branch"
git add update.json "$file"
git commit -q -m "mokaGram $title"
echo "Публикую mokaGram $title ($size байт) в $remote, ветка $branch..."
git push --force "$remote" "$branch:$branch"
echo "Готово."
