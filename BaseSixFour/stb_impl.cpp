// stb_impl.cpp
// Instantiates stb_image_write implementation exactly once.

#define STB_IMAGE_WRITE_IMPLEMENTATION

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4996 4456)
#endif

#include "stb_image_write.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif
