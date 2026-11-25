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

#include "engines/stark/gfx/pspgu.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <pspgu.h>
#include <pspgum.h>

#include "backends/platform/psp/psp_gu_session.h"

#include "engines/stark/gfx/pspguactor.h"
#include "engines/stark/gfx/pspgubitmap.h"
#include "engines/stark/gfx/pspgufade.h"
#include "engines/stark/gfx/pspguprop.h"
#include "engines/stark/gfx/pspgusurface.h"
#include "engines/stark/gfx/pspgutexture.h"

#include "common/debug.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

namespace {

struct SmokeVertex3D {
	uint32 color;
	float x;
	float y;
	float z;
};

struct SmokeSpriteVertex2D {
	float u;
	float v;
	uint32 color;
	float x;
	float y;
	float z;
};

const char *const kSmokeOwner = "stark-pspgu";

} // namespace

PspGuDriver::PspGuDriver() :
		_session(nullptr),
		_sessionReady(false),
		_clearRequested(false),
		_smokeFrameIndex(0),
		_unimplementedMask(0) {
}

PspGuDriver::~PspGuDriver() {
	if (_session && _sessionReady) {
		const PspGuSessionStats &st = _session->getStats();
		debug("STARK PSP Step01 shutdown: frames=%u starts=%u finishes=%u swaps=%u peakList=%u peakVerts=%u",
		      _smokeFrameIndex, st.starts, st.finishes, st.swaps, st.peakListBytes, st.peakVertexBytes);
		_session->release();
	}
	_sessionReady = false;
	_session = nullptr;
}

void PspGuDriver::verifyStep01SessionContracts() {
	if (!_session) {
		return;
	}

	// 1. Concurrent second acquire must fail without disturbing active owner.
	const bool dupOk = _session->acquire("stark-pspgu-duplicate");
	const bool activeAfterDup = _session->isActive();

	// 2. Partial-init / injected allocation failure unwind must leak 0 bytes and stay inactive.
	_session->release();
	const int usedBeforeFail = mallinfo().uordblks;
	_session->setInjectAllocFail(true);
	const bool failOk = _session->acquire("stark-pspgu-alloc-fail");
	const int usedAfterFail = mallinfo().uordblks;
	const bool activeAfterFail = _session->isActive();

	// 3. Re-acquire cleanly and verify suspend/resume roundtrip hooks.
	_sessionReady = _session->acquire(kSmokeOwner);
	if (_sessionReady) {
		_session->onSuspend();
		_session->onResume();
	}

	const PspGuSessionStats &st = _session->getStats();
	debug("[STARK-PSP-STEP01] contracts: dupReject=%s(active=%s) allocFailUnwind=%s(active=%s,deltaB=%d) suspend=%u resume=%u acquires=%u releases=%u",
	      !dupOk ? "OK" : "FAIL",
	      activeAfterDup ? "true" : "false",
	      !failOk ? "OK" : "FAIL",
	      activeAfterFail ? "true" : "false",
	      usedAfterFail - usedBeforeFail,
	      st.suspendCount,
	      st.resumeCount,
	      st.acquires,
	      st.releases);
}

void PspGuDriver::init() {
	_session = PspGuSession::getBackendSession();
	if (!_session) {
		warning("PspGuDriver: PSP backend native GU session unavailable");
		_sessionReady = false;
		return;
	}

	if (!_session->acquire(kSmokeOwner)) {
		warning("PspGuDriver: failed to acquire native GU session cleanly");
		_sessionReady = false;
		return;
	}

	_sessionReady = true;
	_computeLights = false;
	computeScreenViewport();
	_viewport = _screenViewport;

	verifyStep01SessionContracts();

	debug("[STARK-PSP-STEP01] PspGuDriver initialized (session=%s, listCap=%u, reserve=%u, vertexArena=%u)",
	      _sessionReady ? "ready" : "unavailable",
	      PspGuSession::kDisplayListSize,
	      PspGuSession::kDisplayListReserve,
	      PspGuSession::kVertexArenaSize);
}

void PspGuDriver::setScreenViewport(bool noScaling) {
	computeScreenViewport();
	_viewport = _screenViewport;
}

void PspGuDriver::setViewport(const Common::Rect &rect) {
	_viewport = rect;
}

