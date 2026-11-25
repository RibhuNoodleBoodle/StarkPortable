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

#include "engines/stark/gfx/pspgufade.h"
#include "engines/stark/gfx/pspgu.h"

#include <math.h>
#include <pspgu.h>

#include "backends/platform/psp/psp_gu_session.h"
#include "common/util.h"

namespace Stark {
namespace Gfx {

namespace {

struct FadeVertex2D {
	uint32 color;
	float x, y, z;
};

} // namespace

PspGuFadeRenderer::PspGuFadeRenderer(PspGuDriver *gfx) :
		FadeRenderer(),
		_gfx(gfx) {
}

PspGuFadeRenderer::~PspGuFadeRenderer() {
}

void PspGuFadeRenderer::render(float fadeVal) {
	if (!_gfx) {
		return;
	}
	const uint8 alpha = (uint8)CLIP<int32>((int32)roundf((1.0f - fadeVal) * 255.0f), 0, 255);
	if (alpha == 0) {
		return;
	}
	if (!_gfx->ensureFrameBegun()) {
		return;
	}
	PspGuSession *session = _gfx->getSession();
	if (!session || !session->hasDisplayListHeadroom(256)) {
		return;
	}

	const Common::Rect &rtVp = _gfx->getRtViewport();
	if (rtVp.width() <= 0 || rtVp.height() <= 0) {
		return;
	}

	FadeVertex2D *verts = static_cast<FadeVertex2D *>(session->allocVertices(2 * sizeof(FadeVertex2D), 16));
	if (!verts) {
		return;
	}

	// Premultiplied black with alpha = (1.0 - fadeVal) matches GL_ONE, GL_ONE_MINUS_SRC_ALPHA
	const uint32 abgr = ((uint32)alpha << 24);
	verts[0].color = abgr; verts[0].x = (float)rtVp.left;  verts[0].y = (float)rtVp.top;    verts[0].z = 0.0f;
	verts[1].color = abgr; verts[1].x = (float)rtVp.right; verts[1].y = (float)rtVp.bottom; verts[1].z = 0.0f;
	session->flushDataRange(verts, 2 * sizeof(FadeVertex2D));

	_gfx->applySceneViewport();
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuPixelMask(0x00000000);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0x00FFFFFF, 0);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, verts);
}

} // End of namespace Gfx
} // End of namespace Stark
