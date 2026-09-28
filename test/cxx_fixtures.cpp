/**
 * @file cxx_fixtures.cpp
 * One function per C++ naming shape, kept out of line so each has a
 * range of its own. Nothing here is called; the addresses are enough.
 */
#include "cxx_fixtures.h"

#if defined(_MSC_VER)
#define KEEP __declspec(noinline)
#else
#define KEEP __attribute__((noinline))
#endif

namespace ns {

struct Foo {
	KEEP int bar(int a, const char* s);
	KEEP static int baz();
};

int
Foo::bar(int a, const char* s) {
	return a + (s != nullptr ? s[0] : 0);
}

int
Foo::baz() {
	return 3;
}

namespace {

KEEP int
hidden(int x) {
	return x * 2;
}

} // namespace

template <typename T>
KEEP T
twice(T t) {
	return t + t;
}

} // namespace ns

static KEEP int
file_static(int x) {
	return x - 1;
}

static KEEP int
lambda_host(int x) {
	auto add_one = [](int q) KEEP { return q + 1; };
	return add_one(x);
}

/* Keeps the method, which has no address a C caller can take, out of line and present. */
static int (ns::Foo::* volatile keep_bar)(int, const char*) = &ns::Foo::bar;

extern "C" void
test_cxx_fixtures(test_cxx_fixtures_t* out) {
	(void)keep_bar;
	*out = test_cxx_fixtures_t{
		reinterpret_cast<uintptr_t>(&ns::Foo::baz),
		reinterpret_cast<uintptr_t>(&ns::hidden),
		reinterpret_cast<uintptr_t>(&ns::twice<int>),
		reinterpret_cast<uintptr_t>(&file_static),
		reinterpret_cast<uintptr_t>(&lambda_host),
	};
}
