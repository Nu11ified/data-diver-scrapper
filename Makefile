# Build the engine, run the tests, and (optionally) set up the headless
# renderer used for JS-heavy portals via render+https:// urls.

all: build test

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build -j

test: build
	ctest --test-dir build --output-on-failure

run: build
	./build/datadiver

renderer:
	cd tools && npm install

run-with-renderer: build renderer
	DD_RENDERER="npx --prefix tools tsx tools/render.ts" ./build/datadiver

.PHONY: all build test run renderer run-with-renderer
