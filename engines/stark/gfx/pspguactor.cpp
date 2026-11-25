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

#include "engines/stark/gfx/pspguactor.h"
#include "engines/stark/gfx/pspgu.h"
#include "engines/stark/gfx/texture.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <pspgu.h>
#include <pspgum.h>

#include "backends/platform/psp/psp_gu_session.h"
#include "common/debug.h"
#include "common/hashmap.h"
#include "engines/stark/model/animhandler.h"
#include "engines/stark/model/model.h"
#include "engines/stark/scene.h"
#include "engines/stark/services/services.h"
#include "engines/stark/services/settings.h"
#include "math/vector2d.h"

namespace Stark {
namespace Gfx {

namespace {

const uint32 kMaxBatchVertices = 65535;
const uint32 kMaxBatchIndices = 49140; // multiple of 3 (16380 triangles)

inline bool isVertRigid(const VertNode *v) {
	return (v->_bone1 == v->_bone2) || (v->_boneWeight >= 0.9999f) || (v->_boneWeight <= 0.0001f);
}

inline uint32 getRigidVertBone(const VertNode *v) {
	return (v->_boneWeight <= 0.0001f) ? v->_bone2 : v->_bone1;
}

inline const Math::Vector3d &getRigidVertPos(const VertNode *v) {
	return (v->_boneWeight <= 0.0001f) ? v->_pos2 : v->_pos1;
}

} // namespace

PspGuActorRenderer::PspGuActorRenderer(PspGuDriver *gfx) :
		VisualActor(),
		_gfx(gfx),
		_rigidCpuScratch(nullptr),
		_rigidCpuScratchCap(0),
		_bindAudited(false),
		_lastDrawnFrameSeq(0) {
}

PspGuActorRenderer::~PspGuActorRenderer() {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);
	clearBatches();
	if (_rigidCpuScratch) {
		pspGuTrackAuxBytes(-(int32)(_rigidCpuScratchCap * sizeof(SeamVertCpuLit)));
		free(_rigidCpuScratch);
		_rigidCpuScratch = nullptr;
		_rigidCpuScratchCap = 0;
	}
}

void PspGuActorRenderer::clearBatches() {
	for (uint32 i = 0; i < _rigidBatches.size(); ++i) {
		free(_rigidBatches[i].verts);
		free(_rigidBatches[i].indices);
		pspGuTrackAuxBytes(-(int32)_rigidBatches[i].allocBytes);
	}
	_rigidBatches.clear();

	for (uint32 i = 0; i < _seamBatches.size(); ++i) {
		free(_seamBatches[i].srcVerts);
		free(_seamBatches[i].indices);
		free(_seamBatches[i].dynHwVerts);
		free(_seamBatches[i].dynCpuVerts);
		pspGuTrackAuxBytes(-(int32)(_seamBatches[i].staticAllocBytes + _seamBatches[i].dynAllocBytes));
	}
	_seamBatches.clear();
	_bindAudited = false;
}

void PspGuActorRenderer::buildBatches() {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);
	clearBatches();

	if (!_model) {
		return;
	}

	const Common::Array<VertNode *> &srcVerts = _model->getVertices();
	const Common::Array<Face *> &faces = _model->getFaces();
	const Common::Array<Material *> &mats = _model->getMaterials();
	if (srcVerts.empty() || faces.empty()) {
		return;
	}

	uint32 rigidTriTotal = 0;
	uint32 seamTriTotal = 0;

