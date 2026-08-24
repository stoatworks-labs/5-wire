#include "../Shaders.h"

namespace fivewire::shaders
{
/// Texture coordinates pass through UNSCALED -- `MaxUV` is applied at each
/// fetch instead. See the note in Shaders.h: every pass here samples at a
/// horizontal offset from where it was told to, the offsets are in pixels of
/// the picture, and three of the five passes are reading this plugin's own
/// buffers where MaxUV is 1 rather than the host's padded texture.
const char* const kVertex = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";
} // namespace fivewire::shaders
