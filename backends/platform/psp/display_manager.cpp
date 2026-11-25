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

#include <malloc.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspthreadman.h>
#include <psputils.h>
#include <string.h>

#include "common/scummsys.h"
#include "backends/base-backend.h"
#include "backends/platform/psp/psppixelformat.h"
#include "backends/platform/psp/display_client.h"
#include "backends/platform/psp/default_display_client.h"
#include "backends/platform/psp/cursor.h"
#include "backends/platform/psp/pspkeyboard.h"
#include "backends/platform/psp/image_viewer.h"

#include "common/config-manager.h"

#define USE_DISPLAY_CALLBACK	// to use callback for finishing the render
#include "backends/platform/psp/display_manager.h"

#define PSP_BUFFER_WIDTH (512)
#define	PSP_SCREEN_WIDTH	480
#define	PSP_SCREEN_HEIGHT	272
#define PSP_FRAME_SIZE (PSP_BUFFER_WIDTH * PSP_SCREEN_HEIGHT)

//#define ENABLE_RENDER_MEASURE /* how long it takes to render a frame */

//#define __PSP_DEBUG_FUNCS__	/* For debugging function calls */
//#define __PSP_DEBUG_PRINT__	/* For debug printouts */

#include "backends/platform/psp/trace.h"

uint32 __attribute__((aligned(16))) MasterGuRenderer::_displayList[2048];

const OSystem::GraphicsMode DisplayManager::_supportedModes[] = {
	{ "Original Resolution", "Original Resolution", ORIGINAL_RESOLUTION },
	{ "Fit to Screen", "Fit to Screen", FIT_TO_SCREEN },
	{ "Stretch to Screen", "Stretch to Screen", STRETCH_TO_SCREEN },
	{0, 0, 0}
};


// Class VramAllocator -----------------------------------

namespace Common {
DECLARE_SINGLETON(VramAllocator);
}

//#define __PSP_DEBUG_FUNCS__	/* For debugging the stack */
//#define __PSP_DEBUG_PRINT__

#include "backends/platform/psp/trace.h"


void *VramAllocator::allocate(int32 size, bool smallAllocation /* = false */) {
	DEBUG_ENTER_FUNC();
	assert(size > 0);

	byte *lastAddress = smallAllocation ? (byte *)VRAM_SMALL_ADDRESS : (byte *)VRAM_START_ADDRESS;
	Common::List<Allocation>::iterator i;

	// Find a block that fits, starting from the beginning
	for (i = _allocList.begin(); i != _allocList.end(); ++i) {
		byte *currAddress = (*i).address;

		if (currAddress - lastAddress >= size) // We found a match
			break;

		if ((*i).getEnd() > lastAddress)
			lastAddress = (byte *)(*i).getEnd();
	}

	if (lastAddress + size > (byte *)VRAM_END_ADDRESS) {
		PSP_DEBUG_PRINT("No space for allocation of %d bytes. %d bytes already allocated.\n",
		                size, _bytesAllocated);
		return NULL;
	}

	_allocList.insert(i, Allocation(lastAddress, size));
	_bytesAllocated += size;

	PSP_DEBUG_PRINT("Allocated in VRAM, size %u at %p.\n", size, lastAddress);
	PSP_DEBUG_PRINT("Total allocated %u, remaining %u.\n", _bytesAllocated, (2 * 1024 * 1024) - _bytesAllocated);

	return lastAddress;
}

// Deallocate a block from VRAM
void VramAllocator::deallocate(void *address) {
	DEBUG_ENTER_FUNC();
	address = (byte *)CACHED(address);	// Make sure all addresses are the same

	Common::List<Allocation>::iterator i;

	// Find the Allocator to deallocate
	for (i = _allocList.begin(); i != _allocList.end(); ++i) {
		if ((*i).address == address) {
			PSP_DEBUG_PRINT("Deallocated address[%p], size[%u]\n", (*i).address, (*i).size);
			_bytesAllocated -= (*i).size;
			_allocList.erase(i);
			return;
		}
	}

	PSP_DEBUG_PRINT("Address[%p] not allocated.\n", address);
}

void *VramAllocator::allocateAligned(int32 size, uint32 alignment, bool avoidSmallPool, uint32 *outAvailContiguous) {
	DEBUG_ENTER_FUNC();
	if (outAvailContiguous) {
		*outAvailContiguous = 0;
	}
	if (size <= 0) {
		return NULL;
	}
	if (alignment < 16) {
		alignment = 16;
	}

	const uint32 limitAddr = avoidSmallPool ? (uint32)VRAM_SMALL_ADDRESS : (uint32)VRAM_END_ADDRESS;
	uint32 cursor = (uint32)VRAM_START_ADDRESS;
	uint32 maxAvail = 0;

	Common::List<Allocation>::iterator i = _allocList.begin();
	for (; i != _allocList.end(); ++i) {
		const uint32 nodeStart = (uint32)(*i).address;
		const uint32 nodeEnd = (uint32)(*i).getEnd();
		const uint32 gapEnd = nodeStart < limitAddr ? nodeStart : limitAddr;
		if (gapEnd > cursor) {
			const uint32 aligned = (cursor + alignment - 1) & ~(alignment - 1);
			if (gapEnd > aligned) {
				const uint32 avail = gapEnd - aligned;
				if (avail > maxAvail) {
					maxAvail = avail;
				}
				if (avail >= (uint32)size) {
					void *ptr = (void *)(uintptr)aligned;
					_allocList.insert(i, Allocation(ptr, (uint32)size));
					_bytesAllocated += (uint32)size;
					if (outAvailContiguous) {
						*outAvailContiguous = avail;
					}
					return ptr;
				}
			}
		}
		if (nodeEnd > cursor) {
			cursor = nodeEnd;
		}
		if (cursor >= limitAddr) {
			break;
		}
	}

	if (cursor < limitAddr) {
		const uint32 aligned = (cursor + alignment - 1) & ~(alignment - 1);
		if (limitAddr > aligned) {
			const uint32 avail = limitAddr - aligned;
			if (avail > maxAvail) {
				maxAvail = avail;
			}
			if (avail >= (uint32)size) {
				void *ptr = (void *)(uintptr)aligned;
				_allocList.insert(i, Allocation(ptr, (uint32)size));
				_bytesAllocated += (uint32)size;
				if (outAvailContiguous) {
					*outAvailContiguous = avail;
				}
				return ptr;
			}
		}
	}

	if (outAvailContiguous) {
		*outAvailContiguous = maxAvail;
	}
	return NULL;
}

