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

#ifndef STARK_GFX_PSPGU_BITMAP_H
#define STARK_GFX_PSPGU_BITMAP_H

#include "engines/stark/gfx/bitmap.h"
#include "common/array.h"

namespace Stark {
namespace Gfx {

/**
 * Native PSP GE 2D bitmap with automatic <=512 POT tiling, 1-texel overlap
 * filter gutters, edge clamping, and lazy T8 alpha mask for surface fading.
 */
class PspGuBitmap : public Bitmap {
public:
	struct Tile {
		uint16 srcX;
		uint16 srcY;
		uint16 contentW;
		uint16 contentH;
		uint16 padL;
		uint16 padT;
		uint16 texW;
		uint16 texH;
		uint16 stride;
		int psm;
		void *pixels;
		uint32 byteSize;
		uint8 *alphaMask;
		uint32 alphaByteSize;
	};

	explicit PspGuBitmap(const Graphics::Surface *surface = nullptr, const byte *palette = nullptr);
	~PspGuBitmap() override;

	void bind() const override;
	void update(const Graphics::Surface *surface, const byte *palette = nullptr) override;
	void setSamplingFilter(SamplingFilter filter) override;
	Graphics::PixelFormat getBestPixelFormat() const override;

	SamplingFilter getSamplingFilter() const { return _samplingFilter; }
	uint32 getTileCount() const { return _tiles.size(); }
	const Tile &getTile(uint32 index) const { return _tiles[index]; }

	bool ensureAlphaMasks() const;
	uint32 releaseAlphaMasks();
	void markDrawn(uint32 frameSeq) const { _lastDrawnFrameSeq = frameSeq; }
	uint32 getLastDrawnFrameSeq() const { return _lastDrawnFrameSeq; }

	static const uint32 *getAlphaFadeClut();
	static uint32 reclaimIdleAlphaMasks(uint32 currentFrameSeq, uint32 minBytes);

private:
	void releaseTiles();
	void buildAlphaMaskForTile(Tile &tile) const;

	mutable Common::Array<Tile> _tiles;
	SamplingFilter _samplingFilter;
	mutable uint32 _lastDrawnFrameSeq;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_BITMAP_H
