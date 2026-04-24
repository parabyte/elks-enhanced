#!/usr/bin/env bash
set -euo pipefail

export PATH="/sbin:/usr/sbin:$PATH"

usage() {
	echo "Usage: $0 /dev/sdX"
	echo "Installs ELKS onto a hard disk using a bootable 2 GiB root partition"
	echo "plus five additional 2 GiB ext2 partitions."
	exit 1
}

die() {
	echo "$*" >&2
	exit 1
}

as_root() {
	if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
		"$@"
	elif [[ -n "${ELKS_SUDO_NONINTERACTIVE:-}" || ! -t 0 ]]; then
		sudo -n "$@"
	else
		sudo "$@"
	fi
}

refresh_sudo() {
	if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
		return 0
	elif [[ -n "${ELKS_SUDO_NONINTERACTIVE:-}" || ! -t 0 ]]; then
		sudo -n true
	else
		sudo -v
	fi
}

wait_for_blockdev() {
	local dev="$1"
	local tries
	for tries in $(seq 1 50); do
		[[ -b "$dev" ]] && return 0
		sleep 0.1
	done
	die "Partition device not found: $dev"
}

run_e2fsck() {
	local rc
	set +e
	as_root /sbin/e2fsck -fy "$1"
	rc=$?
	set -e
	case "$rc" in
	0|1) return 0 ;;
	*) return "$rc" ;;
	esac
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

