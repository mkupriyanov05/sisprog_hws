CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -pedantic -O2
TARGET  := prog_04_02
SRC     := prog_04_02.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

# Быстрая проверка: create/delete + отказы на опасных путях
test: $(TARGET)
	@set -e; t=$$(mktemp -d); \
	./$(TARGET) --create $$t/a.txt; \
	test -f $$t/a.txt; \
	if ./$(TARGET) --create $$t/a.txt 2>/dev/null; then echo "FAIL: повторное создание"; exit 1; fi; \
	if ./$(TARGET) --create $$t/../x 2>/dev/null; then echo "FAIL: '..' в пути"; exit 1; fi; \
	if ./$(TARGET) --create '/etc/evil' 2>/dev/null; then echo "FAIL: /etc"; exit 1; fi; \
	if ./$(TARGET) --create 'a b' 2>/dev/null; then echo "FAIL: пробел в имени"; exit 1; fi; \
	if ./$(TARGET) --delete $$t 2>/dev/null; then echo "FAIL: удаление каталога"; exit 1; fi; \
	if ./$(TARGET) --bogus x 2>/dev/null; then echo "FAIL: неизвестный режим"; exit 1; fi; \
	./$(TARGET) --delete $$t/a.txt; \
	test ! -e $$t/a.txt; \
	rmdir $$t; \
	echo "Все тесты пройдены"
