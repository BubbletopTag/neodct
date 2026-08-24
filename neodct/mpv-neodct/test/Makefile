# Standalone unit tests for the NeoDCT mpv additions.
# Run with:  make -C neodct/mpv-neodct/test
# No mpv build system involvement; plain C99, no deps.

CC ?= cc
CFLAGS += -std=c99 -Wall -Wextra -Werror -g -I..

TESTS := test_fbdev_format

.PHONY: run clean
run: $(TESTS)
	@rc=0; for t in $(TESTS); do ./$$t || rc=1; done; \
	if [ $$rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi; \
	exit $$rc

test_fbdev_format: test_fbdev_format.c ../fbdev_format.c ../fbdev_format.h \
		test_util.h
	$(CC) $(CFLAGS) -o $@ test_fbdev_format.c ../fbdev_format.c

clean:
	rm -f $(TESTS)
