TARGET_EXEC := runwhenidle
LDLIBS=-lXss -lX11
CC=gcc
ifeq ($(PREFIX),)
    PREFIX := /usr
endif

all: executable

release: CCFLAGS += -O3
release: executable

debug: CCFLAGS += -DDEBUG -ggdb
debug: executable

executable:
	$(CC) main.c -o $(TARGET_EXEC) $(CCFLAGS) $(LDFLAGS) $(LDLIBS)

install: release
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(TARGET_EXEC) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f runwhenidle