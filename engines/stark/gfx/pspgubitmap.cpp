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

#include "engines/stark/gfx/pspgubitmap.h"
#include "engines/stark/gfx/pspgu.h"
#include "engines/stark/gfx/driver.h"

#include <malloc.h>
#include <string.h>
#include <pspgu.h>
#include <psputils.h>

#include "common/debug.h"
#include "graphics/pixelformat.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

namespace {

struct AxisChunk {
	uint16 start;
	uint16 content;
	uint16 padBefore;
	uint16 padAfter;
};

void buildAxisChunks(uint32 length, bool allowMidSplit, Common::Array<AxisChunk> &out) {
	out.clear();
	if (length == 0) {
		return;
	}

	uint32 pos = 0;
	while (pos < length) {
		const uint32 padL = (pos > 0) ? 1 : 0;
		const uint32 remain = length - pos;
		uint32 content;
		if (padL + remain > 512) {
			content = 510;
		} else if (allowMidSplit && (padL + remain) > 256 && (padL + remain) <= 383) {
			const uint32 firstFit = 255 - padL; // padL + content + padR(1) == 256
			if (remain > firstFit && (remain - firstFit + 1) <= 128) {
				content = firstFit;
			} else {
				content = remain;
			}
		} else {
			content = remain;
		}

		AxisChunk chunk;
		chunk.start = (uint16)pos;
		chunk.content = (uint16)content;
		chunk.padBefore = (uint16)padL;
		chunk.padAfter = (pos + content < length) ? 1 : 0;
		out.push_back(chunk);
		pos += content;
	}
}

void writebackCacheAligned(const void *ptr, uint32 bytes) {
	if (!ptr || bytes == 0) {
		return;
	}
	const uintptr start = (uintptr)ptr & ~(uintptr)63;
	const uintptr end = ((uintptr)ptr + bytes + 63) & ~(uintptr)63;
	sceKernelDcacheWritebackRange((const void *)start, (unsigned int)(end - start));
}

template<typename TexelT>
void duplicateTileGuardBorders(TexelT *base, uint32 stride, uint32 texW, uint32 texH,
                               uint32 copyW, uint32 copyH) {
	if (copyW < texW) {
		for (uint32 row = 0; row < copyH; ++row) {
			TexelT *rowPtr = base + row * stride;
			rowPtr[copyW] = rowPtr[copyW - 1];
		}
	}
	if (copyH < texH) {
		const TexelT *srcLastRow = base + (copyH - 1) * stride;
		TexelT *dstGuardRow = base + copyH * stride;
		const uint32 guardCols = MIN<uint32>(texW, copyW + 1);
		memcpy(dstGuardRow, srcLastRow, guardCols * sizeof(TexelT));
	}
}

} // namespace

const uint32 *PspGuBitmap::getAlphaFadeClut() {
	static uint32 s_alphaClut[256] __attribute__((aligned(64)));
	static bool s_clutInitialized = false;
	if (!s_clutInitialized) {
		for (uint32 i = 0; i < 256; ++i) {
			s_alphaClut[i] = (i << 24) | 0x00FFFFFFu;
		}
		writebackCacheAligned(s_alphaClut, sizeof(s_alphaClut));
		s_clutInitialized = true;
	}
	return s_alphaClut;
}

namespace {

Common::Array<PspGuBitmap *> &getLiveBitmapsRegistry() {
	static Common::Array<PspGuBitmap *> s_liveBitmaps;
	return s_liveBitmaps;
}

} // namespace

uint32 PspGuBitmap::reclaimIdleAlphaMasks(uint32 currentFrameSeq, uint32 minBytes) {
	Common::Array<PspGuBitmap *> &reg = getLiveBitmapsRegistry();
	uint32 reclaimed = 0;
	for (uint32 i = 0; i < reg.size(); ++i) {
		PspGuBitmap *bmp = reg[i];
		if (!bmp) {
			continue;
		}
		if (bmp->_lastDrawnFrameSeq == 0 || bmp->_lastDrawnFrameSeq + 2 < currentFrameSeq) {
			reclaimed += bmp->releaseAlphaMasks();
			if (minBytes > 0 && reclaimed >= minBytes) {
				break;
			}
		}
	}
	return reclaimed;
}

PspGuBitmap::PspGuBitmap(const Graphics::Surface *surface, const byte *palette) :
		_samplingFilter(kLinear),
		_lastDrawnFrameSeq(0) {
	getLiveBitmapsRegistry().push_back(this);
	if (surface) {
		update(surface, palette);
	}
}

PspGuBitmap::~PspGuBitmap() {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);
	Common::Array<PspGuBitmap *> &reg = getLiveBitmapsRegistry();
	for (uint32 i = 0; i < reg.size(); ++i) {
		if (reg[i] == this) {
			reg.remove_at(i);
			break;
		}
	}
	releaseTiles();
}

