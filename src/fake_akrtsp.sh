#!/bin/sh
# A "server" that always fails to start, for testing run_akrtsp.sh's failure and
# fallback path without touching the real capture pipeline: the supervisor counts
# it as a failed start every time.
exit 1
