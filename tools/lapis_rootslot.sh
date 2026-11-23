#!/system/bin/sh
# Put a program or a library into the iOS root from the phone itself (rooted
# Android, no PC, no mount): over the blocks of an iOS file lapis does not
# use, found with tools/hfsplus_find.py in build/iosroot_tweaked.img. The file
# keeps its size and path; a program or dylib needs nothing else (the rest is
# zeros). Writes only over the original blocks (sha checked) or over what this
# script put there earlier (Mach-O, with the original kept), and keeps the
# original in Download for restore.
#
#   su -c 'sh /sdcard/Download/lapis_rootslot.sh install SLOT FILE'
#   su -c 'sh /sdcard/Download/lapis_rootslot.sh restore SLOT'
#   su -c 'sh /sdcard/Download/lapis_rootslot.sh status'
#
# Slots (FILE must be exactly COUNT x 4096 bytes, tools/lapis_rootslot_blocks.py):
#   fdr   /usr/bin/IOMFB_FDR_Loader, run once per boot as root
#         (com.apple.iomfb_fdr_loader: factory display calibration, absent here)
#   bbfw  /usr/standalone/update/ICE17.Release.bbfw (an Intel modem's update
#         firmware; lapis has no such modem): room for a 20 MB dylib
DEV=/dev/block/by-name/iosroot
[ -e "$DEV" ] || DEV=/dev/block/sdc92
DIR=/sdcard/Download

slot() {
	case "$1" in
	fdr)  START=2094775; COUNT=36;   ORIG=abf677d9fd40ed2faba2cca784ec8d16484a0ca94ac3e9b29fdbf40c8ab7212d; BACKUP=$DIR/iomfb_fdr_loader_orig.blocks ;;
	bbfw) START=2192798; COUNT=4912; ORIG=012fc192313009320d91aafec04408542e02f82457179f6a22657ae6e526b50b; BACKUP=$DIR/ice17_bbfw_orig.blocks ;;
	*) echo "no slot $1 (fdr, bbfw)"; exit 1 ;;
	esac
}
cur() { dd if="$DEV" bs=4096 skip=$START count=$COUNT 2>/dev/null | sha256sum | cut -c1-64; }
magic() { dd if="$DEV" bs=4096 skip=$START count=1 2>/dev/null | od -An -tx4 -N4 | tr -d ' '; }

case "$1" in
install)
	slot "$2"
	F=$3
	[ -f "$F" ] && [ "$(stat -c %s "$F")" = $((COUNT * 4096)) ] || { echo "$F: not $((COUNT * 4096)) bytes"; exit 1; }
	WANT=$(sha256sum "$F" | cut -c1-64)
	now=$(cur)
	[ "$now" = "$WANT" ] && { echo "$2: already installed"; exit 0; }
	if [ "$now" = "$ORIG" ]; then
		dd if="$DEV" bs=4096 skip=$START count=$COUNT of="$BACKUP" 2>/dev/null
		[ "$(sha256sum "$BACKUP" | cut -c1-64)" = "$ORIG" ] || { echo "backup failed: nothing written"; exit 1; }
	elif [ "$(sha256sum "$BACKUP" 2>/dev/null | cut -c1-64)" != "$ORIG" ] || [ "$(magic)" != feedfacf ]; then
		echo "$2 holds neither the original nor something installed here ($now): nothing written"; exit 1
	fi
	dd if="$F" of="$DEV" bs=4096 seek=$START count=$COUNT conv=notrunc 2>/dev/null; sync
	[ "$(cur)" = "$WANT" ] && echo "$2: installed (original kept in $BACKUP)" || { echo "$2: verify FAILED"; exit 1; }
	;;
restore)
	slot "$2"
	[ "$(cur)" = "$ORIG" ] && { echo "$2: already original"; exit 0; }
	[ "$(sha256sum "$BACKUP" | cut -c1-64)" = "$ORIG" ] || { echo "no good backup at $BACKUP"; exit 1; }
	dd if="$BACKUP" of="$DEV" bs=4096 seek=$START count=$COUNT conv=notrunc 2>/dev/null; sync
	[ "$(cur)" = "$ORIG" ] && echo "$2: restored" || { echo "$2: verify FAILED"; exit 1; }
	;;
*)
	for s in fdr bbfw; do
		slot $s
		now=$(cur)
		[ "$now" = "$ORIG" ] && st=original || st="replaced ($(magic))"
		echo "$s: $st"
	done
	;;
esac
