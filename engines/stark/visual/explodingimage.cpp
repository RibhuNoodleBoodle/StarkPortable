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

#include "engines/stark/visual/explodingimage.h"

#include "common/random.h"
#include "graphics/surface.h"

#include "engines/stark/gfx/driver.h"
#include "engines/stark/gfx/surfacerenderer.h"
#include "engines/stark/gfx/bitmap.h"

#include "engines/stark/services/global.h"
#include "engines/stark/services/services.h"
#include "engines/stark/services/settings.h"

namespace Stark {

VisualExplodingImage::VisualExplodingImage(Gfx::Driver *gfx) :
		Visual(TYPE),
		_gfx(gfx),
		_bitmap(nullptr),
		_surface(nullptr),
		_originalWidth(0),
		_originalHeight(0),
		_srcWidth(0),
		_srcHeight(0) {
	_surfaceRenderer = _gfx->createSurfaceRenderer();
}

VisualExplodingImage::~VisualExplodingImage() {
	if (_surface) {
		_surface->free();
	}
	delete _surface;
	delete _bitmap;
	delete _surfaceRenderer;
}

void VisualExplodingImage::initFromSurface(const Graphics::Surface *surface, uint originalWidth, uint originalHeight) {
	assert(!_surface && !_bitmap);

	_originalWidth  = originalWidth;
	_originalHeight = originalHeight;
	_srcWidth = surface->w;
	_srcHeight = surface->h;
	_srcFormat = surface->format;

	if (_gfx->supportsNativeEffects()) {
		_bitmap = _gfx->createBitmap(surface);
		_bitmap->setSamplingFilter(StarkSettings->getImageSamplingFilter());
	} else {
		_surface = new Graphics::Surface();
		_surface->copyFrom(*surface);
		_bitmap = _gfx->createBitmap(_surface);
		_bitmap->setSamplingFilter(StarkSettings->getImageSamplingFilter());
	}

	// Create an explosion unit for each pixel in the surface
	_units.resize(_srcWidth * _srcHeight);

	Common::Point explosionCenter(_srcWidth / 2, _srcHeight / 2);
	Common::Point explosionAmplitude(48, 16);
	explosionAmplitude.x *= _srcWidth / (float)originalWidth;
	explosionAmplitude.y *= _srcHeight / (float)originalHeight;

	uint index = 0;
	for (uint y = 0; y < _srcHeight; y++) {
		for (uint x = 0; x < _srcWidth; x++, index++) {
			_units[index].setPosition(x, y);
			_units[index].setExplosionSettings(explosionCenter, explosionAmplitude, _srcWidth / (float)originalWidth);
			_units[index].setColor(*static_cast<const uint32 *>(surface->getBasePtr(x, y)), surface->format);
		}
	}
}

void VisualExplodingImage::render(const Common::Point &position) {
	if (_gfx->supportsNativeEffects()) {
		bool stillActive = true;
		for (uint i = 0; i < _units.size(); i++) {
			_units[i].update();
			if (_units[i]._stillImageTimeRemaining <= 0) {
				stillActive = false;
			}
		}
		if (stillActive) {
			_surfaceRenderer->render(_bitmap, position, _originalWidth, _originalHeight);
			return;
		}

		Common::Array<Gfx::Driver::EffectQuad> quads;
		const float invScaleX = _srcWidth > 0 ? (_originalWidth / (float)_srcWidth) : 1.0f;
		const float invScaleY = _srcHeight > 0 ? (_originalHeight / (float)_srcHeight) : 1.0f;
		quads.reserve(_units.size());
		for (uint i = 0; i < _units.size(); i++) {
			const ExplosionUnit &u = _units[i];
			if (u._position.getX() <= 1.f || u._position.getX() >= _srcWidth - 1 ||
			    u._position.getY() <= 1.f || u._position.getY() >= _srcHeight - 1) {
				continue;
			}
			if (u._stillImageTimeRemaining <= 0 && u._position.getDistanceTo(u._center) <= 2.f) {
				continue;
			}
			byte a, r, g, b;
			_srcFormat.colorToARGB(u._mainColor, a, r, g, b);
			if (a == 0) {
				continue;
			}
			const uint32 abgrMain = ((uint32)a << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
			_srcFormat.colorToARGB(u._darkColor, a, r, g, b);
			const uint32 abgrDark = ((uint32)a << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;

			const float cx = floorf(u._position.getX()) * invScaleX;
			const float cy = floorf(u._position.getY()) * invScaleY;
			quads.push_back({ cx - invScaleX, cy, cx + 2.0f * invScaleX, cy + invScaleY, abgrDark, true });
			quads.push_back({ cx, cy - invScaleY, cx + invScaleX, cy + 2.0f * invScaleY, abgrDark, true });
			quads.push_back({ cx, cy, cx + invScaleX, cy + invScaleY, abgrMain, true });
		}
		_gfx->renderEffectQuads(position, Common::Point(_originalWidth, _originalHeight), quads.begin(), quads.size());
		return;
	}

	// Fill with transparent color
	_surface->fillRect(Common::Rect(_surface->w, _surface->h), 0);

	for (uint i = 0; i < _units.size(); i++) {
		_units[i].update();
		_units[i].draw(_surface);
	}

	_bitmap->update(_surface);
	_surfaceRenderer->render(_bitmap, position, _originalWidth, _originalHeight);
}

VisualExplodingImage::ExplosionUnit::ExplosionUnit() :
		_scale(1.f),
		_stillImageTimeRemaining(33 * 33),
		_explosionFastAccelerationTimeRemaining(25 * 33),
		_mainColor(0),
		_darkColor(0) {

}

void VisualExplodingImage::ExplosionUnit::setPosition(int x, int y) {
	_position = Math::Vector2d(x, y);
}

void VisualExplodingImage::ExplosionUnit::setExplosionSettings(const Common::Point &center, const Common::Point &amplitude, float scale) {
	_center = Math::Vector2d(center.x, center.y);

	_speed.setX(cos(StarkRandomSource->getRandomNumber((float)M_PI * 100)) * (float)amplitude.x);
	_speed.setY(sin(StarkRandomSource->getRandomNumber((float)M_PI * 100)) * (float)amplitude.y);

	// Really? ensuring all fragments go in the same direction?
	float magnitude = _position.getDistanceTo(_speed);
	_speed -= _position;
	_speed = _speed / _speed.getMagnitude() * -magnitude;
	_scale = scale;
}

void VisualExplodingImage::ExplosionUnit::setColor(uint32 color, const Graphics::PixelFormat &format) {
	_mainColor = color;

	byte a, r, g, b;
	format.colorToARGB(color, a, r, g, b);
	r >>= 1;
	g >>= 1;
	b >>= 1;

	_darkColor = format.ARGBToColor(a, r, g, b);
}

void VisualExplodingImage::ExplosionUnit::update() {
	if (_stillImageTimeRemaining > 0) {
		_stillImageTimeRemaining -= StarkGlobal->getMillisecondsPerGameloop();
		return;
	}

	if (_position.getDistanceTo(_center) <= 1.f * _scale) {
		// Units near the center stay there (to make it look like they enter the chest)
		return;
	}

	Math::Vector2d speed = _speed.getNormalized() * 0.6f * _scale;
	_position += speed;

	// Update the acceleration to units move towards the center
	Math::Vector2d acceleration = _center - _position;
	if (_explosionFastAccelerationTimeRemaining > 0) {
		acceleration *= 3.0f;
		_explosionFastAccelerationTimeRemaining -= StarkGlobal->getMillisecondsPerGameloop();
	}

	_speed += acceleration;
	_speed -= speed * 2.5f;
}

void VisualExplodingImage::ExplosionUnit::draw(Graphics::Surface *surface) {
	if (_position.getX() <= 1.f || _position.getX() >= surface->w - 1
			|| _position.getY() <= 1.f || _position.getY() >= surface->h - 1) {
		return; // Ignore units outside of the surface
	}

	if (_stillImageTimeRemaining <= 0 && _position.getDistanceTo(_center) <= 2.f) {
		return; // Ignore units close to the center (to make it look like they enter the chest)
	}

	uint32 *pixel = static_cast<uint32 *>(surface->getBasePtr(_position.getX(), _position.getY() - 1));
	*pixel = _darkColor;

	pixel = static_cast<uint32 *>(surface->getBasePtr(_position.getX() - 1, _position.getY()));
	*pixel++ = _darkColor;
	*pixel++ = _mainColor;
	*pixel = _darkColor;

	pixel = static_cast<uint32 *>(surface->getBasePtr(_position.getX(), _position.getY() + 1));
	*pixel = _darkColor;
}

} // End of namespace Stark
