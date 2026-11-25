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

#ifndef STARK_GFX_PSPGU_H
#define STARK_GFX_PSPGU_H

#include "engines/stark/gfx/driver.h"
#include "common/rect.h"

class PspGuSession;

namespace Stark {
namespace Gfx {

/**
 * Shared PSP GE conversion and memory-budget utilities for Step 03+ resources.
 */
uint32 pspGuNextPow2Clamped(uint32 value, uint32 minPow2 = 8, uint32 maxPow2 = 512);
int pspGuClassifySurfacePsm(const Graphics::Surface *src, const byte *palette);
void pspGuCopySurfaceRectToTile(const Graphics::Surface *src, const byte *palette,
                                int32 srcX, int32 srcY, int32 copyW, int32 copyH,
                                void *dst, uint32 dstStride, int psm);
void pspGuCopySurfaceRectToRgba8888(const Graphics::Surface *src, const byte *palette,
                                    int32 srcX, int32 srcY, int32 copyW, int32 copyH,
                                    uint32 *dst, uint32 dstStride);
void pspGuResampleSurfaceToRgba8888(const Graphics::Surface *src, const byte *palette,
                                    uint32 dstW, uint32 dstH, uint32 *dst, uint32 dstStride);
void pspGuTrackBitmapBytes(int32 deltaBytes);
void pspGuTrackTextureBytes(int32 deltaBytes);
void pspGuTrackAuxBytes(int32 deltaBytes);
uint32 pspGuGetLiveBitmapBytes();
uint32 pspGuGetPeakBitmapBytes();
uint32 pspGuGetLiveTextureBytes();
uint32 pspGuGetPeakTextureBytes();
uint32 pspGuGetLiveAuxBytes();

/**
 * Native PSP sceGu / sceGum graphics driver for STARK.
 */
class PspGuDriver : public Driver {
public:
	PspGuDriver();
	~PspGuDriver() override;

	void init() override;

	void setScreenViewport(bool noScaling) override;
	void setViewport(const Common::Rect &rect) override;

	void clearScreen() override;
	void flipBuffer() override;

	Texture *createTexture() override;
	Bitmap *createBitmap(const Graphics::Surface *surface = nullptr, const byte *palette = nullptr) override;
	VisualActor *createActorRenderer() override;
	VisualProp *createPropRenderer() override;
	SurfaceRenderer *createSurfaceRenderer() override;
	FadeRenderer *createFadeRenderer() override;

	Graphics::Surface *getViewportScreenshot(uint w, uint h) const override;

	void set3DMode() override;
	bool computeLightsEnabled() override;

	PspGuSession *getSession() const { return _session; }
	bool isSessionReady() const { return _sessionReady; }
	const Common::Rect &getViewport() const { return _viewport; }
	const Common::Rect &getUnscaledViewport() const { return _unscaledViewport; }
	const Common::Rect &getRtViewport() const { return _rtViewport; }
	uint32 getFrameSeq() const { return _frameSeq; }

	bool ensureFrameBegun();
	void applySceneViewport() const;
	void applyDepthConfig(bool reverseDepth) const;
	void mapLogical2DToRt(float lx, float ly, float &outX, float &outY) const;
	void mapLogical2DRectToRt(int32 destX, int32 destY, uint32 rectW, uint32 rectH,
	                          bool noScalingOverride, bool snapToGrid,
	                          float &outX0, float &outY0, float &outX1, float &outY1) const;
	void reportUnimplemented(const char *methodName);

	static void ensureResourceSafeToModify(uint32 drawnFrameSeq);

private:
	void updateRtViewport();
	void verifyStartupContracts();
	bool verifyStep03SurfaceAndTextureContracts(uint32 *readbackBuf, uint32 readbackStride);

	PspGuSession *_session;
	Common::Rect _viewport;
	Common::Rect _unscaledViewport;
	Common::Rect _rtViewport;
	bool _sessionReady;
	bool _clearRequested;
	bool _reverseDepth;
	uint32 _frameSeq;
	uint32 _unimplementedMask;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_H
