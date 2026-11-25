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

#include "engines/stark/scene.h"
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

struct ColorVertex2D {
	uint32 color;
	float x;
	float y;
	float z;
};

const char *const kNativeOwner = "stark-pspgu";
const uint32 kPaddingSentinelAbgr = 0xFFFF00FFu;

} // namespace

PspGuDriver::PspGuDriver() :
		_session(nullptr),
		_sessionReady(false),
		_clearRequested(false),
		_reverseDepth(true),
		_smokeFrameIndex(0),
		_unimplementedMask(0) {
}

PspGuDriver::~PspGuDriver() {
	if (_session && _sessionReady) {
		const PspGuSessionStats &st = _session->getStats();
		debug("STARK PSP Step02 shutdown: frames=%u sceneSegs=%u lcdPresents=%u starts=%u finishes=%u swaps=%u peakList=%u peakVerts=%u",
		      _smokeFrameIndex, st.sceneSegments, st.lcdPresents,
		      st.starts, st.finishes, st.swaps, st.peakListBytes, st.peakVertexBytes);
		_session->release();
	}
	_sessionReady = false;
	_session = nullptr;
}

void PspGuDriver::updateRtViewport() {
	const int32 rtLeft = (_viewport.left * (int32)PspGuSession::kSceneWidth) / kOriginalWidth;
	const int32 rtTop = (_viewport.top * (int32)PspGuSession::kSceneHeight) / kOriginalHeight;
	const int32 rtRight = (_viewport.right * (int32)PspGuSession::kSceneWidth) / kOriginalWidth;
	const int32 rtBottom = (_viewport.bottom * (int32)PspGuSession::kSceneHeight) / kOriginalHeight;
	_rtViewport = Common::Rect(
			CLIP<int32>(rtLeft, 0, PspGuSession::kSceneWidth),
			CLIP<int32>(rtTop, 0, PspGuSession::kSceneHeight),
			CLIP<int32>(rtRight, 0, PspGuSession::kSceneWidth),
			CLIP<int32>(rtBottom, 0, PspGuSession::kSceneHeight));
}

void PspGuDriver::applySceneViewport() const {
	const int32 rw = _rtViewport.width();
	const int32 rh = _rtViewport.height();
	if (rw <= 0 || rh <= 0) {
		return;
	}
	const int32 offX = 2048 - ((int32)PspGuSession::kSceneWidth / 2);
	const int32 offY = 2048 - ((int32)PspGuSession::kSceneHeight / 2);
	const int32 vpCenterX = offX + (_rtViewport.left + _rtViewport.right) / 2;
	const int32 vpCenterY = offY + (_rtViewport.top + _rtViewport.bottom) / 2;

	sceGuOffset(offX, offY);
	sceGuViewport(vpCenterX, vpCenterY, rw, -rh);
	sceGuScissor(_rtViewport.left, _rtViewport.top, rw, rh);
	sceGuEnable(GU_SCISSOR_TEST);
}

void PspGuDriver::applyDepthConfig(bool reverseDepth) const {
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_FALSE);
	if (reverseDepth) {
		sceGuDepthRange(65535, 0);
		sceGuClearDepth(0);
		sceGuDepthFunc(GU_GREATER);
	} else {
		sceGuDepthRange(0, 65535);
		sceGuClearDepth(65535);
		sceGuDepthFunc(GU_LESS);
	}
}

void PspGuDriver::mapLogical2DToRt(float lx, float ly, float &outX, float &outY) const {
	const float scaleX = _rtViewport.width() / (float)(_unscaledViewport.width() > 0 ? _unscaledViewport.width() : kOriginalWidth);
	const float scaleY = _rtViewport.height() / (float)(_unscaledViewport.height() > 0 ? _unscaledViewport.height() : kOriginalHeight);
	outX = (float)_rtViewport.left + lx * scaleX;
	outY = (float)_rtViewport.top + ly * scaleY;
}

