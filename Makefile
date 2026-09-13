# Build the native arm64 "Samsung Scanner" ICDM bundle.
#
# Fully self-contained: Info.plist and Resources/ are checked into the repo,
# so no original Samsung files are required to build.
#
# Usage:
#   make            build the .app bundle in place
#   make clean      remove the built binary + code signature
#   sudo make install   install to /Library/Image Capture/Devices

SDK	   := $(shell xcrun --sdk macosx --show-sdk-path)
CC	   := clang
TARGET := $(shell uname -m)
CODESIGN := codesign

EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
BUNDLE  := Samsung$(SPACE)Scanner.app
MACOS_BIN := $(BUNDLE)/Contents/MacOS/Samsung$(SPACE)Scanner

CFLAGS  := -arch $(TARGET) -isysroot $(SDK) -mmacosx-version-min=13.0 \
           -fconstant-cfstrings -Wall -Wextra -Wno-unused-parameter -O2
FRAMEWORKS := -framework ICADevices -framework CoreFoundation \
              -framework CoreServices -framework IOKit -framework DiskArbitration \
              -framework ImageIO -framework CoreGraphics

all: bundle

bundle: $(MACOS_BIN)
	$(CODESIGN) --force --sign - "$(BUNDLE)"

$(MACOS_BIN): main.c
	mkdir -p "$(BUNDLE)/Contents/MacOS"
	$(CC) $(CFLAGS) -o "$(MACOS_BIN)" main.c $(FRAMEWORKS)

install: bundle
	@echo "== stopping ICA daemon (a fresh one will start) =="
	-killall iccd 2>/dev/null || true
	@echo "== installing to /Library/Image Capture/Devices =="
	rm -rf "/Library/Image Capture/Devices/$(BUNDLE)"
	cp -R "$(BUNDLE)" "/Library/Image Capture/Devices/"
	$(CODESIGN) --force --sign - "/Library/Image Capture/Devices/$(BUNDLE)"
	@echo "== done. open Preview -> File > Import from Scanner to test. =="

clean:
	rm -rf "$(BUNDLE)/Contents/MacOS" "$(BUNDLE)/Contents/_CodeSignature"

.PHONY: all bundle install clean