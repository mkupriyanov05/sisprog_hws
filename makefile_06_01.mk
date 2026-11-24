# Сборка prog_06_01:   make -f makefile_06_01.mk
# Очистка:           make -f makefile_06_01.mk clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
TARGET  = prog_06_01

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
