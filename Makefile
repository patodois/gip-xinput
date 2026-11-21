CARGO ?= cargo
MINGW_CC ?= x86_64-w64-mingw32-gcc
NATIVE_CC ?= cc
# Native tests use their own port so they never meet a running real host on 26785.
TEST_PORT ?= 26795
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror

.PHONY: all host windows test test-wine
all: host windows

host:
	$(CARGO) build --release --locked --target aarch64-apple-darwin

windows: build/xinput1_3.dll build/xinput-probe.exe

build:
	mkdir -p build

build/xinput1_3.dll: windows/xinput.c windows/protocol.h windows/xinput1_3.def | build
	$(MINGW_CC) $(CFLAGS) -shared -static-libgcc -o "$@" windows/xinput.c windows/xinput1_3.def -lws2_32

build/xinput-probe.exe: windows/probe.c windows/protocol.h | build
	$(MINGW_CC) $(CFLAGS) -static-libgcc -o "$@" "$<"

build/protocol-test: tests/protocol_test.c windows/protocol.h | build
	$(NATIVE_CC) $(CFLAGS) -fsanitize=address,undefined -Iwindows -o "$@" "$<"

build/api-test: windows/xinput.c windows/probe.c windows/protocol.h tests/winapi_test.h tests/winapi_test.c | build
	$(NATIVE_CC) $(CFLAGS) -DGX_NATIVE_TEST -DGX_PORT=$(TEST_PORT) -fsanitize=address,undefined -pthread -o "$@" windows/xinput.c windows/probe.c tests/winapi_test.c

test: windows build/protocol-test build/api-test
	$(CARGO) test --locked
	$(CARGO) build --locked --bin gip-xinput-fake-host
	./build/protocol-test
	GX_TEST_PORT=$(TEST_PORT) python3 -m unittest discover -s tests -p 'test_*.py' -v

# make test-wine WINE=/path/to/wine (a fresh prefix is created under build/)
test-wine: all
	@test -n "$(WINE)" || { echo "set WINE=/path/to/wine" >&2; exit 2; }
	python3 tests/wine_test.py --wine "$(WINE)"
