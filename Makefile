TARGET_EXEC := runwhenidle
LDLIBS=-lXss -lX11
CC=gcc
all: executable

release: CCFLAGS += -O0
release: executable

debug: CCFLAGS += -DDEBUG -ggdb
debug: executable

executable:
	$(CC) -o $(TARGET_EXEC) $(CCFLAGS) $(LDFLAGS) $(LDLIBS) main.c

clean:
	rm -f runwhenidle