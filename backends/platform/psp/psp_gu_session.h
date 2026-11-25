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
	uint32 starts;
	uint32 finishes;
	uint32 swaps;
	uint32 overlayRePresents;
	uint32 suspendCount;
	uint32 resumeCount;
	uint32 lastListBytes;
	uint32 peakListBytes;
	uint32 lastVertexBytes;
	uint32 peakVertexBytes;
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

	PspGuSession();
	~PspGuSession();

	static PspGuSession *getBackendSession();
	void bindDisplayManager(DisplayManager *displayManager);

	/** Acquire exclusive native scene ownership and allocate bounded GE command/vertex buffers. */
	bool acquire(const char *ownerName);

	/** Wait for GE/callback idle, cleanly close any open frame, and free native buffers. */
	void release();

	bool isActive() const { return _active; }
	bool isFrameOpen() const { return _frameOpen; }
	bool isSuspended() const { return _suspended; }
	const char *getOwnerName() const { return _ownerName; }

	/** Begin recording a native GU_DIRECT frame after prior callback/GE completion. */
	bool beginFrame();

	/** Bump-allocate aligned vertices inside the bounded per-frame vertex arena. */
	void *allocVertices(uint32 bytes, uint32 alignment = 16);

	/** Write back CPU data cache for GE-visible buffers (vertices/textures). */
	void flushDataRange(const void *ptr, uint32 bytes) const;

	uint32 getDisplayListBytesUsed() const;
	bool hasDisplayListHeadroom(uint32 neededBytes = 0) const;

	/**
	 * Composite visible backend overlays (dialog overlay, cursor, vkbd, viewer).
	 * Draws inline when a native frame is open, or runs a standalone overlay pass if idle.
	 */
	bool overlayRePresent();

	/** Finish the open frame, optionally compositing overlays, and schedule buffer swap. */
	bool submitFrame(bool compositeOverlay = true);

	/** Block until the scheduled frame callback finishes and swaps buffers. */
	void waitFrame();

	/** Test hook to deterministically trigger an allocation failure on the next acquire(). */
	void setInjectAllocFail(bool enable) { _injectAllocFail = enable; }

	const PspGuSessionStats &getStats() const { return _stats; }

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

	DisplayManager *_displayManager;
	uint8 *_displayList;
	uint8 *_vertexArena;
	uint32 _vertexOffset;
	const char *_ownerName;
	bool _active;
	bool _frameOpen;
	bool _suspended;
	bool _suspendRegistered;
	bool _injectAllocFail;
	PspGuSessionStats _stats;
	SessionSuspendable _suspendable;
};

#endif /* PSP_GU_SESSION_H */
