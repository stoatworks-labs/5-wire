#pragma once

#include <FFGLSDK.h>

namespace fivewire
{
/**
    An off-screen buffer for one stage of the chain.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Every buffer here is sized by the
    composition -- or, for the two reduced ones, by the composition divided by
    8 and by 64. Ensure() is called every frame and is a no-op in the
    overwhelming majority of them.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). One leak would not matter; this plugin drops and rebuilds
    every buffer whenever the composition size changes, and somebody dragging a
    resolution slider about should not be paying megabytes of texture memory
    for each position it passes through.

    **It has no depth buffer.** `FFGLFBO::Initialise()` always attaches a
    24-bit depth renderbuffer the size of the picture, and nothing here draws
    with depth: at 4K that is about 32 MB a buffer for nothing. Ensure()
    allocates through InitialiseColourOnly() instead, the same steps with the
    colour texture alone.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared, because a pass that
	/// reads its own history reads this on its first frame.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	/// FFGLFBO::Initialise() less the depth renderbuffer: the framebuffer, the
	/// colour texture and its attachment, and everything released again if
	/// the framebuffer is not complete.
	bool InitialiseColourOnly( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );
};

} // namespace fivewire
