PYTHON ?= python3
CLANG_FORMAT ?= clang-format
WORK ?= .make-work
WORK_DIR := $(abspath $(WORK))

ELEKLOADER ?=
CORE_MOD ?=
SEQ_MOD ?=
STOCK ?=

FORMAT_FILES := $(wildcard src/*/*.c src/*/*.h mods/mutiny/*.c mods/mutiny/*.h)

export CLANG_FORMAT

.PHONY: help test format format-check mod clean

help:
	@echo "make test           host C tests, Python tests, format check"
	@echo "make format         format the C sources with clang-format"
	@echo "make format-check   check formatting only"
	@echo "make mod STOCK=...  build and lint the mod; needs ELEKLOADER=, CORE_MOD=, SEQ_MOD="
	@echo "make clean          remove $(WORK)"

test:
	$(PYTHON) tools/check.py test --work "$(WORK_DIR)/test"

format:
	$(CLANG_FORMAT) -i --style=file $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror --style=file $(FORMAT_FILES)

mod:
	@test -n "$(STOCK)" || (echo "make mod: set STOCK to your Digitone OS 1.44 .syx" >&2; exit 2)
	MUTINY_ELEKLOADER="$(ELEKLOADER)" MUTINY_STOCK="$(STOCK)" \
	MUTINY_CORE_MOD="$(CORE_MOD)" MUTINY_SEQ_MOD="$(SEQ_MOD)" \
	$(PYTHON) tools/check.py mod --work "$(WORK_DIR)/mod"

clean:
	rm -rf "$(WORK_DIR)"
