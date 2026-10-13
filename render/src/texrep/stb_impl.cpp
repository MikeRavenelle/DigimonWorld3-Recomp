// Implementation of the stb single-header image libraries (third_party/stb), built without warnings.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
#include "stb_image.h"
#include "stb_image_write.h"
