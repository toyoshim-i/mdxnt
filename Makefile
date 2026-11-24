CC      ?= gcc
PYTHON  ?= python3
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -O2 -Iinclude

.PHONY: all test test-compiler test-driver test-rust wasm vscode tools check-z80 clean

all: test

test: test-compiler test-driver test-rust

test-compiler:
	$(PYTHON) tests/test_compiler.py

test-driver:
	@mkdir -p build
	$(CC) $(CFLAGS) -DMDXNT_MOCK_HAL src/mdxnt_drv.c tests/test_driver_host.c -o build/test_driver_host
	./build/test_driver_host

test-rust:
	cargo test --manifest-path tools/mdxnt-rs/Cargo.toml

wasm:
	$(MAKE) -C tools/mdxnt-wasm wasm

vscode:
	cd tools/code && npm install && npm run compile

tools: wasm vscode
	cargo build --release --manifest-path tools/mdxnt-rs/Cargo.toml

check-z80:
	@mkdir -p build
	sdasz80 -o build/mdxnt_hal_pc88.rel hal/pc88_sr/mdxnt_hal_pc88.s
	sdcc -mz80 --sdcccall 1 --opt-code-size --max-allocs-per-node 10000 -Iinclude -c -o build/mdxnt_drv.rel src/mdxnt_drv.c

clean:
	rm -rf build tools/mdxnt-wasm/build tools/code/out
