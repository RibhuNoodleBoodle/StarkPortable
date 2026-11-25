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

#include "engines/stark/gfx/pspguprop.h"
#include "engines/stark/gfx/pspgu.h"
#include "engines/stark/gfx/pspgutexture.h"

#include <malloc.h>
#include <string.h>
#include <pspgu.h>
#include <pspgum.h>

#include "common/debug.h"
#include "engines/stark/formats/biffmesh.h"
#include "engines/stark/scene.h"
#include "engines/stark/services/services.h"

namespace Stark {
namespace Gfx {

namespace {

const uint32 kMaxBatchVertices = 65535;
const uint32 kMaxBatchIndices = 49140; // multiple of 3 (16380 triangles)

} // namespace

PspGuPropRenderer::PspGuPropRenderer(PspGuDriver *gfx) :
		VisualProp(),
		_gfx(gfx),
		_cachedModel(nullptr),
		_cpuLitScratch(nullptr),
		_cpuLitScratchCap(0),
		_lastDrawnFrameSeq(0) {
}

PspGuPropRenderer::~PspGuPropRenderer() {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);
	clearBatches();
	if (_cpuLitScratch) {
		pspGuTrackAuxBytes(-(int32)(_cpuLitScratchCap * sizeof(PropVertexCpuLit)));
		free(_cpuLitScratch);
		_cpuLitScratch = nullptr;
		_cpuLitScratchCap = 0;
	}
}

void PspGuPropRenderer::clearBatches() {
	for (uint32 i = 0; i < _batches.size(); ++i) {
		if (_batches[i].verts) {
			free(_batches[i].verts);
		}
		if (_batches[i].indices) {
			free(_batches[i].indices);
		}
		pspGuTrackAuxBytes(-(int32)_batches[i].allocBytes);
	}
	_batches.clear();
	_cachedModel = nullptr;
}

void PspGuPropRenderer::buildBatches() {
	PspGuDriver::ensureResourceSafeToModify(_lastDrawnFrameSeq);
	clearBatches();

	if (!_model) {
		return;
	}

	const Common::Array<Formats::BiffMesh::Vertex> &srcVerts = _model->getVertices();
	const Common::Array<Face> &faces = _model->getFaces();
	const Common::Array<Material> &materials = _model->getMaterials();
	if (srcVerts.empty() || faces.empty()) {
		_cachedModel = _model;
		return;
	}

	const uint32 srcVertCount = srcVerts.size();
	uint32 *seenGen = static_cast<uint32 *>(calloc(srcVertCount, sizeof(uint32)));
	uint16 *remapIdx = static_cast<uint16 *>(malloc(srcVertCount * sizeof(uint16)));
	if (!seenGen || !remapIdx) {
		free(seenGen);
		free(remapIdx);
		warning("PspGuPropRenderer: OOM allocating remap scratch");
		return;
	}
	uint32 curGen = 1;
	Common::Array<PropVertexHW> localVerts;
	Common::Array<uint16> localIndices;

	for (uint32 fi = 0; fi < faces.size(); ++fi) {
		const Face &face = faces[fi];
		if (face.vertexIndices.size() < 3 || face.materialId >= materials.size()) {
			continue;
		}
		const Material &mat = materials[face.materialId];
		const uint32 totalIndices = face.vertexIndices.size();
		const uint32 triCount = totalIndices / 3;

		localVerts.clear();
		localIndices.clear();
		++curGen;
		if (curGen == 0) {
			memset(seenGen, 0, srcVertCount * sizeof(uint32));
			curGen = 1;
		}

		auto flushBatch = [&]() {
			if (localIndices.empty() || localVerts.empty()) {
				localVerts.clear();
				localIndices.clear();
				++curGen;
				if (curGen == 0) {
					memset(seenGen, 0, srcVertCount * sizeof(uint32));
					curGen = 1;
				}
				return;
			}
			const uint32 vBytes = localVerts.size() * sizeof(PropVertexHW);
			const uint32 iBytes = localIndices.size() * sizeof(uint16);
			PropVertexHW *vMem = static_cast<PropVertexHW *>(memalign(64, vBytes));
			uint16 *iMem = static_cast<uint16 *>(memalign(64, iBytes));
			if (!vMem || !iMem) {
				free(vMem);
				free(iMem);
				warning("PspGuPropRenderer: OOM allocating static batch");
			} else {
				memcpy(vMem, localVerts.begin(), vBytes);
				memcpy(iMem, localIndices.begin(), iBytes);
				pspGuWritebackCache64(vMem, vBytes);
				pspGuWritebackCache64(iMem, iBytes);

				Batch b;
				b.materialId = face.materialId;
				b.verts = vMem;
				b.indices = iMem;
				b.vertexCount = localVerts.size();
				b.indexCount = localIndices.size();
				b.allocBytes = vBytes + iBytes;
				pspGuTrackAuxBytes((int32)b.allocBytes);
				_batches.push_back(b);
			}
			localVerts.clear();
			localIndices.clear();
			++curGen;
			if (curGen == 0) {
				memset(seenGen, 0, srcVertCount * sizeof(uint32));
				curGen = 1;
			}
		};

		for (uint32 ti = 0; ti < triCount; ++ti) {
			const uint32 i0 = face.vertexIndices[ti * 3 + 0];
			const uint32 i1 = face.vertexIndices[ti * 3 + 1];
			const uint32 i2 = face.vertexIndices[ti * 3 + 2];
			if (i0 >= srcVertCount || i1 >= srcVertCount || i2 >= srcVertCount) {
				continue;
			}

			// Count distinct new vertices needed for this triangle before adding
			uint32 newNeeded = 0;
			if (seenGen[i0] != curGen) ++newNeeded;
			if (i1 != i0 && seenGen[i1] != curGen) ++newNeeded;
			if (i2 != i0 && i2 != i1 && seenGen[i2] != curGen) ++newNeeded;

			if (localVerts.size() + newNeeded > kMaxBatchVertices ||
			    localIndices.size() + 3 > kMaxBatchIndices) {
				flushBatch();
			}

			const uint32 triSrc[3] = {i0, i1, i2};
			for (int k = 0; k < 3; ++k) {
				const uint32 srcIdx = triSrc[k];
				uint16 dstIdx;
				if (seenGen[srcIdx] == curGen) {
					dstIdx = remapIdx[srcIdx];
				} else {
					dstIdx = (uint16)localVerts.size();
					seenGen[srcIdx] = curGen;
					remapIdx[srcIdx] = dstIdx;
					const Formats::BiffMesh::Vertex &sv = srcVerts[srcIdx];
					PropVertexHW pv;
					pv.u = mat.doubleSided ? sv.texturePosition.x() : (1.0f - sv.texturePosition.x());
					pv.v = 1.0f - sv.texturePosition.y();
					pv.nx = sv.normal.x();
					pv.ny = sv.normal.y();
					pv.nz = sv.normal.z();
					pv.x = sv.position.x();
					pv.y = sv.position.y();
					pv.z = sv.position.z();
					localVerts.push_back(pv);
				}
				localIndices.push_back(dstIdx);
			}
		}
		flushBatch();
	}

	free(seenGen);
	free(remapIdx);
	_cachedModel = _model;
}

void PspGuPropRenderer::render(const Math::Vector3d &position, float direction, const LightEntryArray &lights) {
	if (!_gfx || !_model) {
		return;
	}
	if (_cachedModel != _model) {
		buildBatches();
	}
	if (_batches.empty()) {
		return;
	}
	if (!_gfx->ensureFrameBegun()) {
		return;
	}

	_gfx->set3DMode();

	const Math::Matrix4 model = getModelMatrix(position, direction);
	const Math::Matrix4 view = StarkScene->getViewMatrix();
	const Math::Matrix4 projection = StarkScene->getProjectionMatrix();

	pspGuLoadMatrix4(GU_PROJECTION, projection);
	pspGuLoadMatrix4(GU_VIEW, view);
	pspGuLoadMatrix4(GU_MODEL, model);

	Math::Matrix4 modelView = view * model;
	Math::Matrix4 normalRot = modelView;
	normalRot.invertAffineOrthonormal();

	const Common::Array<Material> &materials = _model->getMaterials();
	const bool hwLights = pspGuCanUseHardwareLights(lights);

	for (uint32 bi = 0; bi < _batches.size(); ++bi) {
		const Batch &b = _batches[bi];
		const Material &mat = materials[b.materialId];
		const Texture *tex = _texture ? _texture->getTexture(mat.texture) : nullptr;

		if (tex) {
			tex->bind();
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		} else {
			sceGuDisable(GU_TEXTURE_2D);
		}

		const Math::Vector3d baseRgb = tex ? Math::Vector3d(1.0f, 1.0f, 1.0f)
		                                   : Math::Vector3d(mat.r, mat.g, mat.b);

		if (hwLights) {
			pspGuSetupLightingOrCpuMode(lights, baseRgb);
			sceGumDrawArray(GU_TRIANGLES,
			                GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
			                (int)b.indexCount, b.indices, b.verts);
		} else {
			sceGuDisable(GU_LIGHTING);
			pspGuDisableAllLights();
			if (_cpuLitScratchCap < b.vertexCount) {
				if (_cpuLitScratch) {
					pspGuTrackAuxBytes(-(int32)(_cpuLitScratchCap * sizeof(PropVertexCpuLit)));
					free(_cpuLitScratch);
				}
				_cpuLitScratchCap = b.vertexCount;
				_cpuLitScratch = static_cast<PropVertexCpuLit *>(memalign(64, _cpuLitScratchCap * sizeof(PropVertexCpuLit)));
				if (_cpuLitScratch) {
					pspGuTrackAuxBytes((int32)(_cpuLitScratchCap * sizeof(PropVertexCpuLit)));
				} else {
					_cpuLitScratchCap = 0;
				}
			}
			if (_cpuLitScratch) {
				for (uint32 vi = 0; vi < b.vertexCount; ++vi) {
					const PropVertexHW &sv = b.verts[vi];
					PropVertexCpuLit &dv = _cpuLitScratch[vi];
					dv.u = sv.u;
					dv.v = sv.v;
					dv.x = sv.x;
					dv.y = sv.y;
					dv.z = sv.z;
					dv.color = pspGuEvaluateLightsCpu(modelView, normalRot,
					                                  Math::Vector3d(sv.x, sv.y, sv.z),
					                                  Math::Vector3d(sv.nx, sv.ny, sv.nz),
					                                  baseRgb, lights);
				}
				pspGuWritebackCache64(_cpuLitScratch, b.vertexCount * sizeof(PropVertexCpuLit));
				sceGumDrawArray(GU_TRIANGLES,
				                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				                (int)b.indexCount, b.indices, _cpuLitScratch);
			}
		}
	}

	sceGuDisable(GU_LIGHTING);
	pspGuDisableAllLights();
	_lastDrawnFrameSeq = _gfx->getFrameSeq();
}

} // End of namespace Gfx
} // End of namespace Stark
