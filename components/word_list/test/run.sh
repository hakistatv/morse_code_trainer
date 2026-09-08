#!/bin/sh
# Host build + run of the word_list unit test. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
cc -I.. -Wall -Wextra -std=c11 test_word_list.c ../word_list.c -o /tmp/test_word_list
exec /tmp/test_word_list
