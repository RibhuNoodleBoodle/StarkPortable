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

#include "engines/stark/gfx/pspgusurface.h"
#include "engines/stark/gfx/pspgu.h"
#include "engines/stark/gfx/pspgubitmap.h"
#include "engines/stark/gfx/color.h"

#include <math.h>
#include <pspgu.h>

#include "backends/platform/psp/psp_gu_session.h"

namespace Stark {
namespace Gfx {

namespace {

struct SpriteVertex2D {
	float u, v;
	uint32 color;
	float x, y, z;
};

struct SolidVertex2D {
	uint32 color;
	float x, y, z;
};

} // namespace

PspGuSurfaceRenderer::PspGuSurfaceRenderer(PspGuDriver *gfx) :
		_gfx(gfx) {
}

PspGuSurfaceRenderer::~PspGuSurfaceRenderer() {
}

void PspGuSurfaceRenderer::prepare2DState() const {
	_gfx->applySceneViewport();
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_LIGHTING);
	pspGuDisableAllLights();
	sceGuDisable(GU_CLIP_PLANES);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuPixelMask(0xFF000000u);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0x00FFFFFF, 0);
}

bool PspGuSurfaceRenderer::clipQuadToViewport(float &x0, float &y0, float &x1, float &y1,
                                                float *u0, float *v0, float *u1, float *v1) const {
	const Common::Rect &rtVp = _gfx->getRtViewport();
	const float clipL = (float)rtVp.left;
	const float clipT = (float)rtVp.top;
	const float clipR = (float)rtVp.right;
	const float clipB = (float)rtVp.bottom;

	if (x1 <= x0 || y1 <= y0 || x1 <= clipL || x0 >= clipR || y1 <= clipT || y0 >= clipB) {
		return false;
	}

	const float origX0 = x0;
	const float origX1 = x1;
	const float origY0 = y0;
	const float origY1 = y1;
	const float invW = 1.0f / (origX1 - origX0);
	const float invH = 1.0f / (origY1 - origY0);

	const float origU0 = u0 ? *u0 : 0.0f;
	const float origU1 = u1 ? *u1 : 0.0f;
	const float origV0 = v0 ? *v0 : 0.0f;
	const float origV1 = v1 ? *v1 : 0.0f;

	if (x0 < clipL) {
		const float t = (clipL - origX0) * invW;
		x0 = clipL;
		if (u0) {
			*u0 = origU0 + (origU1 - origU0) * t;
		}
	}
	if (x1 > clipR) {
		const float t = (clipR - origX0) * invW;
		x1 = clipR;
		if (u1) {
			*u1 = origU0 + (origU1 - origU0) * t;
		}
	}
	if (y0 < clipT) {
		const float t = (clipT - origY0) * invH;
		y0 = clipT;
		if (v0) {
			*v0 = origV0 + (origV1 - origV0) * t;
		}
	}
	if (y1 > clipB) {
		const float t = (clipB - origY0) * invH;
		y1 = clipB;
		if (v1) {
			*v1 = origV0 + (origV1 - origV0) * t;
		}
	}

	return (x1 > x0) && (y1 > y0);
}

void PspGuSurfaceRenderer::render(const Bitmap *bitmap, const Common::Point &dest) {
	if (!bitmap) {
		return;
	}
	render(bitmap, dest, bitmap->width(), bitmap->height());
}