	for (uint32 fi = 0; fi < faces.size(); ++fi) {
		const Face *face = faces[fi];
		if (!face || face->vertexIndices.size() < 3 || face->materialId >= mats.size()) {
			continue;
		}
		const uint32 triCount = face->vertexIndices.size() / 3;

		// Accumulate rigid batches (<=8 bones/batch) and seam triangles for this face
		Common::Array<uint32> batchVertSrc;
		Common::Array<uint16> batchTriIdx;
		Common::Array<uint8> batchBones;
		Common::HashMap<uint32, uint16> rigidRemap;

		Common::Array<SeamVertex> seamVerts;
		Common::Array<uint16> seamIdx;
		Common::HashMap<uint32, uint16> seamRemap;

		auto flushRigidBatch = [&]() {
			if (batchTriIdx.empty() || batchVertSrc.empty() || batchBones.empty()) {
				batchVertSrc.clear();
				batchTriIdx.clear();
				batchBones.clear();
				rigidRemap.clear();
				return;
			}
			const uint8 bCount = (uint8)batchBones.size();
			const uint32 weightFloats = (bCount > 1) ? bCount : 0;
			const uint32 fStride = weightFloats + 8;
			const uint32 vBytes = batchVertSrc.size() * fStride * sizeof(float);
			const uint32 iBytes = batchTriIdx.size() * sizeof(uint16);

			float *vMem = static_cast<float *>(memalign(64, vBytes));
			uint16 *iMem = static_cast<uint16 *>(memalign(64, iBytes));
			if (!vMem || !iMem) {
				free(vMem);
				free(iMem);
				warning("PspGuActorRenderer: OOM allocating rigid batch");
			} else {
				uint8 boneToSlot[256];
				memset(boneToSlot, 0, sizeof(boneToSlot));
				for (uint8 s = 0; s < bCount; ++s) {
					boneToSlot[batchBones[s]] = s;
				}
				for (uint32 vi = 0; vi < batchVertSrc.size(); ++vi) {
					const VertNode *vn = srcVerts[batchVertSrc[vi]];
					float *dst = vMem + vi * fStride;
					if (weightFloats > 0) {
						const uint32 activeBone = getRigidVertBone(vn);
						const uint8 activeSlot = (activeBone < 256) ? boneToSlot[activeBone] : 0;
						for (uint32 s = 0; s < weightFloats; ++s) {
							dst[s] = (s == activeSlot) ? 1.0f : 0.0f;
						}
					}
					const Math::Vector3d &p = getRigidVertPos(vn);
					dst[weightFloats + 0] = vn->_texS;
					dst[weightFloats + 1] = vn->_texT;
					dst[weightFloats + 2] = vn->_normal.x();
					dst[weightFloats + 3] = vn->_normal.y();
					dst[weightFloats + 4] = vn->_normal.z();
					dst[weightFloats + 5] = p.x();
					dst[weightFloats + 6] = p.y();
					dst[weightFloats + 7] = p.z();
				}
				memcpy(iMem, batchTriIdx.begin(), iBytes);
				pspGuWritebackCache64(vMem, vBytes);
				pspGuWritebackCache64(iMem, iBytes);

				RigidBatch rb;
				memset(&rb, 0, sizeof(rb));
				rb.materialId = face->materialId;
				rb.boneCount = bCount;
				for (uint8 s = 0; s < bCount; ++s) {
					rb.bones[s] = batchBones[s];
				}
				rb.vtype = ((bCount > 1) ? (GU_WEIGHTS(bCount) | GU_WEIGHT_32BITF) : 0) |
				           GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF |
				           GU_INDEX_16BIT | GU_TRANSFORM_3D;
				rb.floatStride = fStride;
				rb.verts = vMem;
				rb.indices = iMem;
				rb.vertexCount = batchVertSrc.size();
				rb.indexCount = batchTriIdx.size();
				rb.allocBytes = vBytes + iBytes;
				pspGuTrackAuxBytes((int32)rb.allocBytes);
				_rigidBatches.push_back(rb);
			}

			batchVertSrc.clear();
			batchTriIdx.clear();
			batchBones.clear();
			rigidRemap.clear();
		};

		auto flushSeamBatch = [&]() {
			if (seamIdx.empty() || seamVerts.empty()) {
				seamVerts.clear();
				seamIdx.clear();
				seamRemap.clear();
				return;
			}
			const uint32 sBytes = seamVerts.size() * sizeof(SeamVertex);
			const uint32 iBytes = seamIdx.size() * sizeof(uint16);
			const uint32 hwBytes = seamVerts.size() * sizeof(SeamVertHW);
			const uint32 cpuBytes = seamVerts.size() * sizeof(SeamVertCpuLit);

			SeamVertex *sMem = static_cast<SeamVertex *>(memalign(64, sBytes));
			uint16 *iMem = static_cast<uint16 *>(memalign(64, iBytes));
			SeamVertHW *hwMem = static_cast<SeamVertHW *>(memalign(64, hwBytes));
			SeamVertCpuLit *cpuMem = static_cast<SeamVertCpuLit *>(memalign(64, cpuBytes));
			if (!sMem || !iMem || !hwMem || !cpuMem) {
				free(sMem); free(iMem); free(hwMem); free(cpuMem);
				warning("PspGuActorRenderer: OOM allocating seam batch");
			} else {
				memcpy(sMem, seamVerts.begin(), sBytes);
				memcpy(iMem, seamIdx.begin(), iBytes);
				pspGuWritebackCache64(iMem, iBytes);

				SeamBatch sb;
				sb.materialId = face->materialId;
				sb.srcVerts = sMem;
				sb.indices = iMem;
				sb.dynHwVerts = hwMem;
				sb.dynCpuVerts = cpuMem;
				sb.vertexCount = seamVerts.size();
				sb.indexCount = seamIdx.size();
				sb.staticAllocBytes = sBytes + iBytes;
				sb.dynAllocBytes = hwBytes + cpuBytes;
				pspGuTrackAuxBytes((int32)(sb.staticAllocBytes + sb.dynAllocBytes));
				_seamBatches.push_back(sb);
			}
			seamVerts.clear();
			seamIdx.clear();
			seamRemap.clear();
		};

		for (uint32 ti = 0; ti < triCount; ++ti) {
			const uint32 i0 = face->vertexIndices[ti * 3 + 0];
			const uint32 i1 = face->vertexIndices[ti * 3 + 1];
			const uint32 i2 = face->vertexIndices[ti * 3 + 2];
			if (i0 >= srcVerts.size() || i1 >= srcVerts.size() || i2 >= srcVerts.size()) {
				continue;
			}
			const VertNode *v0 = srcVerts[i0];
			const VertNode *v1 = srcVerts[i1];
			const VertNode *v2 = srcVerts[i2];

			if (isVertRigid(v0) && isVertRigid(v1) && isVertRigid(v2)) {
				++rigidTriTotal;
				const uint32 triSrc[3] = {i0, i1, i2};
				const uint32 triBones[3] = {getRigidVertBone(v0), getRigidVertBone(v1), getRigidVertBone(v2)};

				uint32 addBones[3];
				uint32 addBoneCount = 0;
				for (int k = 0; k < 3; ++k) {
					const uint8 b8 = (uint8)triBones[k];
					bool found = false;
					for (uint32 e = 0; e < batchBones.size(); ++e) {
						if (batchBones[e] == b8) { found = true; break; }
					}
					for (uint32 e = 0; e < addBoneCount && !found; ++e) {
						if (addBones[e] == b8) { found = true; break; }
					}
					if (!found) {
						addBones[addBoneCount++] = b8;
					}
				}

				uint32 newVerts = 0;
				if (!rigidRemap.contains(i0)) ++newVerts;
				if (i1 != i0 && !rigidRemap.contains(i1)) ++newVerts;
				if (i2 != i0 && i2 != i1 && !rigidRemap.contains(i2)) ++newVerts;

				if (batchBones.size() + addBoneCount > 8 ||
				    batchVertSrc.size() + newVerts > kMaxBatchVertices ||
				    batchTriIdx.size() + 3 > kMaxBatchIndices) {
					flushRigidBatch();
					addBoneCount = 0;
					for (int k = 0; k < 3; ++k) {
						const uint8 b8 = (uint8)triBones[k];
						bool found = false;
						for (uint32 e = 0; e < addBoneCount; ++e) {
							if (addBones[e] == b8) { found = true; break; }
						}
						if (!found) addBones[addBoneCount++] = b8;
					}
				}
				for (uint32 e = 0; e < addBoneCount; ++e) {
					batchBones.push_back((uint8)addBones[e]);
				}
				for (int k = 0; k < 3; ++k) {
					const uint32 srcIdx = triSrc[k];
					uint16 dstIdx;
					if (rigidRemap.contains(srcIdx)) {
						dstIdx = rigidRemap[srcIdx];
					} else {
						dstIdx = (uint16)batchVertSrc.size();
						rigidRemap[srcIdx] = dstIdx;
						batchVertSrc.push_back(srcIdx);
					}
					batchTriIdx.push_back(dstIdx);
				}
			} else {
				++seamTriTotal;
				uint32 newVerts = 0;
				if (!seamRemap.contains(i0)) ++newVerts;
				if (i1 != i0 && !seamRemap.contains(i1)) ++newVerts;
				if (i2 != i0 && i2 != i1 && !seamRemap.contains(i2)) ++newVerts;
				if (seamVerts.size() + newVerts > kMaxBatchVertices ||
				    seamIdx.size() + 3 > kMaxBatchIndices) {
					flushSeamBatch();
				}
				const uint32 triSrc[3] = {i0, i1, i2};
				for (int k = 0; k < 3; ++k) {
					const uint32 srcIdx = triSrc[k];
					uint16 dstIdx;
					if (seamRemap.contains(srcIdx)) {
						dstIdx = seamRemap[srcIdx];
					} else {
						dstIdx = (uint16)seamVerts.size();
						seamRemap[srcIdx] = dstIdx;
						const VertNode *vn = srcVerts[srcIdx];
						SeamVertex sv;
						sv.pos1[0] = vn->_pos1.x(); sv.pos1[1] = vn->_pos1.y(); sv.pos1[2] = vn->_pos1.z();
						sv.pos2[0] = vn->_pos2.x(); sv.pos2[1] = vn->_pos2.y(); sv.pos2[2] = vn->_pos2.z();
						sv.normal[0] = vn->_normal.x(); sv.normal[1] = vn->_normal.y(); sv.normal[2] = vn->_normal.z();
						sv.u = vn->_texS;
						sv.v = vn->_texT;
						sv.bone1 = (uint16)vn->_bone1;
						sv.bone2 = (uint16)vn->_bone2;
						sv.weight = vn->_boneWeight;
						seamVerts.push_back(sv);
					}
					seamIdx.push_back(dstIdx);
				}
			}
		}

		flushRigidBatch();
		flushSeamBatch();
	}

