#!/bin/sh
set -eu

root="$1"
path="$2"
type="$3"
major="$4"
minor="$5"
uid="${6:-}"
gid="${7:-}"
mode="${8:-}"

target="$root$path"
dir=$(dirname "$target")

mkdir -p "$dir"
rm -f "$target"
mknod "$target" "$type" "$major" "$minor"

if [ -n "$uid" ] && [ -n "$gid" ]; then
	if ! chown "$uid:$gid" "$target"; then
		# Some fakeroot builds record the fake owner but still return EINVAL.
		[ -n "${FAKEROOTKEY:-}" ] || exit 1
	fi
fi

if [ -n "$mode" ]; then
	chmod "$mode" "$target"
fi