void PspGuDriver::verifyStep02Contracts() {
	if (!_session) {
		return;
	}

	// 1. Concurrent second acquire must fail without disturbing active owner.
	const bool dupOk = _session->acquire("stark-pspgu-duplicate");
	const bool activeAfterDup = _session->isActive();

	// 2. Transactional eDRAM failure unwind + RAM failure unwind must leave 0 leaked bytes & launcher functional.
	_session->release();
	const int usedBeforeVramFail = mallinfo().uordblks;
	_session->setInjectVramFail(true);
	const bool vramFailOk = _session->acquire("stark-pspgu-vram-fail");
	const int usedAfterVramFail = mallinfo().uordblks;

	_session->setInjectAllocFail(true);
	const bool ramFailOk = _session->acquire("stark-pspgu-ram-fail");
	const int usedAfterRamFail = mallinfo().uordblks;
	const bool activeAfterFail = _session->isActive();

	// 3. Re-acquire cleanly and verify suspend/resume + depth/clip/readback/picking parity.
	_sessionReady = _session->acquire(kNativeOwner);
	if (!_sessionReady) {
		return;
	}
	_session->onSuspend();
	_session->onResume();
	_session->primeScenePadding(kPaddingSentinelAbgr);

	// 4. Reverse depth & forward depth verification + homogeneous clip + 4 corners check.
	bool revDepthPass = false;
	bool fwdDepthPass = false;
	bool cornersAndClipPass = false;
	bool midScreenshotPass = false;

	const uint32 readbackStride = 320;
	uint32 *readbackBuf = static_cast<uint32 *>(memalign(64, readbackStride * PspGuSession::kSceneHeight * sizeof(uint32)));
	if (readbackBuf && _session->beginFrame()) {
		setScreenViewport(false);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_LIGHTING);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_BLEND);
		sceGuShadeModel(GU_FLAT);

		alignas(16) ScePspFMatrix4 ident;
		memset(&ident, 0, sizeof(ident));
		ident.x.x = 1.0f;
		ident.y.y = 1.0f;
		ident.z.z = 1.0f;
		ident.w.w = 1.0f;
		sceGumMatrixMode(GU_PROJECTION);
		sceGumLoadMatrix(&ident);
		sceGumMatrixMode(GU_VIEW);
		sceGumLoadIdentity();
		sceGumMatrixMode(GU_MODEL);
		sceGumLoadIdentity();

		auto drawNdcTri = [&](uint32 col, float zVal, float scaleXY) {
			SmokeVertex3D *v = static_cast<SmokeVertex3D *>(_session->allocVertices(3 * sizeof(SmokeVertex3D), 16));
			v[0].color = col; v[0].x = 0.0f; v[0].y = scaleXY; v[0].z = zVal;
			v[1].color = col; v[1].x = -scaleXY; v[1].y = -scaleXY; v[1].z = zVal;
			v[2].color = col; v[2].x = scaleXY; v[2].y = -scaleXY; v[2].z = zVal;
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, v);
		};

		// Reverse depth: far red (+0.5) < near green (-0.5) > far blue (+0.8) => green wins
		applyDepthConfig(true);
		sceGuClearColor(0xFF080808);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
		drawNdcTri(0xFF0000FFu, 0.5f, 0.8f);  // Red far
		drawNdcTri(0xFF00FF00u, -0.5f, 0.8f); // Green near
		drawNdcTri(0xFFFF0000u, 0.8f, 0.8f);  // Blue deeper far
		if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
			const uint32 revPx = readbackBuf[120 * readbackStride + 160];
			revDepthPass = (revPx & 0x00FFFFFFu) == 0x0000FF00u;
		}

		// Forward depth: far red (+0.5) < near cyan (-0.5) => cyan wins
		if (_session->beginFrame()) {
			applySceneViewport();
			sceGuDisable(GU_TEXTURE_2D);
			sceGuDisable(GU_LIGHTING);
			sceGuDisable(GU_CULL_FACE);
			sceGuDisable(GU_BLEND);
			applyDepthConfig(false);
			sceGuClearColor(0xFF080808);
			sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
			sceGumMatrixMode(GU_PROJECTION);
			sceGumLoadMatrix(&ident);
			sceGumMatrixMode(GU_VIEW);
			sceGumLoadIdentity();
			sceGumMatrixMode(GU_MODEL);
			sceGumLoadIdentity();
			drawNdcTri(0xFF0000FFu, 0.5f, 0.8f);
			drawNdcTri(0xFFFFFF00u, -0.5f, 0.8f); // Cyan near
			drawNdcTri(0xFFFF0000u, 0.8f, 0.8f);
			if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
				const uint32 fwdPx = readbackBuf[120 * readbackStride + 160];
				fwdDepthPass = (fwdPx & 0x00FFFFFFu) == 0x00FFFF00u;
			}
		}

		// Four corners & homogeneous clip outside [-1,1]
		if (_session->beginFrame()) {
			applySceneViewport();
			sceGuDisable(GU_TEXTURE_2D);
			sceGuDisable(GU_LIGHTING);
			sceGuDisable(GU_CULL_FACE);
			sceGuDisable(GU_DEPTH_TEST);
			sceGuDepthMask(GU_TRUE);
			sceGuClearColor(0xFF101010);
			sceGuClear(GU_COLOR_BUFFER_BIT);
			sceGumMatrixMode(GU_PROJECTION);
			sceGumLoadMatrix(&ident);
			sceGumMatrixMode(GU_VIEW);
			sceGumLoadIdentity();
			sceGumMatrixMode(GU_MODEL);
			sceGumLoadIdentity();
			// Oversized clipped triangle covering center via homogeneous clip [-3..+3]
			drawNdcTri(0xFF33AA55u, 0.0f, 3.0f);
			ColorVertex2D *c = static_cast<ColorVertex2D *>(_session->allocVertices(8 * sizeof(ColorVertex2D), 16));
			auto putCorner = [&](int idx, float cx0, float cy0, float cx1, float cy1, uint32 col) {
				c[idx].color = col; c[idx].x = cx0; c[idx].y = cy0; c[idx].z = 0.0f;
				c[idx + 1].color = col; c[idx + 1].x = cx1; c[idx + 1].y = cy1; c[idx + 1].z = 0.0f;
			};
			putCorner(0, 0.0f, 0.0f, 4.0f, 4.0f, 0xFF0000FFu);
			putCorner(2, 316.0f, 0.0f, 320.0f, 4.0f, 0xFF00FF00u);
			putCorner(4, 0.0f, 236.0f, 4.0f, 240.0f, 0xFFFF0000u);
			putCorner(6, 316.0f, 236.0f, 320.0f, 240.0f, 0xFFFFFFFFu);
			sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 8, nullptr, c);
			if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
				const uint32 tl = readbackBuf[0 * readbackStride + 0] & 0x00FFFFFFu;
				const uint32 tr = readbackBuf[0 * readbackStride + 319] & 0x00FFFFFFu;
				const uint32 bl = readbackBuf[239 * readbackStride + 0] & 0x00FFFFFFu;
				const uint32 br = readbackBuf[239 * readbackStride + 319] & 0x00FFFFFFu;
				const uint32 clipCenter = readbackBuf[120 * readbackStride + 160] & 0x00FFFFFFu;
				volatile uint32 *fb = static_cast<volatile uint32 *>(_session->getSceneColorUncached());
				const uint32 padRight = fb ? fb[100 * PspGuSession::kSceneColorStride + 320] : kPaddingSentinelAbgr;
				const uint32 padBot = fb ? fb[240 * PspGuSession::kSceneColorStride + 100] : kPaddingSentinelAbgr;
				cornersAndClipPass = (tl == 0x000000FFu && tr == 0x0000FF00u &&
				                      bl == 0x00FF0000u && br == 0x00FFFFFFu &&
				                      clipCenter == 0x0033AA55u &&
				                      padRight == kPaddingSentinelAbgr && padBot == kPaddingSentinelAbgr);
			}
		}

		// Mid-frame getViewportScreenshot test (partially built frame -> readback -> resume without swap)
		if (_session->beginFrame()) {
			const uint32 swapsBefore = _session->getStats().swaps;
			setViewport(Common::Rect(0, 36, 640, 401));
			sceGuDisable(GU_TEXTURE_2D);
			sceGuDisable(GU_DEPTH_TEST);
			sceGuDepthMask(GU_TRUE);
			sceGuClearColor(0xFF101820);
			sceGuClear(GU_COLOR_BUFFER_BIT);

			ColorVertex2D *preQuad = static_cast<ColorVertex2D *>(_session->allocVertices(2 * sizeof(ColorVertex2D), 16));
			mapLogical2DToRt(0.0f, 0.0f, preQuad[0].x, preQuad[0].y);
			mapLogical2DToRt(200.0f, 200.0f, preQuad[1].x, preQuad[1].y);
			preQuad[0].color = 0xFF22CC44u; preQuad[0].z = 0.0f;
			preQuad[1].color = 0xFF22CC44u; preQuad[1].z = 0.0f;
			sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, preQuad);

			Graphics::Surface *shot = getViewportScreenshot(160, 91);
			const uint32 swapsAfterShot = _session->getStats().swaps;

			ColorVertex2D *postQuad = static_cast<ColorVertex2D *>(_session->allocVertices(2 * sizeof(ColorVertex2D), 16));
			mapLogical2DToRt(400.0f, 0.0f, postQuad[0].x, postQuad[0].y);
			mapLogical2DToRt(600.0f, 200.0f, postQuad[1].x, postQuad[1].y);
			postQuad[0].color = 0xFFDD4422u; postQuad[0].z = 0.0f;
			postQuad[1].color = 0xFFDD4422u; postQuad[1].z = 0.0f;
			sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, postQuad);

			bool shotValid = false;
			if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride) && shot && shot->getPixels()) {
				const uint32 shotLeft = *(const uint32 *)shot->getBasePtr(20, 20);
				const uint32 postRt = readbackBuf[40 * readbackStride + 250];
				shotValid = (shot->w == 160 && shot->h == 91 &&
				             shot->format.bytesPerPixel == 4 &&
				             (shotLeft & 0x00FFFFFFu) == 0x0022CC44u &&
				             (postRt & 0x00FFFFFFu) == 0x00DD4422u &&
				             swapsBefore == swapsAfterShot);
				shot->free();
			} else if (shot) {
				shot->free();
			}
			delete shot;
			midScreenshotPass = shotValid;
		}

		// Present once & wait to retire the contract-test semaphore cleanly before live loop
		_session->submitFrame(false);
		_session->waitFrame();
	}
	free(readbackBuf);

	// 5. Coordinate & ray picking parity check using Stark::Scene vs upstream 640x480 reference math
	bool rayParityPass = false;
	{
		setScreenViewport(false);
		Scene testScene(this);
		testScene.initCamera(Math::Vector3d(0.0f, -500.0f, 150.0f),
		                     Math::Vector3d(0.0f, 1.0f, 0.0f),
		                     45.0f, Common::Rect(640, 365), 100.0f, 64000.0f);
		testScene.scrollCamera(Common::Rect(0, 0, 640, 365));
		const Common::Rect gv = gameViewport();
		const Math::Ray centerRay = testScene.makeRayFromMouse(Common::Point(320, 218));
		const Math::Ray topRay = testScene.makeRayFromMouse(Common::Point(320, 36));
		const Math::Ray botRay = testScene.makeRayFromMouse(Common::Point(320, 401));
		// Convert 3D center point via Scene and also check makeRayFromMouse consistency
		const Common::Point projCenter = testScene.convertPosition3DToGameScreenOriginal(Math::Vector3d(0.0f, 0.0f, 150.0f));
		// Notice in Stark::Scene, convertPosition3DToGameScreenOriginal computes y = -36 + 480 - (1 + 0) * 365 / 2 = 261
		// and makeRayFromMouse(320, 218) maps center of gameViewport [36, 401] -> y_ndc ~ 0 -> direction.y() > 0.99
		rayParityPass = (gv == Common::Rect(0, 36, 640, 401)) &&
		                (fabsf(centerRay.getDirection().y() - 1.0f) < 0.02f) &&
		                (topRay.getDirection().z() > botRay.getDirection().z()) &&
		                (abs(projCenter.x - 320) <= 1 && abs(projCenter.y - 261) <= 1);
	}

	const PspGuSessionStats &st = _session->getStats();
	debug("[STARK-PSP-STEP02] contracts: dupReject=%s vramFail=%s(deltaB=%d) ramFail=%s(deltaB=%d) activeAfterFail=%s "
	      "revDepth=%s fwdDepth=%s cornersClip=%s midShot=%s rayParity=%s ov=[%u,%u) sc=[%u,%u) sd=[%u,%u) freeTail=%u susp=%u/%u acq=%u rel=%u",
	      (!dupOk && activeAfterDup) ? "OK" : "FAIL",
	      !vramFailOk ? "OK" : "FAIL", usedAfterVramFail - usedBeforeVramFail,
	      !ramFailOk ? "OK" : "FAIL", usedAfterRamFail - usedAfterVramFail,
	      activeAfterFail ? "true" : "false",
	      revDepthPass ? "OK" : "FAIL",
	      fwdDepthPass ? "OK" : "FAIL",
	      cornersAndClipPass ? "OK" : "FAIL",
	      midScreenshotPass ? "OK" : "FAIL",
	      rayParityPass ? "OK" : "FAIL",
	      st.overlayOffset, st.overlayEnd,
	      st.sceneColorOffset, st.sceneColorEnd,
	      st.sceneDepthOffset, st.sceneDepthEnd,
	      st.freeEdramAfterDepth,
	      st.suspendCount, st.resumeCount, st.acquires, st.releases);
}