	debug("[STARK-PSP-STEP04] actor batches built: bones=%u faces=%u rigidTris=%u(batches=%u) seamTris=%u(batches=%u)",
	      _model->getBones().size(), faces.size(),
	      rigidTriTotal, _rigidBatches.size(),
	      seamTriTotal, _seamBatches.size());
}

void PspGuActorRenderer::evaluateBindAuditAndSeamSkin(const Common::Array<Math::Matrix4> &boneMats, bool canHwLights) {
	const Common::Array<BoneNode *> &bones = _model->getBones();

	if (!_bindAudited) {
		float worstPosErr = 0.0f;
		float worstNormErr = 0.0f;
		for (uint32 bi = 0; bi < _seamBatches.size(); ++bi) {
			const SeamBatch &sb = _seamBatches[bi];
			for (uint32 vi = 0; vi < sb.vertexCount; ++vi) {
				const SeamVertex &sv = sb.srcVerts[vi];
				if (sv.bone1 >= bones.size() || sv.bone2 >= bones.size()) {
					continue;
				}
				Math::Vector3d p1(sv.pos1[0], sv.pos1[1], sv.pos1[2]);
				Math::Vector3d p2(sv.pos2[0], sv.pos2[1], sv.pos2[2]);
				bones[sv.bone1]->_animRot.transform(p1);
				p1 += bones[sv.bone1]->_animPos;
				bones[sv.bone2]->_animRot.transform(p2);
				p2 += bones[sv.bone2]->_animPos;
				const float pe = (p1 - p2).getMagnitude();
				if (pe > worstPosErr) worstPosErr = pe;

				Math::Vector3d n(sv.normal[0], sv.normal[1], sv.normal[2]);
				Math::Vector3d n1 = n;
				Math::Vector3d n2 = n;
				bones[sv.bone1]->_animRot.transform(n1);
				bones[sv.bone2]->_animRot.transform(n2);
				const float ne = (n1 - n2).getMagnitude();
				if (ne > worstNormErr) worstNormErr = ne;
			}
		}
		_bindAudited = true;
		if (!_seamBatches.empty()) {
			debug("[STARK-PSP-STEP04] actor bind audit: seamMode=cpuDualPose reason=dualBoneLocalPosAndSharedNormal worstPosErr=%.4f worstNormErr=%.4f",
			      worstPosErr, worstNormErr);
		}
	}

	if (!canHwLights) {
		// Evaluated later inside light-enabled CPU loop when lighting per-batch material color
		return;
	}

	for (uint32 bi = 0; bi < _seamBatches.size(); ++bi) {
		const SeamBatch &sb = _seamBatches[bi];
		for (uint32 vi = 0; vi < sb.vertexCount; ++vi) {
			const SeamVertex &sv = sb.srcVerts[vi];
			const BoneNode *bn1 = bones[sv.bone1];
			const BoneNode *bn2 = bones[sv.bone2];

			Math::Vector3d p1(sv.pos1[0], sv.pos1[1], sv.pos1[2]);
			Math::Vector3d p2(sv.pos2[0], sv.pos2[1], sv.pos2[2]);
			bn1->_animRot.transform(p1);
			p1 += bn1->_animPos;
			bn2->_animRot.transform(p2);
			p2 += bn2->_animPos;
			const Math::Vector3d mp = Math::Vector3d::interpolate(p2, p1, sv.weight);

			const Math::Vector3d rawN(sv.normal[0], sv.normal[1], sv.normal[2]);
			Math::Vector3d n1 = rawN;
			Math::Vector3d n2 = rawN;
			bn1->_animRot.transform(n1);
			bn2->_animRot.transform(n2);
			const Math::Vector3d mn = Math::Vector3d::interpolate(n2, n1, sv.weight).getNormalized();

			SeamVertHW &dv = sb.dynHwVerts[vi];
			dv.u = sv.u;
			dv.v = sv.v;
			dv.nx = mn.x(); dv.ny = mn.y(); dv.nz = mn.z();
			dv.x = mp.x();  dv.y = mp.y();  dv.z = mp.z();
		}
		pspGuWritebackCache64(sb.dynHwVerts, sb.vertexCount * sizeof(SeamVertHW));
	}
	(void)boneMats;
}