bool VramAllocator::verifyNoOverlaps(uint32 *outAllocatedBytes, uint32 *outLargestGap) const {
	uint32 prevEnd = (uint32)VRAM_START_ADDRESS;
	uint32 total = 0;
	uint32 largestGap = 0;

	for (Common::List<Allocation>::const_iterator i = _allocList.begin(); i != _allocList.end(); ++i) {
		const uint32 start = (uint32)(*i).address;
		const uint32 end = (uint32)(*i).getEnd();
		if (start < (uint32)VRAM_START_ADDRESS || end > (uint32)VRAM_END_ADDRESS || end <= start) {
			return false;
		}
		if (start < prevEnd) {
			return false;
		}
		if (start - prevEnd > largestGap) {
			largestGap = start - prevEnd;
		}
		prevEnd = end;
		total += (*i).size;
	}
	if ((uint32)VRAM_END_ADDRESS - prevEnd > largestGap) {
		largestGap = (uint32)VRAM_END_ADDRESS - prevEnd;
	}
	if (outAllocatedBytes) {
		*outAllocatedBytes = total;
	}
	if (outLargestGap) {
		*outLargestGap = largestGap;
	}
	return total == _bytesAllocated;
}

bool VramAllocator::containsRange(const void *base, uint32 size) const {
	const uint32 qStart = (uint32)CACHED(base);
	const uint32 qEnd = qStart + size;
	for (Common::List<Allocation>::const_iterator i = _allocList.begin(); i != _allocList.end(); ++i) {
		const uint32 aStart = (uint32)(*i).address;
		const uint32 aEnd = (uint32)(*i).getEnd();
		if (qStart >= aStart && qEnd <= aEnd) {
			return true;
		}
	}
	return false;
}


// Class MasterGuRenderer ----------------------------------------------

void MasterGuRenderer::setupCallbackThread() {
	DEBUG_ENTER_FUNC();

	// start the thread that updates the display
	threadCreateAndStart("DisplayCbThread", PRIORITY_DISPLAY_THREAD, STACK_DISPLAY_THREAD);
}

// this function gets called by PspThread when starting the new thread
void MasterGuRenderer::threadFunction() {
	DEBUG_ENTER_FUNC();

	// Create the callback. It should always get the pointer to MasterGuRenderer
	_callbackId = sceKernelCreateCallback("Display Callback", guCallback, this);
	if (_callbackId < 0) {
		PSP_ERROR("failed to create display callback\n");
	}

	PSP_DEBUG_PRINT("created callback. Going to sleep\n");

	sceKernelSleepThreadCB();	// sleep until we get a callback
}

// Sleep on the render mutex if the rendering thread hasn't finished its work
//
void MasterGuRenderer::sleepUntilRenderFinished() {
	if (!isRenderFinished()) {
		_renderSema.take();   // sleep on the semaphore
		_renderSema.give();
		PSP_DEBUG_PRINT("slept on the rendering semaphore\n");
	}
}


// This callback is called when the render is finished. It swaps the buffers
int MasterGuRenderer::guCallback(int, int, void *__this) {

	MasterGuRenderer *_this = (MasterGuRenderer *)__this;

	sceGuSync(0, 0);				// make sure we wait for GU to finish
	sceDisplayWaitVblankStartCB();	// wait for v-blank without eating main thread cycles
	_this->_currentLcdDrawBuffer = sceGuSwapBuffers();	// swap the back and front buffers
	_this->_guSwaps++;

	_this->_renderFinished = true;	// Only this thread can set the variable to true

	_this->_renderSema.give(); 		// Release render semaphore
	return 0;
}

void MasterGuRenderer::guInit() {
	DEBUG_ENTER_FUNC();

	sceGuInit();
	sceGuStart(0, _displayList);

	guProgramDisplayBufferSizes();

	sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuDepthRange(0xC350, 0x2710);
	sceGuDisable(GU_DEPTH_TEST);	// We'll use depth buffer area
	sceGuDepthMask(GU_TRUE);		// Prevent writes to depth buffer
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuFrontFace(GU_CW);
	sceGuEnable(GU_TEXTURE_2D);

	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);

	sceDisplayWaitVblankStart();
	sceGuDisplay(1);
}

