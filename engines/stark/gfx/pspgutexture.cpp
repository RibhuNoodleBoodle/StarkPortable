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

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "engines/stark/gfx/pspgutexture.h"
#include "engines/stark/gfx/pspgu.h"

#include <malloc.h>
#include <string.h>
#include <pspgu.h>
#include <psputils.h>

#include "common/debug.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

namespace {

bool isPow2U32(uint32 v) {
	return v != 0 && (v & (v - 1)) == 0;
}

void writebackRange64(const void *ptr, uint32 bytes) {
	if (!ptr || bytes == 0) {
		return;
	}
	const uintptr start = (uintptr)ptr & ~(uintptr)63;
	const uintptr end = ((uintptr)ptr + bytes + 63) & ~(uintptr)63;
	sceKernelDcacheWritebackRange((const void *)start, (unsigned int)(end - start));
}

} // namespace

PspGuTexture::PspGuTexture() :
		_clut(nullptr),
		_clutBytes(0),
		_requestedLevels(0),
		_uploadedLevels(0),
		_baseWidth(0),
		_baseHeight(0),
		_isIndexedT8(false),
		_lastBoundFrameSeq(0) {
	memset(_levels, 0, sizeof(_levels));
}

PspGuTexture::~PspGuTexture() {
	PspGuDriver::ensureResourceSafeToModify(_lastBoundFrameSeq);
	releaseLevels();
}

void PspGuTexture::releaseClut() {
	if (_clut) {
		pspGuTrackTextureBytes(-(int32)_clutBytes);
		free(_clut);
		_clut = nullptr;
		_clutBytes = 0;
	}
}

void PspGuTexture::releaseLevels() {
	for (uint32 i = 0; i < kMaxMipLevels; ++i) {
		if (_levels[i].data) {
			pspGuTrackTextureBytes(-(int32)_levels[i].byteSize);
			free(_levels[i].data);
		}
		memset(&_levels[i], 0, sizeof(MipLevel));
	}
	releaseClut();
	_uploadedLevels = 0;
	_baseWidth = 0;
	_baseHeight = 0;
	_isIndexedT8 = false;
}

void PspGuTexture::bind() const {
	bindWrapped(GU_REPEAT, GU_REPEAT, true);
}

void PspGuTexture::bindWrapped(int wrapU, int wrapV, bool linearFilter) const {
	if (_uploadedLevels == 0 || !_levels[0].data) {
		return;
	}

	uint32 validLevels = 0;
	while (validLevels < _uploadedLevels && _levels[validLevels].data) {
		++validLevels;
	}
	if (validLevels == 0) {
		return;
	}
	_lastBoundFrameSeq = PspGuDriver::getCurrentFrameSeq();

	if (_isIndexedT8 && _clut) {
		sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
		sceGuClutLoad(32, _clut);
		sceGuTexMode(GU_PSM_T8, (int)(validLevels - 1), 0, 0);
	} else {
		sceGuTexMode(GU_PSM_8888, (int)(validLevels - 1), 0, 0);
	}

	for (uint32 l = 0; l < validLevels; ++l) {
		const MipLevel &mip = _levels[l];
		sceGuTexImage((int)l, mip.texW, mip.texH, mip.stride, mip.data);
	}
	sceGuTexWrap(wrapU, wrapV);
	const int minFilter = linearFilter ? (validLevels > 1 ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR) : GU_NEAREST;
	const int magFilter = linearFilter ? GU_LINEAR : GU_NEAREST;
	sceGuTexFilter(minFilter, magFilter);
	sceGuTexFlush();
}

void PspGuTexture::setLevelCount(uint32 count) {
	PspGuDriver::ensureResourceSafeToModify(_lastBoundFrameSeq);
	releaseLevels();
	_requestedLevels = MIN<uint32>(count, kMaxMipLevels);
}