void PspGuActorRenderer::render(const Math::Vector3d &position, float direction, const Common::Array<Gfx::LightEntry *> &lights) {
	if (!_gfx || !_model || !_animHandler) {
		return;
	}
	if (_modelIsDirty) {
		buildBatches();
		_modelIsDirty = false;
	}

	_animHandler->animate(_time);
	_model->updateBoundingBox();

	if (!_gfx->ensureFrameBegun()) {
		return;
	}

	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);

	const Common::Array<BoneNode *> &bones = _model->getBones();
	Common::Array<Math::Matrix4> boneMats;
	boneMats.resize(bones.size());
	for (uint32 i = 0; i < bones.size(); ++i) {
		Math::Matrix4 m = bones[i]->_animRot.toMatrix();
		m.setPosition(bones[i]->_animPos);
		boneMats[i] = m;
	}

	const bool hwLights = pspGuCanUseHardwareLights(lights);
	evaluateBindAuditAndSeamSkin(boneMats, hwLights);

	_gfx->set3DMode();

	const Math::Matrix4 model = getModelMatrix(position, direction);
	const Math::Matrix4 view = StarkScene->getViewMatrix();
	const Math::Matrix4 projection = StarkScene->getProjectionMatrix();

	pspGuLoadMatrix4(GU_PROJECTION, projection);
	pspGuLoadMatrix4(GU_VIEW, view);
	pspGuLoadMatrix4(GU_MODEL, model);

	const Math::Matrix4 modelView = view * model;
	Math::Matrix4 normalRot = modelView;
	normalRot.invertAffineOrthonormal();

	const Common::Array<Material *> &mats = _model->getMaterials();

	auto bindMaterialAndLights = [&](uint32 matId, Math::Vector3d &outBaseRgb) {
		const Material *mat = mats[matId];
		const Gfx::Texture *tex = _textureSet ? resolveTexture(mat) : nullptr;
		if (tex) {
			tex->bind();
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			outBaseRgb = Math::Vector3d(1.0f, 1.0f, 1.0f);
		} else {
			sceGuDisable(GU_TEXTURE_2D);
			outBaseRgb = Math::Vector3d(mat->r, mat->g, mat->b);
		}
		if (hwLights) {
			pspGuSetupLightingOrCpuMode(lights, outBaseRgb);
		} else {
			sceGuDisable(GU_LIGHTING);
			pspGuDisableAllLights();
		}
	};

	// 1. Static hardware-skinned rigid batches (1-bone direct model*bone, or 2..8-bone sceGuBoneMatrix)
	for (uint32 bi = 0; bi < _rigidBatches.size(); ++bi) {
		const RigidBatch &rb = _rigidBatches[bi];
		Math::Vector3d baseRgb;
		bindMaterialAndLights(rb.materialId, baseRgb);

		if (hwLights) {
			if (rb.boneCount == 1) {
				const Math::Matrix4 m = model * boneMats[rb.bones[0]];
				pspGuLoadMatrix4(GU_MODEL, m);
			} else {
				pspGuLoadMatrix4(GU_MODEL, model);
				for (uint8 s = 0; s < rb.boneCount; ++s) {
					alignas(16) ScePspFMatrix4 bMat;
					pspGuToSceMatrix(boneMats[rb.bones[s]], bMat);
					sceGuBoneMatrix(s, &bMat);
				}
			}
			sceGumDrawArray(GU_TRIANGLES, rb.vtype, (int)rb.indexCount, rb.indices, rb.verts);
		} else {
			if (_rigidCpuScratchCap < rb.vertexCount) {
				if (_rigidCpuScratch) {
					pspGuTrackAuxBytes(-(int32)(_rigidCpuScratchCap * sizeof(SeamVertCpuLit)));
					free(_rigidCpuScratch);
				}
				_rigidCpuScratchCap = rb.vertexCount;
				_rigidCpuScratch = static_cast<SeamVertCpuLit *>(memalign(64, _rigidCpuScratchCap * sizeof(SeamVertCpuLit)));
				if (_rigidCpuScratch) {
					pspGuTrackAuxBytes((int32)(_rigidCpuScratchCap * sizeof(SeamVertCpuLit)));
				} else {
					_rigidCpuScratchCap = 0;
				}
			}
			if (_rigidCpuScratch) {
				pspGuLoadMatrix4(GU_MODEL, model);
				const uint32 wf = (rb.boneCount > 1) ? rb.boneCount : 0;
				for (uint32 vi = 0; vi < rb.vertexCount; ++vi) {
					const float *sv = rb.verts + vi * rb.floatStride;
					uint8 activeSlot = 0;
					for (uint32 s = 0; s < wf; ++s) {
						if (sv[s] > 0.5f) {
							activeSlot = (uint8)s;
							break;
						}
					}
					const uint8 boneIdx = rb.bones[activeSlot];
					const BoneNode *bn = bones[boneIdx];
					Math::Vector3d mp(sv[wf + 5], sv[wf + 6], sv[wf + 7]);
					Math::Vector3d mn(sv[wf + 2], sv[wf + 3], sv[wf + 4]);
					bn->_animRot.transform(mp);
					mp += bn->_animPos;
					bn->_animRot.transform(mn);

					SeamVertCpuLit &dv = _rigidCpuScratch[vi];
					dv.u = sv[wf + 0];
					dv.v = sv[wf + 1];
					dv.x = mp.x(); dv.y = mp.y(); dv.z = mp.z();
					dv.color = pspGuEvaluateLightsCpu(modelView, normalRot, mp, mn, baseRgb, lights);
				}
				pspGuWritebackCache64(_rigidCpuScratch, rb.vertexCount * sizeof(SeamVertCpuLit));
				sceGumDrawArray(GU_TRIANGLES,
				                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				                (int)rb.indexCount, rb.indices, _rigidCpuScratch);
			}
		}
	}

	// 2. Blended 2-bone seam batches (already in model space, transformed & rasterized on GE)
	if (!_seamBatches.empty()) {
		pspGuLoadMatrix4(GU_MODEL, model);
		for (uint32 bi = 0; bi < _seamBatches.size(); ++bi) {
			const SeamBatch &sb = _seamBatches[bi];
			Math::Vector3d baseRgb;
			bindMaterialAndLights(sb.materialId, baseRgb);

			if (hwLights) {
				sceGumDrawArray(GU_TRIANGLES,
				                GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				                (int)sb.indexCount, sb.indices, sb.dynHwVerts);
			} else {
				for (uint32 vi = 0; vi < sb.vertexCount; ++vi) {
					const SeamVertex &sv = sb.srcVerts[vi];
					const BoneNode *bn1 = bones[sv.bone1];
					const BoneNode *bn2 = bones[sv.bone2];

					Math::Vector3d p1(sv.pos1[0], sv.pos1[1], sv.pos1[2]);
					Math::Vector3d p2(sv.pos2[0], sv.pos2[1], sv.pos2[2]);
					bn1->_animRot.transform(p1);
					p1 += bn1->_animPos;
					bn2->_animRot.transform(p2);
					p2 += bn2->_animPos;
					const Math::Vector3d mp = Math::Vector3d::interpolate(p2, p1, sv.weight);

					const Math::Vector3d rawN(sv.normal[0], sv.normal[1], sv.normal[2]);
					Math::Vector3d n1 = rawN;
					Math::Vector3d n2 = rawN;
					bn1->_animRot.transform(n1);
					bn2->_animRot.transform(n2);
					const Math::Vector3d mn = Math::Vector3d::interpolate(n2, n1, sv.weight).getNormalized();

					SeamVertCpuLit &dv = sb.dynCpuVerts[vi];
					dv.u = sv.u; dv.v = sv.v;
					dv.x = mp.x(); dv.y = mp.y(); dv.z = mp.z();
					dv.color = pspGuEvaluateLightsCpu(modelView, normalRot, mp, mn, baseRgb, lights);
				}
				pspGuWritebackCache64(sb.dynCpuVerts, sb.vertexCount * sizeof(SeamVertCpuLit));
				sceGumDrawArray(GU_TRIANGLES,
				                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				                (int)sb.indexCount, sb.indices, sb.dynCpuVerts);
			}
		}
	}

	sceGuDisable(GU_LIGHTING);
	pspGuDisableAllLights();

	if (_castsShadow &&
	    StarkScene->shouldRenderShadows() &&
	    StarkSettings->getBoolSetting(Settings::kShadow)) {
		renderShadowPass(model, position, lights, bones);
	}

	_lastDrawnFrameSeq = _gfx->getFrameSeq();
}

