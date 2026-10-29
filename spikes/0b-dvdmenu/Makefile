PKGS   = mpv dvdnav sdl3 gl
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lpthread

dvdmenu: dvdmenu.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f dvdmenu

.PHONY: clean
