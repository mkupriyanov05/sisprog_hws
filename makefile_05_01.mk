# makefile_05_01.mk
# Сборка prog_05_01 (password keeper на стеке)
#
# Использование:
#   make -f makefile_05_01.mk          - собрать программу
#   make -f makefile_05_01.mk run      - собрать и запустить
#   make -f makefile_05_01.mk debug    - собрать с отладочной информацией и санитайзерами
#   make -f makefile_05_01.mk clean    - удалить собранные файлы

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2
TARGET  := prog_05_01
SRC     := prog_05_01.c

.PHONY: all run debug clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

# отладочная сборка: символы для gdb + поиск утечек и выходов за границы массива
debug: CFLAGS := -Wall -Wextra -std=c11 -g -O0 -fsanitize=address,undefined
debug: clean $(TARGET)

clean:
	rm -f $(TARGET)