void MasterGuRenderer::guProgramDisplayBufferSizes() {
	DEBUG_ENTER_FUNC();

	PSP_DEBUG_PRINT("Outputbits[%u]\n", GuRenderer::_displayManager->getOutputBitsPerPixel());

	switch (GuRenderer::_displayManager->getOutputBitsPerPixel()) {
	case 16:
		_currentLcdDrawBuffer = (void *)0;
		sceGuDrawBuffer(GU_PSM_4444, (void *)0, PSP_BUFFER_WIDTH);
		sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, (void *)(PSP_FRAME_SIZE * sizeof(uint16)), PSP_BUFFER_WIDTH);
		sceGuDepthBuffer((void *)(PSP_FRAME_SIZE * sizeof(uint16) * 2), PSP_BUFFER_WIDTH);
		VramAllocator::instance().allocate(PSP_FRAME_SIZE * sizeof(uint16) * 2);
		break;
	case 32:
		_currentLcdDrawBuffer = (void *)0;
		sceGuDrawBuffer(GU_PSM_8888, (void *)0, PSP_BUFFER_WIDTH);
		sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, (void *)(PSP_FRAME_SIZE * sizeof(uint32)), PSP_BUFFER_WIDTH);
		sceGuDepthBuffer((void *)(PSP_FRAME_SIZE * sizeof(uint32) * 2), PSP_BUFFER_WIDTH);
		VramAllocator::instance().allocate(PSP_FRAME_SIZE * sizeof(uint32) * 2);
		break;
	}
}

void MasterGuRenderer::bindCurrentLcdBackBuffer() {
	const int psm = (GuRenderer::_displayManager->getOutputBitsPerPixel() == 16) ? GU_PSM_4444 : GU_PSM_8888;
	sceGuDrawBufferList(psm, _currentLcdDrawBuffer, PSP_BUFFER_WIDTH);
}

// These are GU commands that should always stay the same
inline void MasterGuRenderer::guPreRender() {
	DEBUG_ENTER_FUNC();

#ifdef USE_DISPLAY_CALLBACK
	_renderSema.take(); 		// Take the semaphore to prevent writes
								// to the palette/screen before we're done
	_renderFinished = false;	// set to synchronize with callback thread
#endif

#ifdef ENABLE_RENDER_MEASURE
	_lastRenderTime = g_system->getMillis();
#endif /* ENABLE_RENDER_MEASURE */

	_guStarts++;
	sceGuStart(0, _displayList);
	bindCurrentLcdBackBuffer();

	sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuClearColor(0xFF000000);
	sceGuClear(GU_COLOR_BUFFER_BIT);

	sceGuAmbientColor(0xFFFFFFFF);
	sceGuColor(0xFFFFFFFF);
	sceGuTexOffset(0, 0);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);

	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA); // Also good enough for all purposes
	sceGuAlphaFunc(GU_GREATER, 0, 0xFF);	   // Also good enough for all purposes
}

inline void MasterGuRenderer::guPostRender() {
	DEBUG_ENTER_FUNC();

	_guFinishes++;
	sceGuFinish();
#ifdef USE_DISPLAY_CALLBACK
	if (_callbackId < 0) {
		PSP_ERROR("bad callbackId[%d]\n", _callbackId);
		sceGuSync(0, 0);
		sceDisplayWaitVblankStart();
		sceGuSwapBuffers();
		_guSwaps++;
		_renderFinished = true;
		_renderSema.give();
	} else {
		sceKernelNotifyCallback(_callbackId, 0);	// notify the callback. Nothing extra to pass
	}
#else
	sceGuSync(0, 0);

#ifdef ENABLE_RENDER_MEASURE
	uint32 now = g_system->getMillis();
	PSP_INFO_PRINT("Render took %d milliseconds\n", now - _lastRenderTime);
#endif /* ENABLE_RENDER_MEASURE */

	sceDisplayWaitVblankStart();
	_currentLcdDrawBuffer = sceGuSwapBuffers();
	_guSwaps++;
	_renderFinished = true;
#endif /* !USE_DISPLAY_CALLBACK */
}

void MasterGuRenderer::beginSceneSegment(void *displayList) {
	DEBUG_ENTER_FUNC();
	if (!_nativeHoldsSema) {
#ifdef USE_DISPLAY_CALLBACK
		sleepUntilRenderFinished();
		_renderSema.take();
		_renderFinished = false;
#endif
		_nativeHoldsSema = true;
	}
	sceGuStart(GU_DIRECT, displayList);
}

void MasterGuRenderer::resumeSceneSegment(void *displayList) {
	DEBUG_ENTER_FUNC();
	sceGuStart(GU_DIRECT, displayList);
}

void MasterGuRenderer::finishSceneSegmentOnly() {
	DEBUG_ENTER_FUNC();
	sceGuFinish();
	sceGuSync(0, 0);
}

void MasterGuRenderer::beginLcdPresentList(void *displayList) {
	DEBUG_ENTER_FUNC();
	_guStarts++;
	sceGuStart(GU_DIRECT, displayList);
	bindCurrentLcdBackBuffer();
	sceGuTexFlush();
}

void MasterGuRenderer::finishLcdPresentList() {
	_nativeHoldsSema = false;
	guPostRender();
}

void MasterGuRenderer::abortSceneSegmentAndReleaseSema() {
	DEBUG_ENTER_FUNC();
	sceGuFinish();
	sceGuSync(0, 0);
	releaseNativeSemaIfIdle();
}

void MasterGuRenderer::releaseNativeSemaIfIdle() {
	if (_nativeHoldsSema) {
		_nativeHoldsSema = false;
#ifdef USE_DISPLAY_CALLBACK
		_renderFinished = true;
		_renderSema.give();
#else
		_renderFinished = true;
#endif
	}
}

void MasterGuRenderer::guShutDown() {
	sceGuTerm();
}

// Class PspGuSession -------------------------------------------------------

static PspGuSession *s_backendGuSession = 0;

PspGuSession *PspGuSession::getBackendSession() {
	return s_backendGuSession;
}

