/* Unrepresentable file lengths must fail without panicking or creating files. */

#include <limits.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void)
{
  int fd;

  CHECK (!create ("above-max", (unsigned) INT_MAX + 1),
         "reject INT_MAX + 1 length");
  CHECK (open ("above-max") == -1, "rejected file does not exist");
  CHECK (!create ("unsigned-max", UINT_MAX), "reject UINT_MAX length");
  CHECK (open ("unsigned-max") == -1, "second rejected file does not exist");

  CHECK (create ("ordinary", 1), "create ordinary file after rejection");
  CHECK ((fd = open ("ordinary")) > 1, "open ordinary file");
  CHECK (filesize (fd) == 1, "ordinary file has requested length");
  close (fd);
}
