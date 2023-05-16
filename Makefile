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

debian-package:
	docker build --build-arg HOST_UID=`id -u` --tag runwhenidle-ubuntu2204-build distro-packages/ubuntu22.04
	docker run --user build -v .:/opt/src/runwhenidle runwhenidle-ubuntu2204-build /opt/src/runwhenidle/distro-packages/ubuntu22.04/build.sh

clean-debian-package:
	rm -rf package-build
	docker rmi -f runwhenidle-ubuntu2204-build
