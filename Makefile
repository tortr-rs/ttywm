# ttywm
VERSION = 0.5
PREFIX = /usr/local

PKGS = x11 xft fontconfig dbus-1
CFLAGS += -std=c99 -pedantic -Wall -Os -D_DEFAULT_SOURCE -DVERSION=\"$(VERSION)\" `pkg-config --cflags $(PKGS)`
LDLIBS += `pkg-config --libs $(PKGS)` -lpam

ttywm: ttywm.c
	$(CC) $(CFLAGS) -o $@ ttywm.c $(LDFLAGS) $(LDLIBS)

install: ttywm
	@[ -n "$(DESTDIR)" ] || command -v xterm >/dev/null || { echo "ttywm needs xterm: install it first"; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 ttywm ttywm-term ttywm-session $(DESTDIR)$(PREFIX)/bin/
	mkdir -p $(DESTDIR)/usr/share/fonts/ttywm
	install -m 644 vga.ttf $(DESTDIR)/usr/share/fonts/ttywm/
	-fc-cache -f $(DESTDIR)/usr/share/fonts/ttywm
	mkdir -p $(DESTDIR)/usr/share/xsessions
	sed 's|@PREFIX@|$(PREFIX)|' ttywm.desktop > $(DESTDIR)/usr/share/xsessions/ttywm.desktop

uninstall:
	rm -rf $(DESTDIR)$(PREFIX)/bin/ttywm $(DESTDIR)$(PREFIX)/bin/ttywm-term $(DESTDIR)$(PREFIX)/bin/ttywm-session $(DESTDIR)/usr/share/fonts/ttywm \
	      $(DESTDIR)/usr/share/xsessions/ttywm.desktop

# fbttywm: the same on the framebuffer, without X. needs nothing but a libc
fbttywm: fbttywm.c
	$(CC) -O2 -Wall -o $@ fbttywm.c

install-fb: fbttywm
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 fbttywm $(DESTDIR)$(PREFIX)/bin/

uninstall-fb:
	rm -f $(DESTDIR)$(PREFIX)/bin/fbttywm

clean:
	rm -f ttywm fbttywm

.PHONY: install uninstall install-fb uninstall-fb clean