partdev() {
	local dev="$1"
	local part="$2"
	if [[ "$dev" =~ [0-9]$ ]]; then
		printf '%sp%s\n' "$dev" "$part"
	else
		printf '%s%s\n' "$dev" "$part"
	fi
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ELKS_ROOT="$SCRIPT_DIR"
TARGET_DEV="${1:-}"

[[ -n "$TARGET_DEV" ]] || usage
[[ $# -eq 1 ]] || usage
[[ -b "$TARGET_DEV" ]] || die "Not a block device: $TARGET_DEV"

need_cmd sudo
need_cmd dd
need_cmd lsblk
need_cmd wipefs
need_cmd resize2fs
need_cmd e2fsck
need_cmd fdisk
need_cmd blkid
need_cmd partprobe
need_cmd blockdev
need_cmd partx

SRC_IMG="$ELKS_ROOT/image/hd.img"
SETBOOT="$ELKS_ROOT/elks/tools/bin/setboot"
MBR="$ELKS_ROOT/bootblocks/mbr.bin"
VBR="$ELKS_ROOT/bootblocks/ext2.bin"
MOUNT_CFG="$ELKS_ROOT/elkscmd/rootfs_template/etc/mount.cfg"

[[ -f "$SRC_IMG" ]] || die "Missing source image: $SRC_IMG"
[[ -x "$SETBOOT" ]] || die "Missing setboot tool: $SETBOOT"
[[ -f "$MBR" ]] || die "Missing MBR boot block: $MBR"
[[ -f "$VBR" ]] || die "Missing ext2 boot block: $VBR"
[[ -f "$MOUNT_CFG" ]] || die "Missing mount config: $MOUNT_CFG"

MIN_END_SECTOR=25174015
MIN_SECTORS=$((MIN_END_SECTOR + 1))
SECTORS_PER_TRACK=63
HEADS=16
ROOT_PART_OFFSET_HEX=0x1f3

DISK_SECTORS=$(as_root blockdev --getsz "$TARGET_DEV")
(( DISK_SECTORS >= MIN_SECTORS )) || die "Disk is too small: need at least $MIN_SECTORS sectors"

echo "Target device: $TARGET_DEV"
lsblk -o NAME,SIZE,MODEL,FSTYPE,MOUNTPOINT "$TARGET_DEV"
echo
echo "This will destroy all data on $TARGET_DEV."
printf 'Type YES to continue: '
read -r confirm
[[ "$confirm" == "YES" ]] || die "Aborted."

refresh_sudo

WORKDIR=$(mktemp -d /tmp/elks-install-hd.XXXXXX)
TARGET_MNT="$WORKDIR/target"
FDISK_SCRIPT="$WORKDIR/fdisk.script"
mkdir -p "$TARGET_MNT"

cleanup() {
	set +e
	if mountpoint -q "$TARGET_MNT"; then
		as_root umount "$TARGET_MNT"
	fi
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

P1=$(partdev "$TARGET_DEV" 1)
P2=$(partdev "$TARGET_DEV" 2)
P3=$(partdev "$TARGET_DEV" 3)
P5=$(partdev "$TARGET_DEV" 5)
P6=$(partdev "$TARGET_DEV" 6)
P7=$(partdev "$TARGET_DEV" 7)

cat > "$FDISK_SCRIPT" <<'EOF'
o
n
p
1
2048
+2G
a
n
p
2
4196352
+2G
n
p
3
8390656
+2G
n
e
12584960
25174015
n
12587008
+2G
n
16783360
+2G
n
20979712
25174015
w
EOF

echo "Partitioning $TARGET_DEV..."
as_root umount "${TARGET_DEV}"* 2>/dev/null || true
as_root wipefs -a "$TARGET_DEV" >/dev/null 2>&1 || true
as_root dd if=/dev/zero of="$TARGET_DEV" bs=1M count=1 conv=fsync status=none
as_root /sbin/fdisk -H "$HEADS" -S "$SECTORS_PER_TRACK" "$TARGET_DEV" < "$FDISK_SCRIPT"
as_root partprobe "$TARGET_DEV" || true
as_root partx -u "$TARGET_DEV" || true
as_root blockdev --rereadpt "$TARGET_DEV" || true
sleep 2

for part in "$P1" "$P2" "$P3" "$P5" "$P6" "$P7"; do
	wait_for_blockdev "$part"
done

echo "Formatting data partitions..."
for part in "$P1" "$P2" "$P3" "$P5" "$P6" "$P7"; do
	as_root wipefs -a "$part" >/dev/null 2>&1 || true
done
as_root /sbin/mkfs.ext2 -F -E revision=0 -b 1024 -m 0 -L ELKS2 "$P2"
as_root /sbin/mkfs.ext2 -F -E revision=0 -b 1024 -m 0 -L ELKS3 "$P3"
as_root /sbin/mkfs.ext2 -F -E revision=0 -b 1024 -m 0 -L ELKS5 "$P5"
as_root /sbin/mkfs.ext2 -F -E revision=0 -b 1024 -m 0 -L ELKS6 "$P6"
as_root /sbin/mkfs.ext2 -F -E revision=0 -b 1024 -m 0 -L ELKS7 "$P7"

echo "Installing ELKS root filesystem..."
as_root dd if="$SRC_IMG" of="$P1" bs=1M conv=fsync status=progress
run_e2fsck "$P1"
as_root resize2fs "$P1"
run_e2fsck "$P1"

echo "Updating root filesystem config..."
as_root mount "$P1" "$TARGET_MNT"
as_root cp "$MOUNT_CFG" "$TARGET_MNT/etc/mount.cfg"
as_root sync
as_root umount "$TARGET_MNT"

echo "Installing boot blocks..."
wait_for_blockdev "$P1"
as_root dd if="$MBR" of="$TARGET_DEV" bs=446 count=1 conv=notrunc status=none
as_root "$SETBOOT" "$P1" -B"$SECTORS_PER_TRACK","$HEADS" "$VBR"
printf '\000\010\000\000' | as_root dd of="$P1" bs=1 seek=$((ROOT_PART_OFFSET_HEX)) conv=notrunc status=none
as_root sync
run_e2fsck "$P1"

echo
echo "Install complete."
as_root /sbin/fdisk -H "$HEADS" -S "$SECTORS_PER_TRACK" \
	-o Device,Start,End,Sectors,Id,Boot,Start-C/H/S,End-C/H/S -l "$TARGET_DEV"
as_root blkid "$TARGET_DEV" "$P1" "$P2" "$P3" "$P5" "$P6" "$P7" || true