PspGuSession::PspGuSession() :
		_displayManager(0),
		_displayList(0),
		_vertexArena(0),
		_sceneColorPtr(0),
		_sceneDepthPtr(0),
		_sceneColorRel(0),
		_sceneDepthRel(0),
		_vertexOffset(0),
		_ownerName(0),
		_active(false),
		_frameOpen(false),
		_sceneRendered(false),
		_suspended(false),
		_suspendRegistered(false),
		_injectAllocFail(false),
		_injectVramFail(false),
		_suspendable(this) {
	memset(&_stats, 0, sizeof(_stats));
}

PspGuSession::~PspGuSession() {
	release();
}

void PspGuSession::bindDisplayManager(DisplayManager *displayManager) {
	_displayManager = displayManager;
	s_backendGuSession = displayManager ? this : 0;
}

void PspGuSession::freeBuffers() {
	if (_sceneDepthPtr) {
		VramAllocator::instance().deallocate(_sceneDepthPtr);
		_sceneDepthPtr = 0;
		_sceneDepthRel = 0;
	}
	if (_sceneColorPtr) {
		VramAllocator::instance().deallocate(_sceneColorPtr);
		_sceneColorPtr = 0;
		_sceneColorRel = 0;
	}
	if (_displayList) {
		free(_displayList);
		_displayList = 0;
	}
	if (_vertexArena) {
		free(_vertexArena);
		_vertexArena = 0;
	}
	_vertexOffset = 0;
	_sceneRendered = false;
}

void PspGuSession::syncGlobalCounters() {
	if (_displayManager) {
		_displayManager->getMasterGuRenderer().getGuCounters(_stats.starts, _stats.finishes, _stats.swaps);
	}
}

void PspGuSession::logEdramMap() const {
	PSP_INFO_PRINT("[STARK-PSP-STEP02] eDRAM base=0x%08x size=%u lcdDraw=[0,557056) lcdDisp=[557056,1114112) "
	               "overlay=[%u,%u) sceneColor=[%u,%u)(0x%08x,off=0x%06x,512x256x4,vis=320x240) "
	               "sceneDepth=[%u,%u)(0x%08x,off=0x%06x,320x240x2) freeAfter=%u\n",
	               kEdramBase, kEdramSize,
	               _stats.overlayOffset, _stats.overlayEnd,
	               _stats.sceneColorOffset, _stats.sceneColorEnd,
	               (uint32)(uintptr)_sceneColorPtr, (uint32)(uintptr)_sceneColorRel,
	               _stats.sceneDepthOffset, _stats.sceneDepthEnd,
	               (uint32)(uintptr)_sceneDepthPtr, (uint32)(uintptr)_sceneDepthRel,
	               _stats.freeEdramAfterDepth);
}

