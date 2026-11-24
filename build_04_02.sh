#!/bin/bash
# build.sh — сборка prog_04_02 (опционально с тестами)
#   ./build.sh         — собрать
#   ./build.sh test    — собрать и прогнать тесты
#   ./build.sh clean   — удалить бинарник
set -euo pipefail
cd "$(dirname "$0")"

case "${1:-all}" in
    all)   make -f makefile_04_02.mk all ;;
    test)  make -f makefile_04_02.mk all && make -f makefile_04_02.mk test ;;
    clean) make -f makefile_04_02.mk clean ;;
    *)     echo "Использование: $0 [all|test|clean]" >&2; exit 2 ;;
esac
