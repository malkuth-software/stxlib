B=/Volumes/Storage/stxlib_workers/sb1-bidi
D=/Volumes/Storage/stxlib_workers/unicode-18.0.0
ninja -C $B/rel stxlib_bidi_tests > $B/q.log 2>&1 || { grep -A5 'error:' $B/q.log | head -40; exit 1; }
$B/rel/bin/stxlib_bidi_tests third_party/unicode-18.0.0 $D --exhaustive 4 2>&1 | tail -40
