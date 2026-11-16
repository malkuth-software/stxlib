B=/Volumes/Storage/stxlib_workers/sb1-bidi
ninja -C $B/rel stxlib_bidi_tests stxlib_bidi_bench > $B/rel.log 2>&1
echo "ninja status $?"
grep -E -B2 -A8 'error:|FAILED|undefined' $B/rel.log | grep -v 'warning: unused' | head -100
