TARGET_EXEC := runwhenidle
LDLIBS=-lXss -lX11
CFLAGS=-O2
CC=gcc
release:
	$(CC) -o $(TARGET_EXEC) $(CFLAGS) $(LDFLAGS) $(LDLIBS) main.c
clean:
	rm -f runwhenidle