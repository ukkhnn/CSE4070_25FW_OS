/* An unmapped destination is invalid even when the file is already at EOF. */

#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void)
{
  int fd;

  CHECK (create ("empty", 0), "create empty file");
  CHECK ((fd = open ("empty")) > 1, "open empty file");
  msg ("read at EOF into unmapped user memory");
  read (fd, (void *) 0x10000000, 1);
  fail ("should not have survived read()");
}
