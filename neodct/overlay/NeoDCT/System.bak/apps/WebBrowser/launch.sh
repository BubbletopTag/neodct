#!/bin/sh

echo "Starting WebBrowser"

sh /tests/neodct-browser/start.sh > /dev/ttyAMA0 2>&1 # LATER: make that if dev mode is turned on it will do that log.