uint32 PspGuBitmap::releaseAlphaMasks() {
	uint32 freed = 0;
	for (uint32 i = 0; i < _tiles.size(); ++i) {
		if (_tiles[i].alphaMask) {
			pspGuTrackAuxBytes(-(int32)_tiles[i].alphaByteSize);
			free(_tiles[i].alphaMask);
			freed += _tiles[i].alphaByteSize;
			_tiles[i].alphaMask = nullptr;
			_tiles[i].alphaByteSize = 0;
		}
	}
	return freed;
}

void PspGuBitmap::releaseTiles() {
	for (uint32 i = 0; i < _tiles.size(); ++i) {
		if (_tiles[i].pixels) {
			pspGuTrackBitmapBytes(-(int32)_tiles[i].byteSize);
			free(_tiles[i].pixels);
			_tiles[i].pixels = nullptr;
		}
		if (_tiles[i].alphaMask) {
			pspGuTrackAuxBytes(-(int32)_tiles[i].alphaByteSize);
			free(_tiles[i].alphaMask);
			_tiles[i].alphaMask = nullptr;
		}
	}
	_tiles.clear();
}

void PspGuBitmap::bind() const {
	if (_tiles.empty() || !_tiles[0].pixels) {
		return;
	}
	const Tile &t = _tiles[0];
	sceGuTexMode(t.psm, 0, 0, 0);
	sceGuTexImage(0, t.texW, t.texH, t.stride, t.pixels);
	const int filter = (_samplingFilter == kNearest) ? GU_NEAREST : GU_LINEAR;
	sceGuTexFilter(filter, filter);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFlush();
}

void PspGuBitmap::update(const Graphics::Surface *surface, const byte *palette) {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);

	if (!surface || surface->w <= 0 || surface->h <= 0 || !surface->getPixels()) {
		releaseTiles();
		_width = 0;
		_height = 0;
		return;
	}

	const uint32 newW = (uint32)surface->w;
	const uint32 newH = (uint32)surface->h;
	int targetPsm;
	if (_width == newW && _height == newH && !_tiles.empty() && _tiles[0].psm == GU_PSM_5650 &&
	    !palette && newW == 640 && newH >= 180 && surface->format == Driver::getRGBAPixelFormat()) {
		// Full-width opaque video/background updates keep 5650 without scanning every pixel for alpha.
		const uint32 tl = *(const uint32 *)surface->getBasePtr(0, 0);
		const uint32 mid = *(const uint32 *)surface->getBasePtr(newW >> 1, newH >> 1);
		const uint32 br = *(const uint32 *)surface->getBasePtr(newW - 1, newH - 1);
		if (((tl & mid & br) & 0xFF000000u) == 0xFF000000u) {
			targetPsm = GU_PSM_5650;
		} else {
			targetPsm = pspGuClassifySurfacePsm(surface, palette);
		}
	} else {
		targetPsm = pspGuClassifySurfacePsm(surface, palette);
	}
	const uint32 bpp = (targetPsm == GU_PSM_8888) ? 4 : 2;

	Common::Array<AxisChunk> xChunks;
	Common::Array<AxisChunk> yChunks;
	buildAxisChunks(newW, newH >= 64, xChunks);
	buildAxisChunks(newH, newW >= 64, yChunks);

	const uint32 neededTiles = xChunks.size() * yChunks.size();
	bool canReuseInPlace = (_width == newW && _height == newH && _tiles.size() == neededTiles);
	if (canReuseInPlace) {
		uint32 idx = 0;
		for (uint32 yi = 0; yi < yChunks.size() && canReuseInPlace; ++yi) {
			for (uint32 xi = 0; xi < xChunks.size(); ++xi, ++idx) {
				const Tile &existing = _tiles[idx];
				if (existing.srcX != xChunks[xi].start || existing.srcY != yChunks[yi].start ||
				    existing.contentW != xChunks[xi].content || existing.contentH != yChunks[yi].content ||
				    existing.psm != targetPsm || !existing.pixels) {
					canReuseInPlace = false;
					break;
				}
			}
		}
	}

	Common::Array<Tile> targetTiles;
	if (!canReuseInPlace) {
		targetTiles.reserve(neededTiles);
		for (uint32 yi = 0; yi < yChunks.size(); ++yi) {
			for (uint32 xi = 0; xi < xChunks.size(); ++xi) {
				const AxisChunk &xc = xChunks[xi];
				const AxisChunk &yc = yChunks[yi];
				const uint32 copyW = xc.padBefore + xc.content + xc.padAfter;
				const uint32 copyH = yc.padBefore + yc.content + yc.padAfter;

				Tile t;
				t.srcX = xc.start;
				t.srcY = yc.start;
				t.contentW = xc.content;
				t.contentH = yc.content;
				t.padL = xc.padBefore;
				t.padT = yc.padBefore;
				t.texW = (uint16)pspGuNextPow2Clamped(copyW, 8, 512);
				t.texH = (uint16)pspGuNextPow2Clamped(copyH, 8, 512);
				t.stride = MAX<uint16>(16, t.texW);
				t.psm = targetPsm;
				t.byteSize = (uint32)t.stride * (uint32)t.texH * bpp;
				t.pixels = pspGuAllocAlignedWithRetry(64, t.byteSize);
				t.alphaMask = nullptr;
				t.alphaByteSize = 0;

				if (!t.pixels) {
					warning("PspGuBitmap: OOM allocating tile %ux%u (%u bytes, psm=%d) for %ux%u",
					        t.stride, t.texH, t.byteSize, t.psm, newW, newH);
					for (uint32 k = 0; k < targetTiles.size(); ++k) {
						pspGuTrackBitmapBytes(-(int32)targetTiles[k].byteSize);
						free(targetTiles[k].pixels);
					}
					return;
				}
				pspGuTrackBitmapBytes((int32)t.byteSize);
				targetTiles.push_back(t);
			}
		}
		releaseTiles();
		_tiles = targetTiles;
	}

	_width = newW;
	_height = newH;

	uint32 idx = 0;
	for (uint32 yi = 0; yi < yChunks.size(); ++yi) {
		for (uint32 xi = 0; xi < xChunks.size(); ++xi, ++idx) {
			const AxisChunk &xc = xChunks[xi];
			const AxisChunk &yc = yChunks[yi];
			Tile &t = _tiles[idx];

			const int32 srcStartX = (int32)t.srcX - (int32)t.padL;
			const int32 srcStartY = (int32)t.srcY - (int32)t.padT;
			const uint32 copyW = t.padL + t.contentW + xc.padAfter;
			const uint32 copyH = t.padT + t.contentH + yc.padAfter;

			pspGuCopySurfaceRectToTile(surface, palette, srcStartX, srcStartY,
			                           (int32)copyW, (int32)copyH, t.pixels, t.stride, t.psm);

			if (t.psm == GU_PSM_8888) {
				duplicateTileGuardBorders(static_cast<uint32 *>(t.pixels), t.stride, t.texW, t.texH, copyW, copyH);
			} else {
				duplicateTileGuardBorders(static_cast<uint16 *>(t.pixels), t.stride, t.texW, t.texH, copyW, copyH);
			}

			writebackCacheAligned(t.pixels, t.byteSize);

			if (t.alphaMask) {
				buildAlphaMaskForTile(t);
			}
		}
	}
}

