CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
PKGS = gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gio-2.0
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

.PHONY: all clean check install uninstall

all: gtk-pipe

gtk-pipe: gtk-pipe.o secure.o secure-protocol.o pin-buffer.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

gtk-pipe.o: gtk-pipe.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

pin-buffer.o: pin-buffer.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

secure.o: secure.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

secure-protocol.o: secure-protocol.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/test_protocol: tests/test_protocol.c secure-protocol.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_protocol.c secure-protocol.c $(LDLIBS)

check: gtk-pipe tests/test_protocol
	./gtk-pipe --help
	./tests/test_protocol
	@for element in autovideosrc autoaudiosrc vp8enc rtpvp8pay opusenc \
	  rtpopuspay udpsink udpsrc rtpjitterbuffer rtpvp8depay vp8dec \
	  rtpopusdepay opusdec gtksink autoaudiosink webrtcdsp \
	  webrtcechoprobe clockoverlay playbin; do \
	  gst-inspect-1.0 "$$element" >/dev/null || exit 1; \
	done

PREFIX ?= $(HOME)/.local
APPLICATIONS_DIR ?= $(PREFIX)/share/applications
ICONS_DIR ?= $(PREFIX)/share/icons/hicolor/scalable/apps

install: gtk-pipe gtk-pipe.desktop gtk-pipe-secure.desktop gtk-pipe.svg
	install -Dm755 gtk-pipe "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"
	install -Dm644 gtk-pipe.desktop \
		"$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe.desktop"
	install -Dm644 gtk-pipe-secure.desktop \
		"$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe-secure.desktop"
	install -Dm644 gtk-pipe.svg \
		"$(DESTDIR)$(ICONS_DIR)/gtk-pipe.svg"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe.desktop"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe-secure.desktop"
	rm -f "$(DESTDIR)$(ICONS_DIR)/gtk-pipe.svg"

clean:
	rm -f gtk-pipe gtk-pipe.o secure.o secure-protocol.o pin-buffer.o tests/test_protocol tests/test_secure tests/test_app tests/test_secure_sanitize tests/test_app_sanitize

# Requires a GTK display (the runner starts an isolated Broadway display).
tests/test_secure: tests/test_secure.c secure.c secure-protocol.c pin-buffer.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_secure.c secure.c secure-protocol.c pin-buffer.c $(LDLIBS)

.PHONY: check-ui
tests/test_app: tests/test_app.c gtk-pipe.c secure.c secure-protocol.c pin-buffer.c secure.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_app.c secure.c secure-protocol.c pin-buffer.c $(LDLIBS)

check-ui: tests/test_secure tests/test_app
	python3 tests/run-ui.py

.PHONY: check-ui-sanitize
check-ui-sanitize:
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I. -o tests/test_secure_sanitize tests/test_secure.c secure.c secure-protocol.c pin-buffer.c $(LDLIBS)
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I. -o tests/test_app_sanitize tests/test_app.c secure.c secure-protocol.c pin-buffer.c $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=0 UI_TEST_SUFFIX=_sanitize python3 tests/run-ui.py
