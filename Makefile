CC      ?= gcc
PYTHON  ?= python3
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude

.PHONY: all test test-compiler test-driver check-z80 clean

all: test

test: test-compiler test-driver

test-compiler:
	$(PYTHON) tests/test_compiler.py

test-driver:
	@mkdir -p build
	$(CC) $(CFLAGS) -DMDXNT_MOCK_HAL src/mdxnt_drv.c tests/test_driver_host.c -o build/test_driver_host
	./build/test_driver_host

check-z80:
	@mkdir -p build
	sdasz80 -o build/mdxnt_hal_pc88.rel hal/pc88_sr/mdxnt_hal_pc88.s
	sdcc -mz80 --sdcccall 1 --opt-code-size --max-allocs-per-node 10000 -Iinclude -c -o build/mdxnt_drv.rel src/mdxnt_drv.c

clean:
	rm -rf build
