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

#include "engines/stark/visual/image.h"

#include "graphics/surface.h"
#include "image/png.h"

#include "engines/stark/formats/xmg.h"
#include "engines/stark/gfx/driver.h"
#include "engines/stark/gfx/surfacerenderer.h"
#include "engines/stark/gfx/bitmap.h"
#include "engines/stark/services/services.h"
#include "engines/stark/services/settings.h"

namespace Stark {

VisualImageXMG::VisualImageXMG(Gfx::Driver *gfx) :
		Visual(TYPE),
		_gfx(gfx),
		_bitmap(nullptr),
		_surface(nullptr),
		_originalWidth(0),
		_originalHeight(0),
		_surfaceW(0),
		_surfaceH(0),
		_hasSolidMask(false),
		_keepSurface(false) {
	_surfaceRenderer = _gfx->createSurfaceRenderer();
}

VisualImageXMG::~VisualImageXMG() {
	if (_hasSolidMask && !_solidMask.empty() && _gfx) {
		_gfx->trackExternalAuxBytes(-(int32)_solidMask.size());
	}
	if (_surface) {
		_surface->free();
	}
	delete _surface;
	delete _bitmap;
	delete _surfaceRenderer;
}

void VisualImageXMG::compactSurfaceIfSafe() {
	if (_keepSurface || !_gfx || !_gfx->supportsNativeEffects() || !_surface || !_surface->getPixels()) {
		return;
	}
	// Preserve full CPU pixels only for small images (e.g. top-bar icons used by Button effects)
	if (_originalWidth <= 96 && _originalHeight <= 96) {
		return;
	}

	_surfaceW = (uint16)_surface->w;
	_surfaceH = (uint16)_surface->h;

	if (_originalWidth >= 32 && _originalHeight >= 32 && _surfaceW > 0 && _surfaceH > 0) {
		bool allOpaque = true;
		const uint32 totalPx = (uint32)_surfaceW * (uint32)_surfaceH;
		for (uint16 y = 0; y < _surfaceH && allOpaque; ++y) {
			const uint8 *row = (const uint8 *)_surface->getBasePtr(0, y);
			for (uint16 x = 0; x < _surfaceW; ++x) {
				if (row[x * 4 + 3] != 0xFF) {
					allOpaque = false;
					break;
				}
			}
		}
		if (!allOpaque) {
			const uint32 maskBytes = (totalPx + 7u) >> 3;
			_solidMask.resize(maskBytes, 0);
			uint32 bitIdx = 0;
			for (uint16 y = 0; y < _surfaceH; ++y) {
				const uint8 *row = (const uint8 *)_surface->getBasePtr(0, y);
				for (uint16 x = 0; x < _surfaceW; ++x, ++bitIdx) {
					if (row[x * 4 + 3] == 0xFF) {
						_solidMask[bitIdx >> 3] |= (uint8)(1u << (bitIdx & 7u));
					}
				}
			}
			_gfx->trackExternalAuxBytes((int32)_solidMask.size());
		}
		_hasSolidMask = true;
	}

	_surface->free();
	delete _surface;
	_surface = nullptr;
}

void VisualImageXMG::setHotSpot(const Common::Point &hotspot) {
	_hotspot = hotspot;
}

void VisualImageXMG::load(Common::ReadStream *stream) {
	assert(!_surface && !_bitmap);

	// Decode the XMG
	_surface = Formats::XMGDecoder::decode(stream);
	_bitmap = _gfx->createBitmap(_surface);
	_bitmap->setSamplingFilter(StarkSettings->getImageSamplingFilter());

	_originalWidth  = _surface->w;
	_originalHeight = _surface->h;
	compactSurfaceIfSafe();
}

void VisualImageXMG::readOriginalSize(Common::ReadStream *stream) {
	Formats::XMGDecoder::readSize(stream, _originalWidth, _originalHeight);
}

bool VisualImageXMG::loadPNG(Common::SeekableReadStream *stream) {
	assert(!_surface && !_bitmap);

	// Decode the XMG
	Image::PNGDecoder pngDecoder;
	if (!pngDecoder.loadStream(*stream)) {
		return false;
	}

	if (pngDecoder.hasPalette()) {
		warning("Indexed colors PNG images are not supported");
		return false;
	}

	if (StarkSettings->shouldPreMultiplyReplacementPNGs()) {
		// We can do alpha pre-multiplication when loading for
		// convenience when testing modded graphics.
		_surface = multiplyColorWithAlpha(pngDecoder.getSurface());
	} else {
		_surface = pngDecoder.getSurface()->convertTo(Gfx::Driver::getRGBAPixelFormat());
	}

	if (_originalWidth == 0 || _originalHeight == 0) {
		_originalWidth  = _surface->w;
		_originalHeight = _surface->h;
	}

	_bitmap = _gfx->createBitmap(_surface);
	_bitmap->setSamplingFilter(StarkSettings->getImageSamplingFilter());
	compactSurfaceIfSafe();

	return true;
}

Graphics::Surface *VisualImageXMG::multiplyColorWithAlpha(const Graphics::Surface *source) {
	const Graphics::Surface *surface = source;
	Graphics::Surface *convertedSurface = nullptr;

	if (source->format != Gfx::Driver::getRGBAPixelFormat()) {
		convertedSurface = source->convertTo(Gfx::Driver::getRGBAPixelFormat());
		surface = convertedSurface;
	}

	assert(surface->format == Gfx::Driver::getRGBAPixelFormat());

	Graphics::Surface *dest = new Graphics::Surface();
	dest->create(surface->w, surface->h, Gfx::Driver::getRGBAPixelFormat());

	for (int y = 0; y < surface->h; y++) {
		const uint8 *src = (const uint8 *)surface->getBasePtr(0, y);
		uint8 *dst = (uint8 *)dest->getBasePtr(0, y);

		for (int x = 0; x < surface->w; x++) {
			uint8 a, r, g, b;
			r = *src++;
			g = *src++;
			b = *src++;
			a = *src++;

			if (a != 0xFF) {
				r = (int) r * a / 255;
				g = (int) g * a / 255;
				b = (int) b * a / 255;
			}

			*dst++ = r;
			*dst++ = g;
			*dst++ = b;
			*dst++ = a;
		}
	}

	if (convertedSurface) {
		convertedSurface->free();
		delete convertedSurface;
	}

	return dest;
}

void VisualImageXMG::render(const Common::Point &position, bool useOffset) {
	render(position, useOffset, true);
}

void VisualImageXMG::render(const Common::Point &position, bool useOffset, bool unscaled) {
	Common::Point drawPos = useOffset ? position - _hotspot : position;

	if (!unscaled) {
		uint width = _gfx->scaleWidthOriginalToCurrent(_originalWidth);
		uint height = _gfx->scaleHeightOriginalToCurrent(_originalHeight);
		_surfaceRenderer->render(_bitmap, drawPos, width, height);
	} else {
		_surfaceRenderer->render(_bitmap, drawPos, _originalWidth, _originalHeight);
	}
}

void VisualImageXMG::setFadeLevel(float fadeLevel) {
	_surfaceRenderer->setFadeLevel(fadeLevel);
}

bool VisualImageXMG::isPointSolid(const Common::Point &point) const {
	if (_originalWidth < 32 || _originalHeight < 32) {
		return true; // Small images are always solid
	}

	if (!_surface && _hasSolidMask) {
		if (_solidMask.empty() || _surfaceW == 0 || _surfaceH == 0) {
			return true;
		}
		uint16 sx = (uint16)(point.x * _surfaceW / _originalWidth);
		uint16 sy = (uint16)(point.y * _surfaceH / _originalHeight);
		sx = CLIP<uint16>(sx, 0, _surfaceW - 1);
		sy = CLIP<uint16>(sy, 0, _surfaceH - 1);
		const uint32 bitIdx = (uint32)sy * _surfaceW + sx;
		return (_solidMask[bitIdx >> 3] & (1u << (bitIdx & 7u))) != 0;
	}

	assert(_surface);

	Common::Point scaledPoint;
	scaledPoint.x = point.x * _surface->w / _originalWidth;
	scaledPoint.y = point.y * _surface->h / _originalHeight;
	scaledPoint.x = CLIP<uint16>(scaledPoint.x, 0, _surface->w - 1);
	scaledPoint.y = CLIP<uint16>(scaledPoint.y, 0, _surface->h - 1);

	// Maybe implement this method in some other way to avoid having to keep the surface in memory
	const byte *ptr = (const byte *) _surface->getBasePtr(scaledPoint.x, scaledPoint.y);
	return *(ptr + 3) == 0xFF;
}

int VisualImageXMG::getWidth() const {
	return _originalWidth;
}

int VisualImageXMG::getHeight() const {
	return _originalHeight;
}

const Graphics::Surface *VisualImageXMG::getSurface() const {
	return _surface;
}

} // End of namespace Stark
