/* Port: include/CPPLib.h's IS_EMPTY pastes its argument into a name (`_##x##_`), which
 * GCC rejects for anything but a single identifier (e.g. a pp-number like 0.1); IDO
 * tolerated it. The test only asks "is the argument list empty?", which __VA_OPT__
 * answers exactly (GCC 8+ in C, including gnu89): IS_EMPTY() -> 1, anything else -> 0. */
#include_next <CPPLib.h>
#ifdef PORT
#undef IS_EMPTY
#define HH_CPP_FIRST(a, ...) a
#define IS_EMPTY(...) HH_CPP_FIRST(__VA_OPT__(0,) 1)
#endif
