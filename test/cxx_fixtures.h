/**
 * @file cxx_fixtures.h
 * C++ functions the symbol reader must name correctly, reached from C.
 *
 * The fixtures live in a C++ translation unit so the test binary carries
 * namespaces, classes, templates, and a lambda; this header is the C
 * view of them. Methods and the lambda's `operator()` have no address a
 * C caller can take, so the test finds those by scope instead.
 */
#ifndef CW_TEST_CXX_FIXTURES_H
#define CW_TEST_CXX_FIXTURES_H

#include <stdint.h>

/**
 * Addresses of the fixture functions, as the running process sees them.
 */
typedef struct {
	uintptr_t static_method; /**< `ns::Foo::baz`, external. */
	uintptr_t anon_fn;       /**< `ns::<anonymous>::hidden`, internal linkage. */
	uintptr_t template_fn;   /**< `ns::twice<int>`, external. */
	uintptr_t file_static;   /**< `file_static`, internal linkage. */
	uintptr_t lambda_host;   /**< `lambda_host`, a static whose body defines a lambda. */
	uintptr_t lambda_host_ext; /**< `lambda_host_ext`, external, whose body defines a lambda. */
} test_cxx_fixtures_t;

#ifdef __cplusplus
extern "C"
#endif
void
test_cxx_fixtures(test_cxx_fixtures_t* out);

#endif /* CW_TEST_CXX_FIXTURES_H */
