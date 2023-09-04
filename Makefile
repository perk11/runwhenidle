TARGET_EXEC := runwhenidle
LDLIBS=-lXss -lX11
CC=gcc
ifeq ($(PREFIX),)
    PREFIX := /usr
endif
SOURCES = time_utils.c sleep_utils.c tty_utils.c main.c process_handling.c
OBJECTS = $(SOURCES:.c=.o)
all: executable

release: CCFLAGS += -O3
release: executable

debug: CCFLAGS += -DDEBUG -ggdb
debug: executable

executable: CCFLAGS += -DVERSION=\"$(shell git describe --tags 2>/dev/null || (echo -n "0.0-dev-" && git rev-parse HEAD))\"

%.o: %.c
	$(CC) $(CCFLAGS) -c $< -o $@ $(LDFLAGS) $(LDLIBS)

executable: $(OBJECTS)
	$(CC) $(CCFLAGS) $(OBJECTS) -o $(TARGET_EXEC) $(LDFLAGS) $(LDLIBS)

install: release
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(TARGET_EXEC) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f $(OBJECTS) $(TARGET_EXEC)

debian-package:
	docker build --build-arg HOST_UID=`id -u` --tag runwhenidle-ubuntu2204-build distro-packages/ubuntu22.04
	docker run --user build -v .:/opt/src/runwhenidle runwhenidle-ubuntu2204-build /opt/src/runwhenidle/distro-packages/ubuntu22.04/build.sh

clean-debian-package:
	rm -rf package-build
	docker rmi -f runwhenidle-ubuntu2204-build