bool PspGuDriver::ensureFrameBegun() {
	if (!_session || !_sessionReady || !_session->isActive()) {
		return false;
	}
	if (_session->isFrameOpen()) {
		return true;
	}
	return _session->beginFrame();
}

void PspGuDriver::clearScreen() {
	_clearRequested = true;
	if (!ensureFrameBegun()) {
		return;
	}
	if (!_session->hasDisplayListHeadroom(256)) {
		warning("PspGuDriver: insufficient display list headroom during clearScreen");
		return;
	}
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuClearColor(0xFF000000);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void PspGuDriver::drawStep01SmokeScene() {
	if (!ensureFrameBegun()) {
		return;
	}
	if (!_session->hasDisplayListHeadroom(2048)) {
		warning("PspGuDriver: insufficient display list headroom for Step 01 smoke scene");
		return;
	}

	// Ensure black pillarbox bars [0,59) and [421,480) and active 362x272 slate field
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuClearColor(0xFF000000);
	sceGuClear(GU_COLOR_BUFFER_BIT);

	sceGuScissor(59, 0, 362, 272);
	sceGuClearColor(0xFF281810);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

	// 1. 3D transformed triangle via sceGumMatrixMode + sceGumLoadMatrix + sceGumDrawArray(GU_TRANSFORM_3D)
	sceGuOffset(2048 - 240, 2048 - 136);
	sceGuViewport(2048, 2048, 362, -272);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_BLEND);
	sceGuShadeModel(GU_SMOOTH);

	alignas(16) ScePspFMatrix4 proj;
	memset(&proj, 0, sizeof(proj));
	proj.x.x = 272.0f / 362.0f;
	proj.y.y = 1.0f;
	proj.z.z = 1.0f;
	proj.w.w = 1.0f;

	sceGumMatrixMode(GU_PROJECTION);
	sceGumLoadMatrix(&proj);

	sceGumMatrixMode(GU_VIEW);
	sceGumLoadIdentity();

	const float angle = (float)_smokeFrameIndex * 0.35f;
	const float c = cosf(angle);
	const float s = sinf(angle);
	alignas(16) ScePspFMatrix4 model;
	memset(&model, 0, sizeof(model));
	model.x.x = c;
	model.x.y = s;
	model.y.x = -s;
	model.y.y = c;
	model.z.z = 1.0f;
	model.w.w = 1.0f;

	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&model);

	SmokeVertex3D *triVerts = static_cast<SmokeVertex3D *>(
			_session->allocVertices(3 * sizeof(SmokeVertex3D), 16));
	if (!triVerts) {
		warning("PspGuDriver: vertex arena exhausted in Step 01 triangle");
		return;
	}

	const bool oddFrame = (_smokeFrameIndex & 1) != 0;
	triVerts[0].color = oddFrame ? 0xFFFF40FF : 0xFFFFFF40;
	triVerts[0].x = 0.0f;
	triVerts[0].y = 0.65f;
	triVerts[0].z = 0.0f;

	triVerts[1].color = oddFrame ? 0xFF40B0FF : 0xFF40FF80;
	triVerts[1].x = -0.60f;
	triVerts[1].y = -0.50f;
	triVerts[1].z = 0.0f;

	triVerts[2].color = oddFrame ? 0xFFFFFFFF : 0xFF40D0FF;
	triVerts[2].x = 0.60f;
	triVerts[2].y = -0.50f;
	triVerts[2].z = 0.0f;

	sceGumDrawArray(GU_TRIANGLES,
	                GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
	                3, nullptr, triVerts);

	// 2. Through-mode 2D sprite (GU_SPRITES | GU_TRANSFORM_2D) inside active pillarboxed frame
	sceGuViewport(2048, 2048, 480, 272);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);

	uint32 *texPixels = static_cast<uint32 *>(_session->allocVertices(16 * 16 * sizeof(uint32), 16));
	SmokeSpriteVertex2D *spriteVerts = static_cast<SmokeSpriteVertex2D *>(
			_session->allocVertices(2 * sizeof(SmokeSpriteVertex2D), 16));
	if (!texPixels || !spriteVerts) {
		warning("PspGuDriver: vertex arena exhausted in Step 01 sprite");
		return;
	}

	for (uint32 py = 0; py < 16; ++py) {
		for (uint32 px = 0; px < 16; ++px) {
			const bool checker = ((px >> 2) ^ (py >> 2)) & 1;
			texPixels[py * 16 + px] = checker ? 0xFFFFFFFF : 0xFF303030;
		}
	}
	_session->flushDataRange(texPixels, 16 * 16 * sizeof(uint32));
	sceGuTexImage(0, 16, 16, 16, texPixels);

	const uint32 tint = oddFrame ? 0xFFFF8040 : 0xFF40FFE0;
	spriteVerts[0].u = 0.0f;
	spriteVerts[0].v = 0.0f;
	spriteVerts[0].color = tint;
	spriteVerts[0].x = 75.0f;
	spriteVerts[0].y = 20.0f;
	spriteVerts[0].z = 0.0f;

	spriteVerts[1].u = 16.0f;
	spriteVerts[1].v = 16.0f;
	spriteVerts[1].color = tint;
	spriteVerts[1].x = 139.0f;
	spriteVerts[1].y = 84.0f;
	spriteVerts[1].z = 0.0f;

	sceGuDrawArray(GU_SPRITES,
	                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
	                2, nullptr, spriteVerts);
}

