CXX := xcrun clang++
SDK_PATH := $(shell xcrun --show-sdk-path)
FTXUI_PREFIX := /opt/homebrew/opt/ftxui

SOURCES := \
	src/main.cpp \
	src/app.cpp \
	src/core/debug.cpp \
	src/core/diff.cpp \
	src/core/process_killer.cpp \
	src/core/process_usage.cpp \
	src/core/snapshot_pipeline.cpp \
	src/scanner/scanner_support.cpp \
	src/scanner/lsof_fallback.cpp \
	src/scanner/serial_scanner.cpp \
	src/scanner/parallel_scanner.cpp \
	src/tui/live_table.cpp

CXXFLAGS := -std=c++20 -Wall -Wextra -pthread -Iinclude -I$(FTXUI_PREFIX)/include \
	-isysroot $(SDK_PATH)
LDFLAGS := -lproc -L$(FTXUI_PREFIX)/lib -lftxui-component -lftxui-dom -lftxui-screen \
	-Wl,-rpath,$(FTXUI_PREFIX)/lib

.PHONY: all run clean

all: portui

portui: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

run: portui
	./portui

clean:
	rm -f portui
