# Standalone unit tests for the NeoDCT chrome logic.
# Run with:  make -C frontends/framebuffer/neodct/test
# No netsurf build system involvement; plain C99, no deps.

CC ?= cc
CFLAGS += -std=c99 -Wall -Wextra -Werror -g -I..

TESTS := test_url test_menu test_cursor test_ui test_status test_mem test_wrap \
	test_media

.PHONY: run clean
run: $(TESTS)
	@rc=0; for t in $(TESTS); do ./$$t || rc=1; done; \
	if [ $$rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi; \
	exit $$rc

test_url: test_url.c ../neodct_url.c ../neodct_url.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_url.c ../neodct_url.c

test_menu: test_menu.c ../neodct_menu.c ../neodct_menu.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_menu.c ../neodct_menu.c

test_cursor: test_cursor.c ../neodct_cursor.c ../neodct_cursor.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_cursor.c ../neodct_cursor.c

test_ui: test_ui.c ../neodct_ui.c ../neodct_ui.h ../neodct_url.c \
		../neodct_menu.c ../neodct_cursor.c
	$(CC) $(CFLAGS) -o $@ test_ui.c ../neodct_ui.c ../neodct_url.c \
		../neodct_menu.c ../neodct_cursor.c

test_status: test_status.c ../neodct_status.c ../neodct_status.h \
		../neodct_url.c test_util.h
	$(CC) $(CFLAGS) -o $@ test_status.c ../neodct_status.c ../neodct_url.c

test_mem: test_mem.c ../neodct_mem.c ../neodct_mem.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_mem.c ../neodct_mem.c

test_wrap: test_wrap.c ../neodct_wrap.c ../neodct_wrap.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_wrap.c ../neodct_wrap.c

test_media: test_media.c ../neodct_media.c ../neodct_media.h test_util.h
	$(CC) $(CFLAGS) -o $@ test_media.c ../neodct_media.c

clean:
	rm -f $(TESTS)