void PspGuTexture::addLevel(uint32 level, const Graphics::Surface *surface, const byte *palette) {
	PspGuDriver::ensureResourceSafeToModify(_lastBoundFrameSeq);

	if (!surface || surface->w <= 0 || surface->h <= 0 || !surface->getPixels()) {
		return;
	}
	if (level >= kMaxMipLevels) {
		return;
	}
	if (_requestedLevels == 0) {
		_requestedLevels = kMaxMipLevels;
	}

	if (level == 0) {
		_baseWidth = (uint16)pspGuNextPow2Clamped((uint32)surface->w, 8, 512);
		_baseHeight = (uint16)pspGuNextPow2Clamped((uint32)surface->h, 8, 512);
		_isIndexedT8 = (palette != nullptr && surface->format.bytesPerPixel == 1 &&
		                surface->w <= 512 && surface->h <= 512 &&
		                isPow2U32((uint32)surface->w) && isPow2U32((uint32)surface->h));
		if (_isIndexedT8) {
			_clutBytes = 256 * sizeof(uint32);
			_clut = static_cast<uint32 *>(pspGuAllocAlignedWithRetry(64, _clutBytes));
			if (!_clut) {
				_clutBytes = 0;
				_isIndexedT8 = false;
			} else {
				for (uint32 i = 0; i < 256; ++i) {
					const byte r = palette[i * 3 + 0];
					const byte g = palette[i * 3 + 1];
					const byte b = palette[i * 3 + 2];
					_clut[i] = (0xFFu << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
				}
				writebackRange64(_clut, _clutBytes);
				pspGuTrackTextureBytes((int32)_clutBytes);
			}
		}
	} else if (_baseWidth == 0 || _baseHeight == 0) {
		_baseWidth = (uint16)pspGuNextPow2Clamped((uint32)surface->w << level, 8, 512);
		_baseHeight = (uint16)pspGuNextPow2Clamped((uint32)surface->h << level, 8, 512);
		_isIndexedT8 = false;
	}

	const uint16 dstW = MAX<uint16>(1, _baseWidth >> level);
	const uint16 dstH = MAX<uint16>(1, _baseHeight >> level);

	if (_isIndexedT8 && palette && surface->format.bytesPerPixel == 1 &&
	    (uint32)surface->w == dstW && (uint32)surface->h == dstH) {
		const uint16 stride = MAX<uint16>(16, dstW);
		const uint32 byteSize = (uint32)stride * (uint32)dstH;
		uint8 *data = static_cast<uint8 *>(pspGuAllocAlignedWithRetry(64, byteSize));
		if (!data) {
			warning("PspGuTexture: OOM allocating T8 mip %u (%ux%u, %u bytes)", level, stride, dstH, byteSize);
			if (level == 0 && _uploadedLevels == 0) {
				releaseClut();
				_isIndexedT8 = false;
				_baseWidth = 0;
				_baseHeight = 0;
			}
			return;
		}
		if (_levels[level].data) {
			pspGuTrackTextureBytes(-(int32)_levels[level].byteSize);
			free(_levels[level].data);
			memset(&_levels[level], 0, sizeof(MipLevel));
		}
		for (uint16 row = 0; row < dstH; ++row) {
			memcpy(data + (uint32)row * stride, surface->getBasePtr(0, row), dstW);
		}
		writebackRange64(data, byteSize);
		_levels[level].data = data;
		_levels[level].byteSize = byteSize;
		_levels[level].texW = dstW;
		_levels[level].texH = dstH;
		_levels[level].stride = stride;
		if (level + 1 > _uploadedLevels) {
			_uploadedLevels = level + 1;
		}
		pspGuTrackTextureBytes((int32)byteSize);
		return;
	}

	// Non-indexed DDS or mismatched dimensions fall back cleanly to RGBA8888 across levels
	if (_isIndexedT8 && level > 0) {
		// Wait: cannot mix T8 and 8888 inside one mipmap chain; skip unmatched trailing level
		return;
	}

	const uint16 stride = MAX<uint16>(4, dstW);
	const uint32 byteSize = (uint32)stride * (uint32)dstH * sizeof(uint32);
	uint32 *pixels = static_cast<uint32 *>(pspGuAllocAlignedWithRetry(64, byteSize));
	if (!pixels) {
		warning("PspGuTexture: OOM allocating mip %u (%ux%u, %u bytes)", level, stride, dstH, byteSize);
		if (level == 0 && _uploadedLevels == 0) {
			releaseClut();
			_baseWidth = 0;
			_baseHeight = 0;
		}
		return;
	}
	if (_levels[level].data) {
		pspGuTrackTextureBytes(-(int32)_levels[level].byteSize);
		free(_levels[level].data);
		memset(&_levels[level], 0, sizeof(MipLevel));
	}

	if ((uint32)surface->w == dstW && (uint32)surface->h == dstH) {
		pspGuCopySurfaceRectToRgba8888(surface, palette, 0, 0, dstW, dstH, pixels, stride);
	} else {
		pspGuResampleSurfaceToRgba8888(surface, palette, dstW, dstH, pixels, stride);
	}

	writebackRange64(pixels, byteSize);

	_levels[level].data = pixels;
	_levels[level].byteSize = byteSize;
	_levels[level].texW = dstW;
	_levels[level].texH = dstH;
	_levels[level].stride = stride;
	if (level + 1 > _uploadedLevels) {
		_uploadedLevels = level + 1;
	}
	pspGuTrackTextureBytes((int32)byteSize);
}

} // End of namespace Gfx
} // End of namespace Stark
