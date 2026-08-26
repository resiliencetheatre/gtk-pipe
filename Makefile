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

PREFIX ?= /usr/local
install: gtk-pipe
	install -Dm755 gtk-pipe "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/gtk-pipe"

clean:
	rm -f gtk-pipe gtk-pipe.o
