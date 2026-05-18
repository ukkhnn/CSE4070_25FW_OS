# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(create-u32) begin
(create-u32) reject INT_MAX + 1 length
(create-u32) rejected file does not exist
(create-u32) reject UINT_MAX length
(create-u32) second rejected file does not exist
(create-u32) create ordinary file after rejection
(create-u32) open ordinary file
(create-u32) ordinary file has requested length
(create-u32) end
create-u32: exit(0)
EOF
pass;
