# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(read-eof-ptr) begin
(read-eof-ptr) create empty file
(read-eof-ptr) open empty file
(read-eof-ptr) read at EOF into unmapped user memory
read-eof-ptr: exit(-1)
EOF
pass;