void PspGuDriver::init() {
	_session = PspGuSession::getBackendSession();
	if (!_session) {
		warning("PspGuDriver: PSP backend native GU session unavailable");
		_sessionReady = false;
		return;
	}

	if (!_session->acquire(kNativeOwner)) {
		warning("PspGuDriver: failed transactional acquire of native GU session");
		_sessionReady = false;
		return;
	}

	_sessionReady = true;
	_computeLights = false;
	computeScreenViewport();
	setScreenViewport(false);

	verifyStep02Contracts();

	debug("[STARK-PSP-STEP02] PspGuDriver ready (session=%s, listCap=%u, reserve=%u, vertexArena=%u, logical=%dx%d, rt=%ux%u)",
	      _sessionReady ? "ready" : "unavailable",
	      PspGuSession::kDisplayListSize,
	      PspGuSession::kDisplayListReserve,
	      PspGuSession::kVertexArenaSize,
	      kOriginalWidth, kOriginalHeight,
	      PspGuSession::kSceneWidth, PspGuSession::kSceneHeight);
}

void PspGuDriver::setScreenViewport(bool noScaling) {
	if (noScaling) {
		_viewport = Common::Rect(g_system->getWidth(), g_system->getHeight());
		_unscaledViewport = _viewport;
	} else {
		_viewport = _screenViewport;
		_unscaledViewport = Common::Rect(kOriginalWidth, kOriginalHeight);
	}
	updateRtViewport();
	if (_session && _session->isFrameOpen()) {
		applySceneViewport();
	}
}

