CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -fno-builtin-malloc -pthread
LDFLAGS = -pthread -lrt
TARGET = memtest

OBJS = umalloc.o memtest.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

umalloc.o: umalloc.c umalloc.h
	$(CC) $(CFLAGS) -c umalloc.c

memtest.o: memtest.c umalloc.h
	$(CC) $(CFLAGS) -c memtest.c

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run