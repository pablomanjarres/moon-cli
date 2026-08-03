# ======================================================================================
# moon — an educational Linux system-call shell
# ======================================================================================
#   make          build ./moon
#   make run      build and open the shell
#   make clean    remove the binary and the object files
#
# You never have to edit this file when you add a command: SRCS picks up every .c
# in the folder automatically.
# ======================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE
TARGET  = moon

SRCS    = $(wildcard *.c)
HDRS    = $(wildcard *.h)
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)
	@echo "  built -> ./$(TARGET)"

# Every .c depends on every .h: touch shell.h or ui.h and it all rebuilds.
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all run clean