Math::Vector3d PspGuActorRenderer::computeShadowLightDirection(const Common::Array<Gfx::LightEntry *> &lights,
                                                               const Math::Vector3d &actorPosition,
                                                               const Math::Matrix3 &worldToModelRot) const {
	Math::Vector3d sumDirection;
	bool hasLight = false;

	auto addPointContrib = [&](const LightEntry *light, float weight) -> bool {
		const float distance = light->position.getDistanceTo(actorPosition);
		if (distance > light->falloffFar) {
			return false;
		}
		float factor = 1.0f;
		if (distance > light->falloffNear) {
			factor = (light->falloffFar - light->falloffNear > 1.0f)
			         ? (1.0f - (distance - light->falloffNear) / (light->falloffFar - light->falloffNear))
			         : 0.0f;
		}
		const float brightness = (light->color.x() + light->color.y() + light->color.z()) / 3.0f;
		if (factor <= 0.0f || brightness <= 0.0f) {
			return false;
		}
		Math::Vector3d dir = actorPosition - light->position;
		dir.normalize();
		sumDirection += dir * (factor * brightness * weight);
		return true;
	};

	for (uint32 i = 1; i < lights.size(); ++i) {
		const LightEntry *light = lights[i];
		if (!light) {
			continue;
		}
		bool contributes = false;
		switch (light->type) {
		case LightEntry::kPoint:
			contributes = addPointContrib(light, 1.0f);
			break;
		case LightEntry::kDirectional: {
			const float brightness = (light->color.x() + light->color.y() + light->color.z()) / 3.0f;
			if (brightness > 0.0f) {
				Math::Vector3d dir = light->direction;
				dir.normalize();
				sumDirection += dir * brightness;
				contributes = true;
			}
			break;
		}
		case LightEntry::kSpot: {
			Math::Vector3d lightToActor = actorPosition - light->position;
			lightToActor.normalize();
			const float cosAngle = MAX(0.0f, lightToActor.dotProduct(light->direction));
			const float cone = CLIP((cosAngle - light->innerConeAngle.getCosine()) /
			                        MAX(0.001f, light->outerConeAngle.getCosine() - light->innerConeAngle.getCosine()),
			                        0.0f, 1.0f);
			if (cone > 0.0f) {
				contributes = addPointContrib(light, cone);
			}
			break;
		}
		case LightEntry::kAmbient:
		default:
			break;
		}
		if (contributes) {
			hasLight = true;
		}
	}

	if (hasLight) {
		Math::Vector2d horiz(sumDirection.x(), sumDirection.y());
		const float shadowLen = MIN(horiz.getMagnitude(), StarkScene->getMaxShadowLength());
		horiz.normalize();
		horiz *= shadowLen;
		sumDirection.x() = horiz.getX();
		sumDirection.y() = horiz.getY();
		sumDirection.z() = -1.0f;
	} else {
		sumDirection.x() = 0.0f;
		sumDirection.y() = 0.0f;
		sumDirection.z() = -1.0f;
	}

	return worldToModelRot * sumDirection;
}

