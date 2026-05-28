CC = gcc
CFLAGS = -O2 -Wall -Wextra -march=native
LDFLAGS =

SRCDIR = src
BUILDDIR = build
TARGET = pointer_chase_only.x

all: init $(TARGET)

init:
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/pointer_chase_only.o: $(SRCDIR)/pointer_chase_only.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(BUILDDIR)/pointer_chase_only.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BUILDDIR)/*.o $(TARGET)

.PHONY: all init clean