void PspGuBitmap::buildAlphaMaskForTile(Tile &tile) const {
	if (!tile.alphaMask || !tile.pixels) {
		return;
	}
	if (tile.psm == GU_PSM_8888) {
		const uint32 *src = static_cast<const uint32 *>(tile.pixels);
		for (uint32 row = 0; row < tile.texH; ++row) {
			const uint32 *s = src + row * tile.stride;
			uint8 *d = tile.alphaMask + row * tile.stride;
			for (uint32 col = 0; col < tile.texW; ++col) {
				d[col] = (uint8)(s[col] >> 24);
			}
		}
	} else if (tile.psm == GU_PSM_4444) {
		const uint16 *src = static_cast<const uint16 *>(tile.pixels);
		for (uint32 row = 0; row < tile.texH; ++row) {
			const uint16 *s = src + row * tile.stride;
			uint8 *d = tile.alphaMask + row * tile.stride;
			for (uint32 col = 0; col < tile.texW; ++col) {
				d[col] = (uint8)(((s[col] >> 12) & 0xFu) * 17u);
			}
		}
	} else if (tile.psm == GU_PSM_5551) {
		const uint16 *src = static_cast<const uint16 *>(tile.pixels);
		for (uint32 row = 0; row < tile.texH; ++row) {
			const uint16 *s = src + row * tile.stride;
			uint8 *d = tile.alphaMask + row * tile.stride;
			for (uint32 col = 0; col < tile.texW; ++col) {
				d[col] = (s[col] & 0x8000u) ? 255 : 0;
			}
		}
	} else {
		memset(tile.alphaMask, 255, tile.alphaByteSize);
	}
	writebackCacheAligned(tile.alphaMask, tile.alphaByteSize);
}

bool PspGuBitmap::ensureAlphaMasks() const {
	for (uint32 i = 0; i < _tiles.size(); ++i) {
		Tile &t = _tiles[i];
		if (!t.pixels) {
			return false;
		}
		if (!t.alphaMask) {
			const uint32 sz = (uint32)t.stride * (uint32)t.texH;
			t.alphaMask = static_cast<uint8 *>(pspGuAllocAlignedWithRetry(64, sz));
			if (!t.alphaMask) {
				return false;
			}
			t.alphaByteSize = sz;
			pspGuTrackAuxBytes((int32)sz);
			buildAlphaMaskForTile(t);
		}
	}
	return !_tiles.empty();
}

void PspGuBitmap::setSamplingFilter(SamplingFilter filter) {
	_samplingFilter = filter;
}

Graphics::PixelFormat PspGuBitmap::getBestPixelFormat() const {
	return Driver::getRGBAPixelFormat();
}

} // End of namespace Gfx
} // End of namespace Stark