void PspGuDriver::flipBuffer() {
	_clearRequested = false;
	if (!_session || !_sessionReady || !_session->isActive()) {
		return;
	}

	drawStep01SmokeScene();

	if (!_session->submitFrame(true)) {
		warning("PspGuDriver: submitFrame failed, unwinding cleanly");
		return;
	}
	_session->waitFrame();

	if (_smokeFrameIndex < 3 || (_smokeFrameIndex % 120) == 0) {
		const PspGuSessionStats &st = _session->getStats();
		debug("[STARK-PSP-STEP01] frame=%u starts=%u finishes=%u swaps=%u listB=%u/%u vertsB=%u/%u",
		      _smokeFrameIndex, st.starts, st.finishes, st.swaps,
		      st.lastListBytes, st.peakListBytes, st.lastVertexBytes, st.peakVertexBytes);
	}
	_smokeFrameIndex++;
}

Texture *PspGuDriver::createTexture() {
	return new PspGuTexture();
}

Bitmap *PspGuDriver::createBitmap(const Graphics::Surface *surface, const byte *palette) {
	return new PspGuBitmap(surface, palette);
}

VisualActor *PspGuDriver::createActorRenderer() {
	return new PspGuActorRenderer(this);
}

VisualProp *PspGuDriver::createPropRenderer() {
	return new PspGuPropRenderer(this);
}

SurfaceRenderer *PspGuDriver::createSurfaceRenderer() {
	return new PspGuSurfaceRenderer(this);
}

FadeRenderer *PspGuDriver::createFadeRenderer() {
	return new PspGuFadeRenderer(this);
}

Graphics::Surface *PspGuDriver::getViewportScreenshot(uint w, uint h) const {
	warning("STARK PSP Step01: getViewportScreenshot(%u, %u) unimplemented - returning blank surface", w, h);
	Graphics::Surface *surface = new Graphics::Surface();
	surface->create(w, h, getRGBAPixelFormat());
	if (surface->getPixels()) {
		memset(surface->getPixels(), 0, (size_t)surface->pitch * surface->h);
	}
	return surface;
}

void PspGuDriver::set3DMode() {
	// Step 01 baseline keeps 3D pipeline state setup inside the diagnostic/Step 02+ scene path.
}

bool PspGuDriver::computeLightsEnabled() {
	return _computeLights;
}

void PspGuDriver::reportUnimplemented(const char *methodName) {
	// Rate-limit warnings using a compact FNV-1a bucket so frame loops stay responsive.
	uint32 hash = 2166136261u;
	for (const char *p = methodName; p && *p; ++p) {
		hash ^= (uint8)*p;
		hash *= 16777619u;
	}
	const uint32 bit = 1u << (hash & 31u);
	if ((_unimplementedMask & bit) == 0) {
		_unimplementedMask |= bit;
		warning("STARK PSP Step01: %s is developmentally unavailable (unimplemented until Step 02+)",
		        methodName ? methodName : "unknown");
	}
}

} // End of namespace Gfx
} // End of namespace Stark
