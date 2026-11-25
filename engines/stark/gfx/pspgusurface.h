/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef STARK_GFX_PSPGU_SURFACE_H
#define STARK_GFX_PSPGU_SURFACE_H

#include "engines/stark/gfx/surfacerenderer.h"

namespace Stark {
namespace Gfx {

class PspGuDriver;

/**
 * Native PSP GE 2D surface renderer supporting premultiplied alpha blending,
 * tiled large bitmaps, solid fills, snap-to-grid, and signed per-pixel fade.
 */
class PspGuSurfaceRenderer : public SurfaceRenderer {
public:
	explicit PspGuSurfaceRenderer(PspGuDriver *gfx);
	~PspGuSurfaceRenderer() override;

	void render(const Bitmap *bitmap, const Common::Point &dest) override;
	void render(const Bitmap *bitmap, const Common::Point &dest, uint dstW, uint dstH) override;
	void fill(const Color &color, const Common::Point &dest, uint dstW, uint dstH) override;

private:
	void prepare2DState() const;
	bool clipQuadToViewport(float &x0, float &y0, float &x1, float &y1,
	                        float *u0, float *v0, float *u1, float *v1) const;

	PspGuDriver *_gfx;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_SURFACE_H
