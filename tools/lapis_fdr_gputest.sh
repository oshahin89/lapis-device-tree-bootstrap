#!/system/bin/sh
# Put lapis_gputest into the iOS root from the phone itself (rooted Android,
# no PC, no mount): its 36 blocks go over /usr/bin/IOMFB_FDR_Loader, a job
# iOS runs once at every boot as root (com.apple.iomfb_fdr_loader: Apple's
# factory display calibration, absent on lapis). The file keeps its size;
# the rest after the program is zeros. Blocks found with tools/hfsplus_find.py
# in build/iosroot_tweaked.img; the script writes only if the phone holds
# exactly those original blocks, and keeps them for restore.
#
#   su -c 'sh /sdcard/Download/lapis_fdr_gputest.sh install'
#   su -c 'sh /sdcard/Download/lapis_fdr_gputest.sh restore'
#
# install expects /sdcard/Download/lapis_gputest_fdr.blocks (147456 bytes).
set -e
DEV=/dev/block/by-name/iosroot
[ -e "$DEV" ] || DEV=/dev/block/sdc92
START=2094775
COUNT=36
ORIG=abf677d9fd40ed2faba2cca784ec8d16484a0ca94ac3e9b29fdbf40c8ab7212d
NEW=46777034c96f1380c1013899835125f7d4b383765d27642b6af92ea3e8241eaa
DIR=/sdcard/Download
BACKUP=$DIR/iomfb_fdr_loader_orig.blocks

cur() { dd if="$DEV" bs=4096 skip=$START count=$COUNT 2>/dev/null | sha256sum | cut -c1-64; }
put() { dd if="$1" of="$DEV" bs=4096 seek=$START count=$COUNT conv=notrunc 2>/dev/null; sync; }

now=$(cur)
case "$1" in
install)
	[ "$now" = "$NEW" ] && { echo "already installed"; exit 0; }
	[ "$(sha256sum "$DIR/lapis_gputest_fdr.blocks" | cut -c1-64)" = "$NEW" ] || { echo "lapis_gputest_fdr.blocks is not the right file"; exit 1; }
	if [ "$now" = "$ORIG" ]; then
		dd if="$DEV" bs=4096 skip=$START count=$COUNT of="$BACKUP" 2>/dev/null
		[ "$(sha256sum "$BACKUP" | cut -c1-64)" = "$ORIG" ] || { echo "backup failed: nothing written"; exit 1; }
	elif [ "$(sha256sum "$BACKUP" 2>/dev/null | cut -c1-64)" != "$ORIG" ] || \
	     [ "$(dd if="$DEV" bs=4096 skip=$START count=1 2>/dev/null | od -An -tx4 -N4 | tr -d ' ')" != feedfacf ]; then
		echo "the blocks hold neither the original nor an earlier lapis_gputest ($now): nothing written"; exit 1
	fi
	put "$DIR/lapis_gputest_fdr.blocks"
	[ "$(cur)" = "$NEW" ] && echo "installed (original kept in $BACKUP)" || { echo "verify FAILED"; exit 1; }
	;;
restore)
	[ "$now" = "$ORIG" ] && { echo "already original"; exit 0; }
	[ "$(sha256sum "$BACKUP" | cut -c1-64)" = "$ORIG" ] || { echo "no good backup at $BACKUP"; exit 1; }
	put "$BACKUP"
	[ "$(cur)" = "$ORIG" ] && echo "restored" || { echo "verify FAILED"; exit 1; }
	;;
*)
	echo "usage: $0 install|restore (now: $([ "$now" = "$ORIG" ] && echo original || ([ "$now" = "$NEW" ] && echo installed || echo unknown)))"
	;;
esac
