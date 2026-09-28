VERSION = 1.2.1
CFLAGS  = -O2 -Wall -Wextra -Wno-deprecated-declarations -mmacosx-version-min=11.0 -arch arm64 -arch x86_64

build/rastertoip100: src/rastertoip100.c $(wildcard src/ip100_*_lut.h)
	mkdir -p build
	clang $(CFLAGS) -o $@ $< -lcups

# Regenerate the colour table from the measurement data in data/
lut:
	for m in plain draft superfine envelope glossy glossy2 pro matte; do python3 tools/buildlut.py $$m; done

# Signed + notarized installer (see packaging/build-pkg.sh)
pkg: build/rastertoip100
	VERSION=$(VERSION) packaging/build-pkg.sh

clean:
	rm -rf build dist

.PHONY: lut pkg clean
