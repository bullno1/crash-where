// Source of wasm_dwarf.wasm, the Wasm reader's DWARF fixture: a module
// with DWARF, a name section and a build id, and nothing else linked in.
// Built with the clang and wasm-ld of Emscripten 6.0.10 (LLVM 24), from
// this directory:
//
//   clang --target=wasm32 -O1 -g -fdebug-compilation-dir=. -fno-exceptions \
//     -c wasm_dwarf.cpp -o wasm_dwarf.o
//   wasm-ld --no-entry --export-all --build-id=sha1 wasm_dwarf.o -o wasm_dwarf.wasm
//
// test/wasm.c names lines of this file by number; keep them in place.
#define KEEP __attribute__((noinline))
volatile int side;

namespace ns {
struct Foo {
	KEEP static int bar(volatile int* p) { return 1000 / *p; }
	KEEP int operator()(int x) const;
};
int Foo::operator()(int x) const { return x + 1; }
}

namespace {
KEEP int hidden(int x) {
	side = x;
	return x * 2;
}
}

static inline __attribute__((always_inline)) int twice(int x) {
	side = x;
	return x * 2;
}

KEEP static int helper(volatile int* p) {
	return twice(ns::Foo::bar(p));
}

template <typename T>
KEEP T tpl(T x) { return x + 1; }

// Kept by the compiler, never called and not exported: the linker drops it
// and tombstones its DWARF.
static __attribute__((used)) int unused(int x) {
	side = x;
	return x;
}

extern "C" KEEP int outer(volatile int* p) {
	ns::Foo f;
	return helper(p) + hidden(1) + f(2) + tpl(3);
}