void PspGuDriver::setViewport(const Common::Rect &rect) {
	_viewport = Common::Rect(
			_screenViewport.width() * rect.width() / kOriginalWidth,
			_screenViewport.height() * rect.height() / kOriginalHeight);
	_viewport.translate(
			_screenViewport.left + _screenViewport.width() * rect.left / kOriginalWidth,
			_screenViewport.top + _screenViewport.height() * rect.top / kOriginalHeight);
	_unscaledViewport = rect;
	updateRtViewport();
	if (_session && _session->isFrameOpen()) {
		applySceneViewport();
	}
}

bool PspGuDriver::ensureFrameBegun() {
	if (!_session || !_sessionReady || !_session->isActive()) {
		return false;
	}
	if (_session->isFrameOpen()) {
		return true;
	}
	const bool ok = _session->beginFrame();
	if (ok) {
		applySceneViewport();
	}
	return ok;
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
	applySceneViewport();
	applyDepthConfig(_reverseDepth);
	sceGuClearColor(0xFF000000);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void PspGuDriver::drawStep02ValidationScene() {
	if (!ensureFrameBegun()) {
		return;
	}
	if (!_session->hasDisplayListHeadroom(4096)) {
		warning("PspGuDriver: insufficient display list headroom for Step 02 validation scene");
		return;
	}

	// Pass A: Full 640x480 logical screen -> 320x240 RT
	setScreenViewport(false);
	applyDepthConfig(_reverseDepth);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_BLEND);
	sceGuClearColor(0xFF201410);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

	// 1-pixel outer border around [0, 320) x [0, 240) + lines at logical y=36 (rt y=18) & y=401 (rt y=200)
	// plus 4 corner squares + numbered calibration grid ticks
	ColorVertex2D *v2d = static_cast<ColorVertex2D *>(_session->allocVertices(40 * sizeof(ColorVertex2D), 16));
	if (!v2d) {
		return;
	}
	int vIdx = 0;
	auto emitRect2D = [&](float x0, float y0, float x1, float y1, uint32 col) {
		v2d[vIdx].color = col; v2d[vIdx].x = x0; v2d[vIdx].y = y0; v2d[vIdx].z = 0.0f;
		v2d[vIdx + 1].color = col; v2d[vIdx + 1].x = x1; v2d[vIdx + 1].y = y1; v2d[vIdx + 1].z = 0.0f;
		vIdx += 2;
	};
	// Bright yellow 1-px outer border (for edge/bleed inspection at LCD [59,421) x [0,272))
	const uint32 borderCol = 0xFF00FFFFu;
	emitRect2D(0.0f, 0.0f, 320.0f, 1.0f, borderCol);     // top y=0
	emitRect2D(0.0f, 239.0f, 320.0f, 240.0f, borderCol); // bottom y=239
	emitRect2D(0.0f, 0.0f, 1.0f, 240.0f, borderCol);     // left x=0
	emitRect2D(319.0f, 0.0f, 320.0f, 240.0f, borderCol); // right x=319
	// Lines at logical y=36 (rt y=18) and y=401 (rt y=200)
	emitRect2D(1.0f, 17.0f, 319.0f, 18.0f, 0xFFFF9020u);
	emitRect2D(1.0f, 200.0f, 319.0f, 201.0f, 0xFFFF9020u);
	// Adjacent odd-edged 2D rectangles (no gap, no overlap) in top bar
	emitRect2D(19.0f, 4.0f, 56.0f, 14.0f, 0xFF3366FFu);  // width 37
	emitRect2D(56.0f, 4.0f, 97.0f, 14.0f, 0xFF33FF66u);  // width 41
	// Four corner squares (red, green, blue, white)
	emitRect2D(2.0f, 2.0f, 10.0f, 10.0f, 0xFF0000FFu);
	emitRect2D(310.0f, 2.0f, 318.0f, 10.0f, 0xFF00FF00u);
	emitRect2D(2.0f, 230.0f, 10.0f, 238.0f, 0xFFFF0000u);
	emitRect2D(310.0f, 230.0f, 318.0f, 238.0f, 0xFFFFFFFFu);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, vIdx, nullptr, v2d);

	// Pass B: Same-frame viewport switch to logical world subwindow [0, 640) x [36, 401) -> [0, 320) x [18, 200)
	setViewport(Common::Rect(0, kTopBorderHeight, kOriginalWidth, kTopBorderHeight + kGameViewportHeight));

	// 4x3 world grid lines inside [0, 320) x [18, 200)
	ColorVertex2D *grid = static_cast<ColorVertex2D *>(_session->allocVertices(12 * sizeof(ColorVertex2D), 16));
	if (grid) {
		for (int gx = 0; gx < 3; ++gx) {
			const float rx = (float)((gx + 1) * 80);
			grid[gx * 2].color = 0xFF403028u; grid[gx * 2].x = rx; grid[gx * 2].y = 18.0f; grid[gx * 2].z = 0.0f;
			grid[gx * 2 + 1].color = 0xFF403028u; grid[gx * 2 + 1].x = rx + 1.0f; grid[gx * 2 + 1].y = 200.0f; grid[gx * 2 + 1].z = 0.0f;
		}
		for (int gy = 0; gy < 2; ++gy) {
			const float ry = 18.0f + (float)((gy + 1) * 60);
			const int base = 6 + gy * 2;
			grid[base].color = 0xFF403028u; grid[base].x = 0.0f; grid[base].y = ry; grid[base].z = 0.0f;
			grid[base + 1].color = 0xFF403028u; grid[base + 1].x = 320.0f; grid[base + 1].y = ry + 1.0f; grid[base + 1].z = 0.0f;
		}
		sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 10, nullptr, grid);
	}

	// Pass C: Depth-tested overlapping 3D triangles inside [0, 320) x [18, 200)
	applyDepthConfig(_reverseDepth);
	sceGuShadeModel(GU_SMOOTH);

	alignas(16) ScePspFMatrix4 proj;
	memset(&proj, 0, sizeof(proj));
	proj.x.x = 182.0f / 320.0f;
	proj.y.y = 1.0f;
	proj.z.z = 1.0f;
	proj.w.w = 1.0f;
	sceGumMatrixMode(GU_PROJECTION);
	sceGumLoadMatrix(&proj);
	sceGumMatrixMode(GU_VIEW);
	sceGumLoadIdentity();

	alignas(16) ScePspFMatrix4 farModel;
	memset(&farModel, 0, sizeof(farModel));
	farModel.x.x = 1.0f; farModel.y.y = 1.0f; farModel.z.z = 1.0f; farModel.w.w = 1.0f;
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&farModel);

	SmokeVertex3D *farTri = static_cast<SmokeVertex3D *>(_session->allocVertices(3 * sizeof(SmokeVertex3D), 16));
	if (farTri) {
		farTri[0].color = 0xFF2020E0u; farTri[0].x = -0.65f; farTri[0].y = -0.55f; farTri[0].z = 0.40f;
		farTri[1].color = 0xFF2020E0u; farTri[1].x = 0.65f; farTri[1].y = -0.55f; farTri[1].z = 0.40f;
		farTri[2].color = 0xFF2060FFu; farTri[2].x = 0.0f; farTri[2].y = 0.70f; farTri[2].z = 0.40f;
		sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, farTri);
	}

	const float angle = (float)_smokeFrameIndex * 0.25f;
	const float ca = cosf(angle);
	const float sa = sinf(angle);
	alignas(16) ScePspFMatrix4 nearModel;
	memset(&nearModel, 0, sizeof(nearModel));
	nearModel.x.x = ca; nearModel.x.y = sa;
	nearModel.y.x = -sa; nearModel.y.y = ca;
	nearModel.z.z = 1.0f; nearModel.w.w = 1.0f;
	sceGumLoadMatrix(&nearModel);

	SmokeVertex3D *nearTri = static_cast<SmokeVertex3D *>(_session->allocVertices(3 * sizeof(SmokeVertex3D), 16));
	if (nearTri) {
		const bool oddFrame = (_smokeFrameIndex & 1) != 0;
		nearTri[0].color = oddFrame ? 0xFFFF50FFu : 0xFFFFFF40u; nearTri[0].x = 0.0f; nearTri[0].y = 0.60f; nearTri[0].z = -0.40f;
		nearTri[1].color = oddFrame ? 0xFF50E0FFu : 0xFF40FF80u; nearTri[1].x = -0.55f; nearTri[1].y = -0.45f; nearTri[1].z = -0.40f;
		nearTri[2].color = oddFrame ? 0xFFFFFFFFu : 0xFF40D0FFu; nearTri[2].x = 0.55f; nearTri[2].y = -0.45f; nearTri[2].z = -0.40f;
		sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, nearTri);
	}

	// Pass D: Same-frame unscaled bottom-bar verification tag via setScreenViewport(true)
	setScreenViewport(true);
	ColorVertex2D *tag = static_cast<ColorVertex2D *>(_session->allocVertices(2 * sizeof(ColorVertex2D), 16));
	if (tag) {
		const bool oddFrame = (_smokeFrameIndex & 1) != 0;
		mapLogical2DToRt(240.0f, 412.0f, tag[0].x, tag[0].y);
		mapLogical2DToRt(400.0f, 460.0f, tag[1].x, tag[1].y);
		tag[0].color = oddFrame ? 0xFF40FFE0u : 0xFFFF8040u; tag[0].z = 0.0f;
		tag[1].color = tag[0].color; tag[1].z = 0.0f;
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, tag);
	}
}

