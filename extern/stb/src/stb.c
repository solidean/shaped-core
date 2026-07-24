/*
 * stb single-file public-domain libraries — https://github.com/nothings/stb
 * See ../LICENSE (MIT / public-domain dual license).
 *
 * stb.c instantiates the implementations defined behind macros in the stb headers.
 * The implementation lives here ONLY: every other TU includes the plain headers (declarations only) and links this static target — the exact xxhash src/xxhash.c split.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"
