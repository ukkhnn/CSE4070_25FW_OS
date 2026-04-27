# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(priority-donate-handoff) begin
(priority-donate-handoff) New holder should have priority 32.  Actual priority: 32.
(priority-donate-handoff) Reacquiring holder should have priority 33.  Actual priority: 33.
(priority-donate-handoff) New holder should have priority 32.  Actual priority: 32.
(priority-donate-handoff) end
EOF
pass;