void PspGuSurfaceRenderer::render(const Bitmap *bitmap, const Common::Point &dest, uint dstW, uint dstH) {
	if (!_gfx || !bitmap || dstW == 0 || dstH == 0 || bitmap->width() == 0 || bitmap->height() == 0) {
		return;
	}

	const PspGuBitmap *guBitmap = static_cast<const PspGuBitmap *>(bitmap);
	const uint32 tileCount = guBitmap->getTileCount();
	if (tileCount == 0) {
		return;
	}

	if (!_gfx->ensureFrameBegun()) {
		return;
	}

	PspGuSession *session = _gfx->getSession();
	if (!session) {
		return;
	}

	float rtX0 = 0.0f, rtY0 = 0.0f, rtX1 = 0.0f, rtY1 = 0.0f;
	_gfx->mapLogical2DRectToRt(dest.x, dest.y, dstW, dstH,
	                           _noScalingOverride, _snapToGrid,
	                           rtX0, rtY0, rtX1, rtY1);

	const float rtW = rtX1 - rtX0;
	const float rtH = rtY1 - rtY0;
	if (rtW <= 0.0f || rtH <= 0.0f) {
		return;
	}

	const uint32 srcW = bitmap->width();
	const uint32 srcH = bitmap->height();
	const bool applyFade = fabsf(_fadeLevel) >= 0.002f;
	const uint8 fadeByte = applyFade ? (uint8)CLIP<int32>((int32)roundf(fabsf(_fadeLevel) * 255.0f), 0, 255) : 0;
	const bool hasAlphaFade = applyFade && fadeByte > 0 && guBitmap->ensureAlphaMasks();

	prepare2DState();
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	const int filter = (guBitmap->getSamplingFilter() == Bitmap::kNearest) ? GU_NEAREST : GU_LINEAR;
	sceGuTexFilter(filter, filter);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);

	for (uint32 ti = 0; ti < tileCount; ++ti) {
		const PspGuBitmap::Tile &tile = guBitmap->getTile(ti);
		if (!tile.pixels) {
			continue;
		}

		float qx0 = (tile.srcX == 0) ? rtX0 : rtX0 + rtW * ((float)tile.srcX / (float)srcW);
		float qx1 = (tile.srcX + tile.contentW == srcW) ? rtX1 : rtX0 + rtW * ((float)(tile.srcX + tile.contentW) / (float)srcW);
		float qy0 = (tile.srcY == 0) ? rtY0 : rtY0 + rtH * ((float)tile.srcY / (float)srcH);
		float qy1 = (tile.srcY + tile.contentH == srcH) ? rtY1 : rtY0 + rtH * ((float)(tile.srcY + tile.contentH) / (float)srcH);

		float u0 = (float)tile.padL;
		float u1 = (float)(tile.padL + tile.contentW);
		float v0 = (float)tile.padT;
		float v1 = (float)(tile.padT + tile.contentH);

		if (!clipQuadToViewport(qx0, qy0, qx1, qy1, &u0, &v0, &u1, &v1)) {
			continue;
		}

		if (!session->hasDisplayListHeadroom(hasAlphaFade ? 512 : 256)) {
			warning("PspGuSurfaceRenderer: display list headroom exhausted");
			break;
		}

		SpriteVertex2D *verts = static_cast<SpriteVertex2D *>(session->allocVertices(2 * sizeof(SpriteVertex2D), 16));
		if (!verts) {
			warning("PspGuSurfaceRenderer: vertex arena exhausted");
			break;
		}

		verts[0].u = u0; verts[0].v = v0; verts[0].color = 0xFFFFFFFFu; verts[0].x = qx0; verts[0].y = qy0; verts[0].z = 0.0f;
		verts[1].u = u1; verts[1].v = v1; verts[1].color = 0xFFFFFFFFu; verts[1].x = qx1; verts[1].y = qy1; verts[1].z = 0.0f;
		session->flushDataRange(verts, 2 * sizeof(SpriteVertex2D));

		sceGuTexMode(tile.psm, 0, 0, 0);
		sceGuTexImage(0, tile.texW, tile.texH, tile.stride, tile.pixels);
		sceGuTexFlush();
		sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, verts);

		if (hasAlphaFade && tile.alphaMask) {
			SpriteVertex2D *fVerts = static_cast<SpriteVertex2D *>(session->allocVertices(2 * sizeof(SpriteVertex2D), 16));
			if (fVerts) {
				const uint32 fadeAbgr = 0xFF000000u | ((uint32)fadeByte << 16) | ((uint32)fadeByte << 8) | (uint32)fadeByte;
				fVerts[0] = verts[0]; fVerts[0].color = fadeAbgr;
				fVerts[1] = verts[1]; fVerts[1].color = fadeAbgr;
				session->flushDataRange(fVerts, 2 * sizeof(SpriteVertex2D));

				sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
				sceGuClutLoad(32, PspGuBitmap::getAlphaFadeClut());
				sceGuTexMode(GU_PSM_T8, 0, 0, 0);
				sceGuTexImage(0, tile.texW, tile.texH, tile.stride, tile.alphaMask);
				sceGuTexFlush();
				sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
				sceGuPixelMask(0xFF000000u);
				sceGuBlendFunc(_fadeLevel >= 0.0f ? GU_ADD : GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF);
				sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, fVerts);

				sceGuPixelMask(0xFF000000u);
				sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
				sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0x00FFFFFF, 0);
			}
		}
	}

	guBitmap->markDrawn(_gfx->getFrameSeq());
}

void PspGuSurfaceRenderer::fill(const Color &color, const Common::Point &dest, uint dstW, uint dstH) {
	if (!_gfx || dstW == 0 || dstH == 0) {
		return;
	}
	if (!_gfx->ensureFrameBegun()) {
		return;
	}
	PspGuSession *session = _gfx->getSession();
	if (!session || !session->hasDisplayListHeadroom(256)) {
		return;
	}

	float rtX0 = 0.0f, rtY0 = 0.0f, rtX1 = 0.0f, rtY1 = 0.0f;
	_gfx->mapLogical2DRectToRt(dest.x, dest.y, dstW, dstH,
	                           _noScalingOverride, _snapToGrid,
	                           rtX0, rtY0, rtX1, rtY1);
	if (!clipQuadToViewport(rtX0, rtY0, rtX1, rtY1, nullptr, nullptr, nullptr, nullptr)) {
		return;
	}

	SolidVertex2D *verts = static_cast<SolidVertex2D *>(session->allocVertices(2 * sizeof(SolidVertex2D), 16));
	if (!verts) {
		return;
	}

	const int32 fadeDelta = (int32)roundf(_fadeLevel * (float)color.a);
	const uint8 r = (uint8)CLIP<int32>((int32)color.r + fadeDelta, 0, 255);
	const uint8 g = (uint8)CLIP<int32>((int32)color.g + fadeDelta, 0, 255);
	const uint8 b = (uint8)CLIP<int32>((int32)color.b + fadeDelta, 0, 255);
	const uint32 abgr = ((uint32)color.a << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;

	verts[0].color = abgr; verts[0].x = rtX0; verts[0].y = rtY0; verts[0].z = 0.0f;
	verts[1].color = abgr; verts[1].x = rtX1; verts[1].y = rtY1; verts[1].z = 0.0f;
	session->flushDataRange(verts, 2 * sizeof(SolidVertex2D));

	prepare2DState();
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, verts);
}

} // End of namespace Gfx
} // End of namespace Stark
