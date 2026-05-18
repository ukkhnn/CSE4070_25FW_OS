# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(seek-u32) begin
(seek-u32) create positions file
(seek-u32) open positions file
(seek-u32) write initial contents
(seek-u32) set ordinary position
(seek-u32) seek to INT_MAX + 1
(seek-u32) first invalid seek preserves position
(seek-u32) seek to UINT_MAX
(seek-u32) second invalid seek preserves position
(seek-u32) INT_MAX remains a valid position
(seek-u32) read after restoring ordinary position
(seek-u32) file contents are unchanged
(seek-u32) end
seek-u32: exit(0)
EOF
pass;
