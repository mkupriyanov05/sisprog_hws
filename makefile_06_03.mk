# Сборка prog_06_03:   make -f makefile_06_03.mk
# Очистка:           make -f makefile_06_03.mk clean
#
# Нужны заголовки и библиотека OpenSSL:  sudo apt install libssl-dev

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDLIBS  = -lcrypto
TARGET  = prog_06_03

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
