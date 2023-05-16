TARGET_EXEC := runwhenidle
LDLIBS=-lXss -lX11
CC=gcc
all: executable

release: CCFLAGS += -O3
release: executable

debug: CCFLAGS += -DDEBUG -ggdb
debug: executable

executable:
	$(CC) main.c -o $(TARGET_EXEC) $(CCFLAGS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f runwhenidle