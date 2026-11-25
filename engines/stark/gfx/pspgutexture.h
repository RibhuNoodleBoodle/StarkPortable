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
 * Step 01 placeholder texture implementation for native PSP sceGu driver.
 */
class PspGuTexture : public Texture {
public:
	PspGuTexture();
	~PspGuTexture() override;

	void bind() const override;
	void setLevelCount(uint32 count) override;
	void addLevel(uint32 level, const Graphics::Surface *surface, const byte *palette = nullptr) override;

private:
	uint32 _levelCount;
	mutable bool _warnedBind;
	bool _warnedAdd;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_TEXTURE_H
