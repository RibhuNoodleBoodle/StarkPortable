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

#ifndef PSP_GU_SESSION_H
#define PSP_GU_SESSION_H

#include "common/scummsys.h"
#include "backends/platform/psp/powerman.h"

class DisplayManager;

/**
 * Telemetry and verification counters for the backend-owned native PSP GU session.
 */
struct PspGuSessionStats {
	uint32 acquires;
	uint32 releases;
	uint32 duplicateAcquireRejects;
	uint32 allocFailures;
	uint32 vramFailures;
	uint32 starts;
	uint32 finishes;
	uint32 swaps;
	uint32 sceneSegments;
	uint32 lcdPresents;
	uint32 overlayRePresents;
	uint32 suspendCount;
	uint32 resumeCount;
	uint32 lastListBytes;
	uint32 peakListBytes;
	uint32 lastVertexBytes;
	uint32 peakVertexBytes;
	uint32 sceneColorOffset;
	uint32 sceneColorEnd;
	uint32 sceneDepthOffset;
	uint32 sceneDepthEnd;
	uint32 overlayOffset;
	uint32 overlayEnd;
	uint32 freeEdramAfterDepth;
};

/**
 * Narrow backend-owned native GE/GU scene session managed through DisplayManager
 * and MasterGuRenderer, keeping sceGuInit/sceGuTerm/guCallback singular in the backend.
 */
class PspGuSession {
public:
	static const uint32 kDisplayListSize = 131072;   // 128 KiB direct display list
	static const uint32 kDisplayListReserve = 4096;  // 4 KiB safety reserve before finish
	static const uint32 kVertexArenaSize = 262144;   // 256 KiB streamed vertex arena

	// Step 02 offscreen eDRAM scene target constants (Memory_Budget.txt)
	static const uint32 kEdramBase = 0x04000000;
	static const uint32 kEdramSize = 2097152;
	static const uint32 kEdramTargetAlign = 8192;
	static const uint32 kSceneWidth = 320;
	static const uint32 kSceneHeight = 240;
	static const uint32 kSceneColorStride = 512;
	static const uint32 kSceneColorTexHeight = 256;
	static const uint32 kSceneColorBytes = kSceneColorStride * kSceneColorTexHeight * 4; // 524,288
	static const uint32 kSceneDepthStride = 320;
	static const uint32 kSceneDepthBytes = kSceneDepthStride * kSceneHeight * 2;        // 153,600

	static const uint32 kLcdWidth = 480;
	static const uint32 kLcdHeight = 272;
	static const uint32 kPresentLeft = 59;
	static const uint32 kPresentRight = 421; // x = 59..420 inclusive (width 362)

	PspGuSession();
	~PspGuSession();

	static PspGuSession *getBackendSession();
	void bindDisplayManager(DisplayManager *displayManager);

	/** Acquire exclusive native scene ownership and allocate bounded eDRAM & RAM buffers. */
	bool acquire(const char *ownerName);

	/** Wait for GE/callback idle, cleanly close any open frame, free buffers, restore launcher Screen. */
	void release();

	bool isActive() const { return _active; }
	bool isFrameOpen() const { return _frameOpen; }
	bool isSuspended() const { return _suspended; }
	bool hasSceneTarget() const { return _sceneColorPtr != 0 && _sceneDepthPtr != 0; }
	bool hasValidSceneImage() const { return _sceneRendered; }
	const char *getOwnerName() const { return _ownerName; }

	/** Begin recording a native GU_DIRECT scene segment into the 320x240 eDRAM target. */
	bool beginFrame();

	/** Bind the 320x240 RGBA8888 color + 320x240x2 depth eDRAM buffers on the open GU list. */
	void bindSceneRenderTarget();

	/** Bump-allocate aligned vertices inside the bounded per-frame vertex arena. */
	void *allocVertices(uint32 bytes, uint32 alignment = 16);

