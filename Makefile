# POSIX make: works with BSD make and GNU make.
#
#   make                       png + jpeg
#   make IMG= IMGLIBS=         no image support (smallest)
#   make IMG=-DHAVE_PNG IMGLIBS=-lpng16
#   make debug                 -O0 -g with ASan/UBSan

VERSION  = 0.1.0
PREFIX   = /usr/local
LOCALBASE= /usr/local
CC       = cc
OPT      = -Os
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -Wno-unused-parameter -Werror
IMG      = -DHAVE_PNG -DHAVE_JPEG
IMGLIBS  = -lpng16 -ljpeg
CFLAGS   = -std=c11 $(OPT) -pipe -ffunction-sections -fdata-sections $(WARN)
CPPFLAGS = -DVERSION=\"$(VERSION)\" $(IMG) -I$(LOCALBASE)/include -I$(LOCALBASE)/include/libpng16
LDFLAGS  = -Wl,--gc-sections -L$(LOCALBASE)/lib
LIBS     = $(IMGLIBS) -lm

OBJ = src/main.o src/buf.o src/json.o src/fs.o src/search.o src/proc.o src/img.o

all: mcp-fbsd

mcp-fbsd: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): src/mcp.h src/config.h

src/config.h:
	cp src/config.def.h $@

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

debug:
	$(MAKE) clean
	$(MAKE) OPT="-O0 -g -fsanitize=address,undefined" LDFLAGS="-L$(LOCALBASE)/lib -fsanitize=address,undefined"

strip: mcp-fbsd
	strip mcp-fbsd

check: mcp-fbsd
	python3 tests/run.py ./mcp-fbsd

install: mcp-fbsd
	install -d $(DESTDIR)$(PREFIX)/bin
	install -s -m 755 mcp-fbsd $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f mcp-fbsd $(OBJ)

.PHONY: all debug strip check install clean
