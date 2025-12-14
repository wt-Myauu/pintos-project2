# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(buddy) begin
(buddy) Allocated A at index 0
(buddy) Allocated B at index 4
(buddy) end
EOF
pass;