void PspGuDriver::flipBuffer() {
	_clearRequested = false;
	if (!_session || !_sessionReady || !_session->isActive()) {
		return;
	}

	drawStep02ValidationScene();

	if (!_session->submitFrame(true)) {
		warning("PspGuDriver: submitFrame failed, unwinding cleanly");
		return;
	}
	_session->waitFrame();

	if (_smokeFrameIndex < 3 || (_smokeFrameIndex % 120) == 0) {
		const PspGuSessionStats &st = _session->getStats();
		debug("[STARK-PSP-STEP02] frame=%u sceneSegs=%u lcdPresents=%u starts=%u finishes=%u swaps=%u listB=%u/%u vertsB=%u/%u",
		      _smokeFrameIndex, st.sceneSegments, st.lcdPresents,
		      st.starts, st.finishes, st.swaps,
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
	Graphics::Surface *result = nullptr;
	if (!_session || !_sessionReady || !_session->isActive() || !_session->hasSceneTarget()) {
		result = new Graphics::Surface();
		result->create(w, h, getRGBAPixelFormat());
		if (result->getPixels()) {
			memset(result->getPixels(), 0, (size_t)result->pitch * result->h);
		}
		return result;
	}

	const bool wasOpen = _session->isFrameOpen();
	if (wasOpen) {
		_session->flushSceneSegment();
	} else {
		sceGuSync(0, 0);
	}

	const int32 rx = _rtViewport.left;
	const int32 ry = _rtViewport.top;
	const int32 rw = _rtViewport.width();
	const int32 rh = _rtViewport.height();

	if (rw > 0 && rh > 0) {
		const int32 copyX = rx & ~3;
		const int32 copyW = ((rx + rw + 3) & ~3) - copyX;
		const int32 dstStride = (copyW + 15) & ~15;
		uint32 *stage = static_cast<uint32 *>(memalign(64, (size_t)dstStride * (size_t)rh * sizeof(uint32)));
		if (stage && _session->readSceneRectToRam(copyX, ry, copyW, rh, stage, dstStride)) {
			Graphics::Surface tmp;
			tmp.create((uint)rw, (uint)rh, getRGBAPixelFormat());
			if (tmp.getPixels()) {
				const int32 xSkip = rx - copyX;
				for (int32 y = 0; y < rh; ++y) {
					const uint32 *srcRow = stage + y * dstStride + xSkip;
					uint32 *dstRow = static_cast<uint32 *>(tmp.getBasePtr(0, y));
					memcpy(dstRow, srcRow, (size_t)rw * sizeof(uint32));
				}
				result = tmp.scale(w, h, false);
			}
			tmp.free();
		}
		free(stage);
	}

	if (!result) {
		result = new Graphics::Surface();
		result->create(w, h, getRGBAPixelFormat());
		if (result->getPixels()) {
			memset(result->getPixels(), 0, (size_t)result->pitch * result->h);
		}
	}

	if (wasOpen) {
		_session->resumeSceneSegment();
		applySceneViewport();
	}
	return result;
}

void PspGuDriver::set3DMode() {
	if (_session && _session->isFrameOpen()) {
		applySceneViewport();
		applyDepthConfig(_reverseDepth);
	}
}

bool PspGuDriver::computeLightsEnabled() {
	return _computeLights;
}

void PspGuDriver::reportUnimplemented(const char *methodName) {
	uint32 hash = 2166136261u;
	for (const char *p = methodName; p && *p; ++p) {
		hash ^= (uint8)*p;
		hash *= 16777619u;
	}
	const uint32 bit = 1u << (hash & 31u);
	if ((_unimplementedMask & bit) == 0) {
		_unimplementedMask |= bit;
		warning("STARK PSP Step02: %s is developmentally unavailable (unimplemented until Step 03+)",
		        methodName ? methodName : "unknown");
	}
}

} // End of namespace Gfx
} // End of namespace Stark
