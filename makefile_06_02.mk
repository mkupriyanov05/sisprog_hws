# Сборка prog_06_02:   make -f makefile_06_02.mk
# Очистка:           make -f makefile_06_02.mk clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
TARGET  = prog_06_02

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
