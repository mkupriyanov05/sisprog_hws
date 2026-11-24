# makefile_05_03.mk
# Сборка prog_05_03 (heap sort)
#
# Использование:
#   make -f makefile_05_03.mk          - собрать программу
#   make -f makefile_05_03.mk run      - собрать и запустить с примером
#   make -f makefile_05_03.mk test     - собрать и прогнать несколько проверок
#   make -f makefile_05_03.mk debug    - собрать с отладочной информацией и санитайзерами
#   make -f makefile_05_03.mk clean    - удалить собранные файлы

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2
TARGET  := prog_05_03
SRC     := prog_05_03.c

.PHONY: all run test debug clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) --max "5 3 8 1 9 2"

# быстрые проверки: ожидаемый результат указан в комментарии
test: $(TARGET)
	./$(TARGET) --min "5 3 8 1 9 2"          # 1 2 3 5 8 9
	./$(TARGET) --max "5 3 8 1 9 2"          # 9 8 5 3 2 1
	./$(TARGET) "3.5, -2, 10; 0"             # -2 0 3.5 10
	printf '4 1\n3 2\n' > /tmp/prog_05_03_test.txt
	./$(TARGET) --max -f /tmp/prog_05_03_test.txt   # 4 3 2 1
	rm -f /tmp/prog_05_03_test.txt

# отладочная сборка: символы для gdb + поиск утечек и выходов за границы памяти
debug: CFLAGS := -Wall -Wextra -std=c11 -g -O0 -fsanitize=address,undefined
debug: clean $(TARGET)

clean:
	rm -f $(TARGET)
