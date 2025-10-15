#!/bin/bash
if [ -z "${1:-}" ] || [ -z "${2:-}" ] || [ -z "${3:-}" ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: upload-fw.sh target path/to/firmware.cab path/to/lvfs-creds/netrc"
    exit 2
fi

set -uo pipefail

RESULT=$(curl --fail-with-body \
    --netrc-file "$3" \
    -X POST \
    -F target="$1" \
    -F file=@"$2" \
    https://fwupd.org/lvfs/upload/token)
RC=$?
if [ $RC != 0 ]; then
    echo "Uploading firmware failed with exit code $RC"
    exit $RC
fi

if ! jq -e '.success == true' <<< "$RESULT" ; then
    echo "Uploading firmware failed, remote said:"
    echo "$RESULT"
    exit 1
else
    echo "$RESULT"
fi
