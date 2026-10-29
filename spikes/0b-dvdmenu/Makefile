PKGS   = mpv dvdnav sdl3 gl
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I../../libdiscnav \
         $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lpthread
SRCS   = dvdmenu.c ../../libdiscnav/spu.c

dvdmenu: $(SRCS) ../../libdiscnav/spu.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f dvdmenu

.PHONY: clean
