#!/bin/sh
# Build the test image once, then run each requested coursework suite.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
image=cse4070-pintos-test:ubuntu22
if [ "$#" -eq 0 ]; then set -- prj2 prj3 prj4; fi
for project do
  case "$project" in prj2|prj3|prj4) ;; *) echo "Usage: $0 [prj2|prj3|prj4 ...]" >&2; exit 2 ;; esac
done
docker image inspect "$image" >/dev/null 2>&1 || docker build -t "$image" "$repo_root/tools"
status=0
for project do
  results="$repo_root/test-results/$project"
  mkdir -p "$results"
  docker run --rm --network none \
    --mount "type=bind,src=$repo_root,dst=/input,readonly" \
    --mount "type=bind,src=$repo_root/tools/test-project.sh,dst=/test-project.sh,readonly" \
    --mount "type=bind,src=$results,dst=/results" \
    -e "TEST_JOBS=${TEST_JOBS:-2}" \
    "$image" sh /test-project.sh "$project" || status=1
done
exit "$status"
