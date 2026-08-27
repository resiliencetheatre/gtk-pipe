CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
PKGS = gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gio-2.0
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

.PHONY: all clean check install uninstall

all: gtk-pipe

gtk-pipe: gtk-pipe.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

gtk-pipe.o: gtk-pipe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

check: gtk-pipe
	./gtk-pipe --help
	@for element in autovideosrc autoaudiosrc vp8enc rtpvp8pay opusenc \
	  rtpopuspay udpsink udpsrc rtpjitterbuffer rtpvp8depay vp8dec \
	  rtpopusdepay opusdec gtksink autoaudiosink webrtcdsp \
	  webrtcechoprobe clockoverlay playbin; do \
	  gst-inspect-1.0 "$$element" >/dev/null || exit 1; \
	done

PREFIX ?= $(HOME)/.local
APPLICATIONS_DIR ?= $(PREFIX)/share/applications
ICONS_DIR ?= $(PREFIX)/share/icons/hicolor/scalable/apps

install: gtk-pipe gtk-pipe.desktop gtk-pipe.svg
	install -Dm755 gtk-pipe "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"
	install -Dm644 gtk-pipe.desktop \
		"$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe.desktop"
	install -Dm644 gtk-pipe.svg \
		"$(DESTDIR)$(ICONS_DIR)/gtk-pipe.svg"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"
	rm -f "$(DESTDIR)$(APPLICATIONS_DIR)/gtk-pipe.desktop"
	rm -f "$(DESTDIR)$(ICONS_DIR)/gtk-pipe.svg"

clean:
	rm -f gtk-pipe gtk-pipe.o
