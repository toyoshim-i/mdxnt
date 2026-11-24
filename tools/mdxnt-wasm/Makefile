WASI_SDK ?= $(HOME)/.local/wasi-sdk
WASM_CC  ?= $(WASI_SDK)/bin/clang
WASM_CXX ?= $(WASI_SDK)/bin/clang++

ROOT := ../..
YMFM := $(ROOT)/third_party/ymfm/src
OUT_WASM := $(ROOT)/tools/code/media/mdxnt_player.wasm

COMMON_INC := -I$(ROOT)/include -I$(YMFM) -I.
WASM_CFLAGS := -O3 -DNDEBUG -std=c99 -DMDXNT_MOCK_HAL $(COMMON_INC)
WASM_CXXFLAGS := -O3 -DNDEBUG -std=c++14 -fno-exceptions -fno-rtti $(COMMON_INC)
WASM_LDFLAGS := -mexec-model=reactor -Wl,--no-entry -Wl,--gc-sections -Wl,--strip-debug

YMFM_SRCS := $(YMFM)/ymfm_opn.cpp $(YMFM)/ymfm_ssg.cpp $(YMFM)/ymfm_adpcm.cpp
PLAYER_SRCS := ymfm_chip.cpp mdxnt_player.cpp

.PHONY: all wasm host-test clean

all: wasm

wasm: $(OUT_WASM)

$(OUT_WASM): $(ROOT)/src/mdxnt_drv.c $(YMFM_SRCS) $(PLAYER_SRCS) opn_chip.h
	@mkdir -p build $(dir $(OUT_WASM))
	$(WASM_CC) $(WASM_CFLAGS) -c $(ROOT)/src/mdxnt_drv.c -o build/mdxnt_drv.o
	$(WASM_CXX) $(WASM_CXXFLAGS) -c $(YMFM)/ymfm_opn.cpp -o build/ymfm_opn.o
	$(WASM_CXX) $(WASM_CXXFLAGS) -c $(YMFM)/ymfm_ssg.cpp -o build/ymfm_ssg.o
	$(WASM_CXX) $(WASM_CXXFLAGS) -c $(YMFM)/ymfm_adpcm.cpp -o build/ymfm_adpcm.o
	$(WASM_CXX) $(WASM_CXXFLAGS) -c ymfm_chip.cpp -o build/ymfm_chip.o
	$(WASM_CXX) $(WASM_CXXFLAGS) -c mdxnt_player.cpp -o build/mdxnt_player.o
	$(WASM_CXX) $(WASM_LDFLAGS) \
		build/mdxnt_drv.o build/ymfm_opn.o build/ymfm_ssg.o build/ymfm_adpcm.o \
		build/ymfm_chip.o build/mdxnt_player.o \
		-o $(OUT_WASM)
	@ls -lh $(OUT_WASM)

host-test:
	@mkdir -p build
	gcc -std=c99 -O2 -DMDXNT_MOCK_HAL $(COMMON_INC) -c $(ROOT)/src/mdxnt_drv.c -o build/mdxnt_drv_host.o
	g++ -std=c++14 -O2 $(COMMON_INC) build/mdxnt_drv_host.o $(YMFM_SRCS) $(PLAYER_SRCS) test_host.cpp -o build/test_host
	./build/test_host $(ROOT)/build/demo_rs.mdxt

clean:
	rm -rf build $(OUT_WASM)
