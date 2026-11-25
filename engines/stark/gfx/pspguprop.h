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

#ifndef STARK_GFX_PSPGU_PROP_H
#define STARK_GFX_PSPGU_PROP_H

#include "engines/stark/visual/prop.h"
#include "common/array.h"

namespace Stark {
namespace Gfx {

class PspGuDriver;

/**
 * Native PSP GE 3D prop renderer with cached indexed static batches,
 * material UV orientation, and hardware/CPU-fallback lighting.
 */
class PspGuPropRenderer : public VisualProp {
public:
	struct PropVertexHW {
		float u, v;
		float nx, ny, nz;
		float x, y, z;
	};

	struct PropVertexCpuLit {
		float u, v;
		uint32 color;
		float x, y, z;
	};

	struct Batch {
		uint32 materialId;
		PropVertexHW *verts;
		uint16 *indices;
		uint32 vertexCount;
		uint32 indexCount;
		uint32 allocBytes;
	};

	explicit PspGuPropRenderer(PspGuDriver *gfx);
	~PspGuPropRenderer() override;

	void render(const Math::Vector3d &position, float direction, const Gfx::LightEntryArray &lights) override;

private:
	friend class PspGuDriver;

	void clearBatches();
	void buildBatches();

	PspGuDriver *_gfx;
	const Formats::BiffMesh *_cachedModel;
	Common::Array<Batch> _batches;
	PropVertexCpuLit *_cpuLitScratch;
	uint32 _cpuLitScratchCap;
	uint32 _lastDrawnFrameSeq;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_PROP_H