	/** Write back CPU data cache for GE-visible buffers (vertices/textures). */
	void flushDataRange(const void *ptr, uint32 bytes) const;

	uint32 getDisplayListBytesUsed() const;
	const uint8 *getDisplayListPtr() const { return _displayList; }
	bool hasDisplayListHeadroom(uint32 neededBytes = 0) const;
	uint32 getVertexOffset() const { return _vertexOffset; }
	bool hasVertexHeadroom(uint32 neededBytes, uint32 alignment = 16) const {
		if (!_frameOpen) {
			return false;
		}
		const uint32 aligned = (_vertexOffset + alignment - 1) & ~(alignment - 1);
		return aligned <= kVertexArenaSize && neededBytes <= kVertexArenaSize - aligned;
	}

	/** Prime unused 512x256 padding texels outside 320x240 with a debug sentinel color. */
	void primeScenePadding(uint32 sentinelAbgr);

	/** Close the open scene segment (if any) and sync once before sampling or CPU readback. */
	bool flushSceneSegment();

	/** Resume an interrupted scene segment (e.g. after mid-frame screenshot readback) without swapping. */
	bool resumeSceneSegment();

	/** Synchronized VRAM->RAM readback of a cropped rectangle from the 320x240 scene target. */
	bool readSceneRectToRam(int rx, int ry, int rw, int rh, uint32 *dstRam, int dstStride);

	/**
	 * Emit the LCD presentation sprite drawing visible 320x240 RT onto x=[59,421) y=[0,272)
	 * into the currently active GU display list (must already have current LCD backbuffer bound).
	 */
	bool emitLcdSceneBlitInActiveList();

	/**
	 * Composite visible backend overlays (dialog overlay, cursor, vkbd, viewer).
	 * Draws inline when a native list is open, or runs a standalone overlay pass if idle.
	 */
	bool overlayRePresent();

	/** Finish scene segment, sync once, present 320x240 -> LCD backbuffer, overlay, and swap. */
	bool submitFrame(bool compositeOverlay = true);

	/** Block until the scheduled frame callback finishes and swaps buffers. */
	void waitFrame();

	/** Test hooks to deterministically trigger RAM or VRAM allocation failure on next acquire(). */
	void setInjectAllocFail(bool enable) { _injectAllocFail = enable; }
	void setInjectVramFail(bool enable) { _injectVramFail = enable; }

	const PspGuSessionStats &getStats() const { return _stats; }

	void *getSceneColorCached() const { return _sceneColorPtr; }
	void *getSceneColorUncached() const;
	void *getSceneColorRel() const { return _sceneColorRel; }
	void *getSceneDepthRel() const { return _sceneDepthRel; }
	void *getCurrentLcdBackRel() const;

	/** Suspendable hooks invoked via PowerManager. */
	int onSuspend();
	int onResume();

private:
	class SessionSuspendable : public Suspendable {
	public:
		explicit SessionSuspendable(PspGuSession *owner) : _owner(owner) {}
		int suspend() override { return _owner ? _owner->onSuspend() : 0; }
		int resume() override { return _owner ? _owner->onResume() : 0; }
	private:
		PspGuSession *_owner;
	};

	void freeBuffers();
	void syncGlobalCounters();
	void logEdramMap() const;

	DisplayManager *_displayManager;
	uint8 *_displayList;
	uint8 *_vertexArena;
	void *_sceneColorPtr;
	void *_sceneDepthPtr;
	void *_sceneColorRel;
	void *_sceneDepthRel;
	uint32 _vertexOffset;
	const char *_ownerName;
	bool _active;
	bool _frameOpen;
	bool _sceneRendered;
	bool _suspended;
	bool _suspendRegistered;
	bool _injectAllocFail;
	bool _injectVramFail;
	PspGuSessionStats _stats;
	SessionSuspendable _suspendable;
};

#endif /* PSP_GU_SESSION_H */
