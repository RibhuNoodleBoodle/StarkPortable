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

#ifndef STARK_GFX_PSPGU_TEXTURE_H
#define STARK_GFX_PSPGU_TEXTURE_H

#include "engines/stark/gfx/texture.h"

namespace Stark {
namespace Gfx {

/**
 * Native PSP GE 3D texture storing up to 8 aligned POT RGBA8888 mip levels (<= 512x512).
 */
class PspGuTexture : public Texture {
public:
	static const uint32 kMaxMipLevels = 8;

	struct MipLevel {
		void *data;
		uint32 byteSize;
		uint16 texW;
		uint16 texH;
		uint16 stride;
	};

	PspGuTexture();
	~PspGuTexture() override;

	void bind() const override;
	void bindWrapped(int wrapU, int wrapV, bool linearFilter) const;
	void setLevelCount(uint32 count) override;
	void addLevel(uint32 level, const Graphics::Surface *surface, const byte *palette = nullptr) override;

	uint32 getLevelCount() const { return _uploadedLevels; }
	uint16 getBaseWidth() const { return _baseWidth; }
	uint16 getBaseHeight() const { return _baseHeight; }
	bool isIndexedT8() const { return _isIndexedT8; }

private:
	void releaseLevels();
	void releaseClut();

	MipLevel _levels[kMaxMipLevels];
	uint32 *_clut;
	uint32 _clutBytes;
	uint32 _requestedLevels;
	uint32 _uploadedLevels;
	uint16 _baseWidth;
	uint16 _baseHeight;
	bool _isIndexedT8;
	mutable uint32 _lastBoundFrameSeq;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_TEXTURE_H
