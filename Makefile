override CFLAGS := -Wall -std=gnu99 -pedantic -O0 -g -pthread $(CFLAGS)
override LDLIBS := -pthread $(LDLIBS)

fs.o: fs.c

disk.o: disk.c disk.h

.PHONY: clean

clean:
	rm -f fs.o