namespace {

struct ShadowVert {
	uint32 color;
	float x, y, z;
};

template <typename PosFn>
void projectAndDrawShadowBatch(PspGuSession *session, const Math::Vector3d &lightDir, float safeDirY,
                               uint32 vertexCount, uint32 indexCount, const uint16 *indices, PosFn getModelPos) {
	if (vertexCount == 0 || indexCount == 0 || !indices || !session->hasDisplayListHeadroom(128)) {
		return;
	}
	ShadowVert *sv = static_cast<ShadowVert *>(session->allocVertices(vertexCount * sizeof(ShadowVert), 16));
	if (!sv) {
		return;
	}
	for (uint32 vi = 0; vi < vertexCount; ++vi) {
		const Math::Vector3d mp = getModelPos(vi);
		const float t = -mp.y() / safeDirY;
		const Math::Vector3d sp = mp + lightDir * t;
		sv[vi].color = 0x80000000u;
		sv[vi].x = sp.x();
		sv[vi].y = 0.0f;
		sv[vi].z = sp.z();
	}
	session->flushDataRange(sv, vertexCount * sizeof(ShadowVert));
	sceGumDrawArray(GU_TRIANGLES,
	                GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
	                (int)indexCount, indices, sv);
}

} // namespace

void PspGuActorRenderer::renderShadowPass(const Math::Matrix4 &model, const Math::Vector3d &position,
                                          const Common::Array<Gfx::LightEntry *> &lights,
                                          const Common::Array<BoneNode *> &bones) const {
	PspGuSession *session = _gfx->getSession();
	if (!session || (_rigidBatches.empty() && _seamBatches.empty())) {
		return;
	}

	Math::Matrix4 modelInverse = model;
	modelInverse.inverse();
	const Math::Vector3d lightDir = computeShadowLightDirection(lights, position, modelInverse.getRotation());
	const float safeDirY = (fabsf(lightDir.y()) > 1e-4f) ? lightDir.y() : -1.0f;

	pspGuLoadMatrix4(GU_MODEL, model);
	sceGuDisable(GU_LIGHTING);
	pspGuDisableAllLights();
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuEnable(GU_STENCIL_TEST);
	sceGuStencilFunc(GU_EQUAL, 0, 0xFF);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
	sceGuPixelMask(0x00000000u);

	for (uint32 bi = 0; bi < _rigidBatches.size(); ++bi) {
		const RigidBatch &rb = _rigidBatches[bi];
		const uint32 wf = (rb.boneCount > 1) ? rb.boneCount : 0;
		projectAndDrawShadowBatch(session, lightDir, safeDirY, rb.vertexCount, rb.indexCount, rb.indices,
		                          [&](uint32 vi) -> Math::Vector3d {
			const float *sv = rb.verts + vi * rb.floatStride;
			uint8 activeSlot = 0;
			for (uint32 s = 0; s < wf; ++s) {
				if (sv[s] > 0.5f) {
					activeSlot = (uint8)s;
					break;
				}
			}
			const BoneNode *bn = bones[rb.bones[activeSlot]];
			Math::Vector3d mp(sv[wf + 5], sv[wf + 6], sv[wf + 7]);
			bn->_animRot.transform(mp);
			mp += bn->_animPos;
			return mp;
		});
	}

	for (uint32 bi = 0; bi < _seamBatches.size(); ++bi) {
		const SeamBatch &sb = _seamBatches[bi];
		projectAndDrawShadowBatch(session, lightDir, safeDirY, sb.vertexCount, sb.indexCount, sb.indices,
		                          [&](uint32 vi) -> Math::Vector3d {
			const SeamVertex &sv = sb.srcVerts[vi];
			const BoneNode *bn1 = bones[sv.bone1];
			const BoneNode *bn2 = bones[sv.bone2];
			Math::Vector3d p1(sv.pos1[0], sv.pos1[1], sv.pos1[2]);
			Math::Vector3d p2(sv.pos2[0], sv.pos2[1], sv.pos2[2]);
			bn1->_animRot.transform(p1);
			p1 += bn1->_animPos;
			bn2->_animRot.transform(p2);
			p2 += bn2->_animPos;
			return Math::Vector3d::interpolate(p2, p1, sv.weight);
		});
	}

	sceGuPixelMask(0xFF000000u);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDepthMask(GU_FALSE);
}

} // End of namespace Gfx
} // End of namespace Stark
