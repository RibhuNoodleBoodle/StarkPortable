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

#ifndef STARK_GFX_PSPGU_ACTOR_H
#define STARK_GFX_PSPGU_ACTOR_H

#include "engines/stark/visual/actor.h"
#include "common/array.h"

namespace Stark {

class BoneNode;

namespace Gfx {

class PspGuDriver;

/**
 * Native PSP GE 3D actor renderer with static hardware-skinned (<=8 bone)
 * batches for rigid geometry, bind-equivalence audited seam handling, planar
 * stencil-masked shadows, facial texture resolution, and hardware/CPU lighting.
 */
class PspGuActorRenderer : public VisualActor {
public:
	struct RigidBatch {
		uint32 materialId;
		uint8 bones[8];
		uint8 boneCount;
		int vtype;
		uint32 floatStride;
		float *verts;
		uint16 *indices;
		uint32 vertexCount;
		uint32 indexCount;
		uint32 allocBytes;
	};

	struct SeamVertex {
		float pos1[3];
		float pos2[3];
		float normal[3];
		float u, v;
		uint16 bone1;
		uint16 bone2;
		float weight;
	};

	struct SeamVertHW {
		float u, v;
		float nx, ny, nz;
		float x, y, z;
	};

	struct SeamVertCpuLit {
		float u, v;
		uint32 color;
		float x, y, z;
	};

	struct SeamBatch {
		uint32 materialId;
		SeamVertex *srcVerts;
		uint16 *indices;
		SeamVertHW *dynHwVerts;
		SeamVertCpuLit *dynCpuVerts;
		uint32 vertexCount;
		uint32 indexCount;
		uint32 staticAllocBytes;
		uint32 dynAllocBytes;
	};

	explicit PspGuActorRenderer(PspGuDriver *gfx);
	~PspGuActorRenderer() override;

	void render(const Math::Vector3d &position, float direction, const Common::Array<Gfx::LightEntry *> &lights) override;

private:
	void clearBatches();
	void buildBatches();
	void evaluateBindAuditAndSeamSkin(const Common::Array<Math::Matrix4> &boneMats, bool canHwLights);
	Math::Vector3d computeShadowLightDirection(const Common::Array<Gfx::LightEntry *> &lights,
	                                           const Math::Vector3d &actorPosition,
	                                           const Math::Matrix3 &worldToModelRot) const;
	void renderShadowPass(const Math::Matrix4 &model, const Math::Vector3d &position,
	                      const Common::Array<Gfx::LightEntry *> &lights,
	                      const Common::Array<BoneNode *> &bones) const;

	PspGuDriver *_gfx;
	Common::Array<RigidBatch> _rigidBatches;
	Common::Array<SeamBatch> _seamBatches;
	SeamVertCpuLit *_rigidCpuScratch;
	uint32 _rigidCpuScratchCap;
	bool _bindAudited;
	uint32 _lastDrawnFrameSeq;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_ACTOR_H
