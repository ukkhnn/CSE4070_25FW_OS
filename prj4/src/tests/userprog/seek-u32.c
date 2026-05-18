/* Invalid unsigned positions leave the last valid position unchanged. */

#include <limits.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void)
{
  int fd;
  char buffer[2];

  CHECK (create ("positions", 4), "create positions file");
  CHECK ((fd = open ("positions")) > 1, "open positions file");
  CHECK (write (fd, "abcd", 4) == 4, "write initial contents");
  seek (fd, 1);
  CHECK (tell (fd) == 1, "set ordinary position");

  msg ("seek to INT_MAX + 1");
  seek (fd, (unsigned) INT_MAX + 1);
  CHECK (tell (fd) == 1, "first invalid seek preserves position");
  msg ("seek to UINT_MAX");
  seek (fd, UINT_MAX);
  CHECK (tell (fd) == 1, "second invalid seek preserves position");

  seek (fd, INT_MAX);
  CHECK (tell (fd) == INT_MAX, "INT_MAX remains a valid position");
  seek (fd, 1);
  CHECK (read (fd, buffer, sizeof buffer) == sizeof buffer,
         "read after restoring ordinary position");
  CHECK (buffer[0] == 'b' && buffer[1] == 'c', "file contents are unchanged");
  close (fd);
}
