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

#include "engines/stark/gfx/pspgubitmap.h"
#include "engines/stark/gfx/driver.h"

#include "common/debug.h"
#include "graphics/pixelformat.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

PspGuBitmap::PspGuBitmap(const Graphics::Surface *surface, const byte *palette) :
		_samplingFilter(kLinear),
		_warnedBind(false),
		_warnedUpdate(false) {
	if (surface) {
		update(surface, palette);
	}
}

PspGuBitmap::~PspGuBitmap() {
}

void PspGuBitmap::bind() const {
	if (!_warnedBind) {
		_warnedBind = true;
		warning("STARK PSP Step01: PspGuBitmap::bind unimplemented");
	}
}

void PspGuBitmap::update(const Graphics::Surface *surface, const byte *palette) {
	if (surface) {
		_width = surface->w;
		_height = surface->h;
	}
	if (!_warnedUpdate) {
		_warnedUpdate = true;
		warning("STARK PSP Step01: PspGuBitmap::update(%ux%u) unimplemented", _width, _height);
	}
}

void PspGuBitmap::setSamplingFilter(SamplingFilter filter) {
	_samplingFilter = filter;
}

Graphics::PixelFormat PspGuBitmap::getBestPixelFormat() const {
	return Driver::getRGBAPixelFormat();
}

} // End of namespace Gfx
} // End of namespace Stark
