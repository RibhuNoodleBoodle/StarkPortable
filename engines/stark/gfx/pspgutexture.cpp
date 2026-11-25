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

#include "engines/stark/gfx/pspgutexture.h"

#include "common/debug.h"

namespace Stark {
namespace Gfx {

PspGuTexture::PspGuTexture() :
		_levelCount(0),
		_warnedBind(false),
		_warnedAdd(false) {
}

PspGuTexture::~PspGuTexture() {
}

void PspGuTexture::bind() const {
	if (!_warnedBind) {
		_warnedBind = true;
		warning("STARK PSP Step01: PspGuTexture::bind unimplemented");
	}
}

void PspGuTexture::setLevelCount(uint32 count) {
	_levelCount = count;
}

void PspGuTexture::addLevel(uint32 level, const Graphics::Surface *surface, const byte *palette) {
	if (!_warnedAdd) {
		_warnedAdd = true;
		warning("STARK PSP Step01: PspGuTexture::addLevel(level=%u) unimplemented", level);
	}
}

} // End of namespace Gfx
} // End of namespace Stark