bool PspGuSession::acquire(const char *ownerName) {
	if (!_displayManager) {
		return false;
	}
	if (_active) {
		_stats.duplicateAcquireRejects++;
		PSP_ERROR("PspGuSession: duplicate acquire rejected for '%s' (owned by '%s')\n",
		          ownerName ? ownerName : "unknown", _ownerName ? _ownerName : "unknown");
		return false;
	}

	_displayManager->waitUntilRenderFinished();
	sceGuSync(0, 0);

	// Release legacy 640x480 Screen VRAM backing while keeping logical 640x480 cursor/screen size
	_displayManager->releaseScreenBackingForNative();

	if (_injectAllocFail) {
		_injectAllocFail = false;
		_stats.allocFailures++;
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	uint32 availVram = 0;
	if (_injectVramFail) {
		_injectVramFail = false;
		_stats.vramFailures++;
		VramAllocator::instance().verifyNoOverlaps(0, &availVram);
		PSP_ERROR("PspGuSession: eDRAM scene color alloc failed (injected): requested=%u available=%u\n",
		          kSceneColorBytes, availVram);
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	_sceneColorPtr = VramAllocator::instance().allocateAligned(
			(int32)kSceneColorBytes, kEdramTargetAlign, true, &availVram);
	if (!_sceneColorPtr) {
		_stats.vramFailures++;
		PSP_ERROR("PspGuSession: eDRAM scene color alloc failed: requested=%u available=%u\n",
		          kSceneColorBytes, availVram);
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	uint32 availDepth = 0;
	_sceneDepthPtr = VramAllocator::instance().allocateAligned(
			(int32)kSceneDepthBytes, kEdramTargetAlign, true, &availDepth);
	if (!_sceneDepthPtr) {
		_stats.vramFailures++;
		PSP_ERROR("PspGuSession: eDRAM scene depth alloc failed: requested=%u available=%u\n",
		          kSceneDepthBytes, availDepth);
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	const uint32 lcdEnd = PSP_FRAME_SIZE * sizeof(uint32) * 2;
	uint32 ovOff = 0, ovEnd = 0;
	_displayManager->getOverlayVramRange(ovOff, ovEnd);
	const uint32 scOff = (uint32)(uintptr)_sceneColorPtr - kEdramBase;
	const uint32 scEnd = scOff + kSceneColorBytes;
	const uint32 sdOff = (uint32)(uintptr)_sceneDepthPtr - kEdramBase;
	const uint32 sdEnd = sdOff + kSceneDepthBytes;
	const uint32 smallOff = kEdramSize - 4096;

	const bool overlapValid = VramAllocator::instance().verifyNoOverlaps() &&
	                          VramAllocator::instance().containsRange(_sceneColorPtr, kSceneColorBytes) &&
	                          VramAllocator::instance().containsRange(_sceneDepthPtr, kSceneDepthBytes) &&
	                          (scOff >= lcdEnd) &&
	                          (ovEnd == 0 || scOff >= ovEnd || scEnd <= ovOff) &&
	                          (ovEnd == 0 || sdOff >= ovEnd || sdEnd <= ovOff) &&
	                          (scEnd <= sdOff || sdEnd <= scOff) &&
	                          (scEnd <= smallOff) &&
	                          (sdEnd <= smallOff);
	if (!overlapValid) {
		_stats.vramFailures++;
		PSP_ERROR("PspGuSession: eDRAM interval overlap detected sc=[%u,%u) sd=[%u,%u) ov=[%u,%u)\n",
		          scOff, scEnd, sdOff, sdEnd, ovOff, ovEnd);
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	_displayList = (uint8 *)memalign(64, kDisplayListSize);
	_vertexArena = (uint8 *)memalign(64, kVertexArenaSize);
	if (!_displayList || !_vertexArena) {
		_stats.allocFailures++;
		freeBuffers();
		_displayManager->restoreScreenBackingAfterNative();
		return false;
	}

	memset(_displayList, 0, kDisplayListSize);
	memset(_vertexArena, 0, kVertexArenaSize);
	sceKernelDcacheWritebackRange(_displayList, kDisplayListSize);
	sceKernelDcacheWritebackRange(_vertexArena, kVertexArenaSize);

	_sceneColorRel = (void *)(uintptr)scOff;
	_sceneDepthRel = (void *)(uintptr)sdOff;
	_stats.overlayOffset = ovOff;
	_stats.overlayEnd = ovEnd;
	_stats.sceneColorOffset = scOff;
	_stats.sceneColorEnd = scEnd;
	_stats.sceneDepthOffset = sdOff;
	_stats.sceneDepthEnd = sdEnd;
	_stats.freeEdramAfterDepth = kEdramSize - sdEnd;

	_vertexOffset = 0;
	_ownerName = ownerName;
	_active = true;
	_frameOpen = false;
	_sceneRendered = false;
	_suspended = false;
	_stats.acquires++;

	if (!_suspendRegistered) {
		_suspendRegistered = PowerManager::instance().registerForSuspend(&_suspendable);
	}
	syncGlobalCounters();
	logEdramMap();
	return true;
}

void PspGuSession::release() {
	if (_displayManager) {
		if (_frameOpen) {
			_displayManager->getMasterGuRenderer().abortSceneSegmentAndReleaseSema();
			_frameOpen = false;
		} else {
			_displayManager->getMasterGuRenderer().releaseNativeSemaIfIdle();
		}
		_displayManager->waitUntilRenderFinished();
	}
	sceGuSync(0, 0);

	if (_suspendRegistered) {
		PowerManager::instance().unregisterForSuspend(&_suspendable);
		_suspendRegistered = false;
	}

	const bool wasActive = _active;
	_active = false;
	_frameOpen = false;
	_suspended = false;
	_ownerName = 0;
	freeBuffers();

	if (wasActive && _displayManager) {
		_displayManager->restoreScreenBackingAfterNative();
		_stats.releases++;
		syncGlobalCounters();
	}
}

bool PspGuSession::beginFrame() {
	if (!_active || !_displayManager || _frameOpen || _suspended) {
		return false;
	}
	_displayManager->getMasterGuRenderer().beginSceneSegment(_displayList);
	_vertexOffset = 0;
	_frameOpen = true;
	bindSceneRenderTarget();
	syncGlobalCounters();
	return true;
}

void PspGuSession::bindSceneRenderTarget() {
	if (!_active || !_frameOpen || !_sceneColorRel || !_sceneDepthRel) {
		return;
	}
	sceGuDrawBufferList(GU_PSM_8888, _sceneColorRel, kSceneColorStride);
	sceGuDepthBuffer(_sceneDepthRel, kSceneDepthStride);
}

void *PspGuSession::getSceneColorUncached() const {
	return _sceneColorPtr ? UNCACHED(_sceneColorPtr) : 0;
}

void *PspGuSession::getCurrentLcdBackRel() const {
	return _displayManager ? _displayManager->getMasterGuRenderer().getCurrentLcdDrawBuffer() : 0;
}

void PspGuSession::primeScenePadding(uint32 sentinelAbgr) {
	if (!_sceneColorPtr) {
		return;
	}
	sceGuSync(0, 0);
	volatile uint32 *fb = (volatile uint32 *)UNCACHED(_sceneColorPtr);
	for (uint32 y = 0; y < kSceneHeight; ++y) {
		volatile uint32 *row = fb + y * kSceneColorStride + kSceneWidth;
		for (uint32 x = kSceneWidth; x < kSceneColorStride; ++x) {
			*row++ = sentinelAbgr;
		}
	}
	for (uint32 y = kSceneHeight; y < kSceneColorTexHeight; ++y) {
		volatile uint32 *row = fb + y * kSceneColorStride;
		for (uint32 x = 0; x < kSceneColorStride; ++x) {
			*row++ = sentinelAbgr;
		}
	}
	sceGuTexFlush();
}

bool PspGuSession::flushSceneSegment() {
	if (!_active || !_displayManager || !_frameOpen) {
		sceGuSync(0, 0);
		return false;
	}
	if (_vertexOffset > 0 && _vertexArena) {
		flushDataRange(_vertexArena, _vertexOffset);
	}
	const uint32 listBytes = getDisplayListBytesUsed();
	_stats.lastListBytes = listBytes;
	if (listBytes > _stats.peakListBytes) {
		_stats.peakListBytes = listBytes;
	}
	_displayManager->getMasterGuRenderer().finishSceneSegmentOnly();
	_stats.sceneSegments++;
	_sceneRendered = true;
	_frameOpen = false;
	return true;
}

bool PspGuSession::resumeSceneSegment() {
	if (!_active || !_displayManager || _frameOpen || _suspended) {
		return false;
	}
	_displayManager->getMasterGuRenderer().resumeSceneSegment(_displayList);
	_vertexOffset = 0;
	_frameOpen = true;
	bindSceneRenderTarget();
	return true;
}

bool PspGuSession::readSceneRectToRam(int rx, int ry, int rw, int rh, uint32 *dstRam, int dstStride) {
	if (!_active || !_sceneColorPtr || !dstRam || rw <= 0 || rh <= 0 || dstStride < rw) {
		return false;
	}
	if (rx < 0 || ry < 0 || rx + rw > (int)kSceneWidth || ry + rh > (int)kSceneHeight) {
		return false;
	}
	if (_frameOpen) {
		flushSceneSegment();
	} else {
		sceGuSync(0, 0);
	}

	const uint32 copyBytes = (uint32)dstStride * (uint32)rh * sizeof(uint32);
	sceKernelDcacheWritebackInvalidateRange(dstRam, copyBytes);

	sceGuStart(GU_DIRECT, _displayList);
	sceGuCopyImage(GU_PSM_8888, rx, ry, rw, rh, kSceneColorStride, _sceneColorPtr,
	               0, 0, dstStride, dstRam);
	sceGuTexSync();
	sceGuFinish();
	sceGuSync(0, 0);

	sceKernelDcacheInvalidateRange(dstRam, copyBytes);
	return true;
}

bool PspGuSession::emitLcdSceneBlitInActiveList() {
	if (!_active || !_sceneColorPtr || !_sceneRendered) {
		return false;
	}
	struct PresentVertex {
		float u, v;
		uint32 color;
		float x, y, z;
	};

	sceGuOffset(2048 - (kLcdWidth / 2), 2048 - (kLcdHeight / 2));
	sceGuViewport(2048, 2048, kLcdWidth, kLcdHeight);
	sceGuScissor(kPresentLeft, 0, kPresentRight - kPresentLeft, kLcdHeight);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFlush();
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, kSceneColorStride, kSceneColorTexHeight, kSceneColorStride, _sceneColorPtr);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuColor(0xFFFFFFFF);

	PresentVertex *verts = (PresentVertex *)sceGuGetMemory(2 * sizeof(PresentVertex));
	verts[0].u = 0.5f;
	verts[0].v = 0.5f;
	verts[0].color = 0xFFFFFFFF;
	verts[0].x = (float)kPresentLeft;
	verts[0].y = 0.0f;
	verts[0].z = 0.0f;

	verts[1].u = (float)kSceneWidth - 0.5f;
	verts[1].v = (float)kSceneHeight - 0.5f;
	verts[1].color = 0xFFFFFFFF;
	verts[1].x = (float)kPresentRight;
	verts[1].y = (float)kLcdHeight;
	verts[1].z = 0.0f;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, verts);
	return true;
}

void *PspGuSession::allocVertices(uint32 bytes, uint32 alignment) {
	if (!_active || !_frameOpen || !_vertexArena || bytes == 0) {
		return 0;
	}
	if (alignment < 4) {
		alignment = 4;
	}
	const uint32 alignedOffset = (_vertexOffset + alignment - 1) & ~(alignment - 1);
	if (alignedOffset > kVertexArenaSize || bytes > kVertexArenaSize - alignedOffset) {
		return 0;
	}
	void *ptr = _vertexArena + alignedOffset;
	_vertexOffset = alignedOffset + bytes;
	_stats.lastVertexBytes = _vertexOffset;
	if (_vertexOffset > _stats.peakVertexBytes) {
		_stats.peakVertexBytes = _vertexOffset;
	}
	return ptr;
}

void PspGuSession::flushDataRange(const void *ptr, uint32 bytes) const {
	if (!ptr || bytes == 0) {
		return;
	}
	const uintptr start = (uintptr)ptr & ~(uintptr)63;
	const uintptr end = ((uintptr)ptr + bytes + 63) & ~(uintptr)63;
	sceKernelDcacheWritebackRange((const void *)start, (unsigned int)(end - start));
}

uint32 PspGuSession::getDisplayListBytesUsed() const {
	if (!_frameOpen) {
		return _stats.lastListBytes;
	}
	const int used = sceGuCheckList();
	return used > 0 ? (uint32)used : 0;
}

bool PspGuSession::hasDisplayListHeadroom(uint32 neededBytes) const {
	if (!_frameOpen) {
		return false;
	}
	const uint32 used = getDisplayListBytesUsed();
	return used + neededBytes + kDisplayListReserve <= kDisplayListSize;
}

bool PspGuSession::overlayRePresent() {
	if (!_active || !_displayManager) {
		return false;
	}
	if (!_displayManager->hasAnyOverlayVisible()) {
		return false;
	}
	if (_frameOpen) {
		return submitFrame(true);
	}
	const bool presented = _displayManager->renderAll();
	if (presented) {
		_stats.overlayRePresents++;
	}
	syncGlobalCounters();
	return presented;
}

bool PspGuSession::submitFrame(bool compositeOverlay) {
	if (!_active || !_displayManager) {
		return false;
	}
	if (_frameOpen) {
		flushSceneSegment();
	} else if (!_sceneRendered) {
		return false;
	}

	_displayManager->getMasterGuRenderer().beginLcdPresentList(_displayList);
	sceGuOffset(2048 - (kLcdWidth / 2), 2048 - (kLcdHeight / 2));
	sceGuViewport(2048, 2048, kLcdWidth, kLcdHeight);
	sceGuScissor(0, 0, kLcdWidth, kLcdHeight);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuClearColor(0xFF000000);
	sceGuClear(GU_COLOR_BUFFER_BIT);

	emitLcdSceneBlitInActiveList();
	_stats.lcdPresents++;

	if (compositeOverlay && _displayManager->hasAnyOverlayVisible()) {
		_displayManager->renderOverlayClients(true);
		_stats.overlayRePresents++;
	}

	_displayManager->getMasterGuRenderer().finishLcdPresentList();
	syncGlobalCounters();
	return true;
}

void PspGuSession::waitFrame() {
	if (_displayManager) {
		_displayManager->waitUntilRenderFinished();
	}
	sceGuSync(0, 0);
	syncGlobalCounters();
}

int PspGuSession::onSuspend() {
	if (!_active) {
		return 0;
	}
	_suspended = true;
	if (_displayManager) {
		if (_frameOpen) {
			_displayManager->getMasterGuRenderer().abortSceneSegmentAndReleaseSema();
			_frameOpen = false;
		} else {
			_displayManager->getMasterGuRenderer().releaseNativeSemaIfIdle();
		}
		_displayManager->waitUntilRenderFinished();
	}
	sceGuSync(0, 0);
	_stats.suspendCount++;
	syncGlobalCounters();
	return 0;
}

int PspGuSession::onResume() {
	if (!_active) {
		return 0;
	}
	_suspended = false;
	_stats.resumeCount++;
	if (_displayManager && _displayManager->hasAnyOverlayVisible()) {
		overlayRePresent();
	}
	syncGlobalCounters();
	return 0;
}


// Class DisplayManager -----------------------------------------------------

DisplayManager::~DisplayManager() {
	_nativeSession.release();
	_masterGuRenderer.guShutDown();
}

void DisplayManager::init() {
	DEBUG_ENTER_FUNC();

	_nativeSession.bindDisplayManager(this);

	_displayParams.outputBitsPerPixel = 32;	// can be changed to produce 16-bit output

	GuRenderer::setDisplayManager(this);
	_screen->init();
	_overlay->init();
	_cursor->init();

	_masterGuRenderer.guInit();				// start up the renderer
#ifdef USE_DISPLAY_CALLBACK
	_masterGuRenderer.setupCallbackThread();
#endif

	// Init overlay since we never change the size
	_overlay->deallocate();
	_overlay->setBytesPerPixel(sizeof(uint16));
	_overlay->setSize(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	_overlay->allocate();
}

void DisplayManager::setSizeAndPixelFormat(uint width, uint height, const Graphics::PixelFormat *format) {
	DEBUG_ENTER_FUNC();
	PSP_DEBUG_PRINT("w[%u], h[%u], pformat[%p]\n", width, height, format);

	_screen->deallocate();

	_screen->setScummvmPixelFormat(format);
	_screen->setSize(width, height);
	if (!_nativeSession.isActive()) {
		_screen->allocate();
	}

	_cursor->setScreenPaletteScummvmPixelFormat(format);

	_displayParams.screenSource.width = width;
	_displayParams.screenSource.height = height;
	calculateScaleParams();
}

bool DisplayManager::setGraphicsMode(int mode) {
	DEBUG_ENTER_FUNC();

	_graphicsMode = mode;

	calculateScaleParams();

	return true;
}

void DisplayManager::calculateScaleParams() {

	if (!_displayParams.screenSource.width || !_displayParams.screenSource.height)
		return; // we can't calculate anything without these

	switch (_graphicsMode) {
	case ORIGINAL_RESOLUTION:
		// check if we can fit the original resolution inside the screen
		if ((_displayParams.screenSource.width <= PSP_SCREEN_WIDTH) &&
			(_displayParams.screenSource.height <= PSP_SCREEN_HEIGHT)) {
			_displayParams.screenOutput.width =  _displayParams.screenSource.width;
			_displayParams.screenOutput.height =  _displayParams.screenSource.height;
			break;
		}
		// else revert to fit to screen
	case FIT_TO_SCREEN: { // maximize the height while keeping aspect ratio
			float aspectRatio;

			if (ConfMan.getBool("aspect_ratio")) {
				aspectRatio = 4.0f / 3.0f;
			} else {
				aspectRatio = (float)_displayParams.screenSource.width / (float)_displayParams.screenSource.height;
			}

			_displayParams.screenOutput.height = PSP_SCREEN_HEIGHT; // always full height
			_displayParams.screenOutput.width = (uint32)(PSP_SCREEN_HEIGHT * aspectRatio);

			if (_displayParams.screenOutput.width > PSP_SCREEN_WIDTH) { // shrink if wider than screen
				_displayParams.screenOutput.height = (uint32)(((float)PSP_SCREEN_HEIGHT * (float)PSP_SCREEN_WIDTH) / (float)_displayParams.screenOutput.width);
				_displayParams.screenOutput.width = PSP_SCREEN_WIDTH;
			}
		}
		break;
	case STRETCH_TO_SCREEN: // we simply stretch to the whole screen
		_displayParams.screenOutput.width = PSP_SCREEN_WIDTH;
		_displayParams.screenOutput.height = PSP_SCREEN_HEIGHT;
		break;
	default:
		PSP_ERROR("Unsupported graphics mode[%d].\n", _graphicsMode);
	}

	// calculate scale factors for X and Y
	_displayParams.scaleX = ((float)_displayParams.screenOutput.width) / _displayParams.screenSource.width;
	_displayParams.scaleY = ((float)_displayParams.screenOutput.height) / _displayParams.screenSource.height;

}

void DisplayManager::waitUntilRenderFinished() {
#ifdef USE_DISPLAY_CALLBACK
	_masterGuRenderer.sleepUntilRenderFinished();
#endif /* USE_DISPLAY_CALLBACK */
}

void DisplayManager::releaseNativeSessionOnEngineDone() {
	if (_nativeSession.isActive()) {
		_nativeSession.release();
	}
}

void DisplayManager::releaseScreenBackingForNative() {
	waitUntilRenderFinished();
	sceGuSync(0, 0);
	if (_screen) {
		_screen->deallocate();
	}
}

void DisplayManager::restoreScreenBackingAfterNative() {
	waitUntilRenderFinished();
	sceGuSync(0, 0);
	if (_screen && _screen->getWidth() > 0 && _screen->getHeight() > 0) {
		_screen->allocate();
		_screen->setDirty();
	}
}

void DisplayManager::getOverlayVramRange(uint32 &offset, uint32 &end) const {
	offset = 0;
	end = 0;
	if (_overlay) {
		byte *pixels = _overlay->buffer().getPixels();
		if (pixels && VramAllocator::isAddressInVram(pixels)) {
			const uint32 addr = (uint32)CACHED(pixels);
			offset = addr - PspGuSession::kEdramBase;
			end = offset + _overlay->buffer().getSizeInBytes();
		}
	}
}

bool DisplayManager::hasAnyOverlayVisible() const {
	return (_imageViewer && _imageViewer->isVisible()) ||
	       (_overlay && _overlay->isVisible()) ||
	       (_cursor && _cursor->isVisible()) ||
	       (_keyboard && _keyboard->isVisible());
}

void DisplayManager::prepareOverlayGuState() {
	sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_LIGHTING);
	sceGuFrontFace(GU_CW);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuAmbientColor(0xFFFFFFFF);
	sceGuColor(0xFFFFFFFF);
	sceGuTexOffset(0, 0);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
}

void DisplayManager::renderOverlayClients(bool markClean) {
	prepareOverlayGuState();

	if (_imageViewer->isVisible())
		_imageViewer->render();
	if (markClean)
		_imageViewer->setClean();

	if (_overlay->isVisible())
		_overlay->render();
	if (markClean)
		_overlay->setClean();

	if (_cursor->isVisible())
		_cursor->render();
	if (markClean)
		_cursor->setClean();

	if (_keyboard->isVisible())
		_keyboard->render();
	if (markClean)
		_keyboard->setClean();
}

// return true if we really rendered or no dirty. False otherwise
bool DisplayManager::renderAll() {
	DEBUG_ENTER_FUNC();

#ifdef USE_DISPLAY_CALLBACK
	if (!_masterGuRenderer.isRenderFinished()) {
		PSP_DEBUG_PRINT("Callback render not finished.\n");
		return false;	// didn't render
	}
#endif /* USE_DISPLAY_CALLBACK */

	if (_nativeSession.isActive()) {
		if (_nativeSession.isFrameOpen()) {
			return true;
		}
		if (!hasAnyOverlayVisible()) {
			_screen->setClean();
			_imageViewer->setClean();
			_overlay->setClean();
			_cursor->setClean();
			_keyboard->setClean();
			return true;
		}
		if (!_imageViewer->isDirty() &&
			!_overlay->isDirty() &&
			!_cursor->isDirty() &&
			!_keyboard->isDirty()) {
			return true;
		}
		if (!isTimeToUpdate())
			return false;

		_masterGuRenderer.guPreRender();
		_screen->setClean();
		_nativeSession.emitLcdSceneBlitInActiveList();
		renderOverlayClients(true);
		_masterGuRenderer.guPostRender();
		return true;
	}

	// This is cheaper than checking time, so we do it first
	// Any one of these being dirty causes everything to draw
	if (!_screen->isDirty() &&
		!_overlay->isDirty() &&
		!_cursor->isDirty() &&
		!_keyboard->isDirty() &&
		!_imageViewer->isDirty()) {
		PSP_DEBUG_PRINT("Nothing dirty\n");
		return true;	// nothing to render
	}

	if (!isTimeToUpdate())
		return false;	// didn't render

	PSP_DEBUG_PRINT("dirty: screen[%s], overlay[%s], cursor[%s], keyboard[%s], imageViewer[%s]\n",
	                _screen->isDirty() ? "true" : "false",
	                _overlay->isDirty() ? "true" : "false",
	                _cursor->isDirty() ? "true" : "false",
	                _keyboard->isDirty() ? "true" : "false",
					_imageViewer->isDirty() ? "true" : "false"
	               );

	_masterGuRenderer.guPreRender();	// Set up rendering

	_screen->render();
	_screen->setClean();				// clean out dirty bit

	renderOverlayClients(true);

	_masterGuRenderer.guPostRender();

	return true;	// rendered successfully
}

inline bool DisplayManager::isTimeToUpdate() {

#define MAX_FPS 60 // was 30

	uint32 now = g_system->getMillis();
	if (now - _lastUpdateTime < (1000 / MAX_FPS))
		return false;

	_lastUpdateTime = now;
	return true;
}

Common::List<Graphics::PixelFormat> DisplayManager::getSupportedPixelFormats() const {
	Common::List<Graphics::PixelFormat> list;

	// In order of preference
	// Note that the OSystem documentation suggests that the format with the highest
	// depth should come first, but for PSP BGR565 is listed first instead to reduce
	// memory usage
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_5650));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_5551, false));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_5551, true));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_4444, false));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_4444, true));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_8888, false));
	list.push_back(PSPPixelFormat::convertToScummvmPixelFormat(PSPPixelFormat::Type_8888, true));
	list.push_back(Graphics::PixelFormat::createFormatCLUT8());

	return list;
}
