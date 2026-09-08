#!/bin/sh
# Host build + run of the morse_tree unit test. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
cc -I.. -Wall -Wextra -std=c11 test_morse_tree.c ../morse_tree.c -o /tmp/test_morse_tree
exec /tmp/test_morse_tree
