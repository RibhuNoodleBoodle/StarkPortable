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
#include <stdlib.h>
#include <string.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psputils.h>

#include "backends/platform/psp/psp_gu_session.h"

#include "engines/stark/formats/biffmesh.h"
#include "engines/stark/scene.h"
#include "engines/stark/gfx/color.h"
#include "engines/stark/gfx/pspguactor.h"
#include "engines/stark/gfx/pspgubitmap.h"
#include "engines/stark/gfx/pspgufade.h"
#include "engines/stark/gfx/pspguprop.h"
#include "engines/stark/gfx/pspgusurface.h"
#include "engines/stark/gfx/pspgutexture.h"

#include "common/debug.h"
#include "common/endian.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

namespace {

struct SmokeVertex3D {
	uint32 color;
	float x, y, z;
};

struct ColorVertex2D {
	uint32 color;
	float x, y, z;
};

const char *const kNativeOwner = "stark-pspgu";
const uint32 kPaddingSentinelAbgr = 0xFFFF00FFu;
const uint32 kGeTextureBudgetCeiling = 4 * 1024 * 1024;

PspGuDriver *s_activeDriver = nullptr;
uint32 s_liveBitmapBytes = 0;
uint32 s_peakBitmapBytes = 0;
uint32 s_liveTextureBytes = 0;
uint32 s_peakTextureBytes = 0;
uint32 s_liveAuxBytes = 0;
bool s_warnedBudgetCeiling = false;

void checkBudgetCeiling() {
	const uint32 total = s_liveBitmapBytes + s_liveTextureBytes + s_liveAuxBytes;
	if (!s_warnedBudgetCeiling && total > kGeTextureBudgetCeiling) {
		s_warnedBudgetCeiling = true;
		warning("STARK PSP Step03: GE texture+bitmap RAM %u bytes exceeded 4 MiB ceiling", total);
	}
}

uint32 readPixelAbgrFromSurface(const Graphics::Surface *src, const byte *palette, int32 x, int32 y) {
	if (palette && src->format.bytesPerPixel == 1) {
		const byte idx = *(const byte *)src->getBasePtr(x, y);
		const byte r = palette[idx * 3 + 0];
		const byte g = palette[idx * 3 + 1];
		const byte b = palette[idx * 3 + 2];
		return (0xFFu << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
	}
	uint8 a = 255, r = 0, g = 0, b = 0;
	src->format.colorToARGB(src->getPixel(x, y), a, r, g, b);
	return ((uint32)a << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
}

} // namespace

void pspGuWritebackCache64(const void *ptr, uint32 bytes) {
	if (!ptr || bytes == 0) {
		return;
	}
	const uintptr start = reinterpret_cast<uintptr>(ptr) & ~static_cast<uintptr>(63);
	const uintptr end = (reinterpret_cast<uintptr>(ptr) + bytes + 63) & ~static_cast<uintptr>(63);
	sceKernelDcacheWritebackRange(reinterpret_cast<const void *>(start), static_cast<unsigned int>(end - start));
}

void pspGuToSceMatrix(const Math::Matrix4 &mat, ScePspFMatrix4 &out) {
	// Math::Matrix4 is row-major (column vectors); ScePspFMatrix4 stores 4 columns.
	const float *d = mat.getData();
	out.x.x = d[0]; out.x.y = d[4]; out.x.z = d[8];  out.x.w = d[12];
	out.y.x = d[1]; out.y.y = d[5]; out.y.z = d[9];  out.y.w = d[13];
	out.z.x = d[2]; out.z.y = d[6]; out.z.z = d[10]; out.z.w = d[14];
	out.w.x = d[3]; out.w.y = d[7]; out.w.z = d[11]; out.w.w = d[15];
}

void pspGuLoadMatrix4(int matrixMode, const Math::Matrix4 &mat) {
	alignas(16) ScePspFMatrix4 sceMat;
	pspGuToSceMatrix(mat, sceMat);
	sceGumMatrixMode(matrixMode);
	sceGumLoadMatrix(&sceMat);
}

uint32 pspGuPackAbgrFloats(float r, float g, float b, float a) {
	const uint32 rb = (uint32)(CLIP(r, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint32 gb = (uint32)(CLIP(g, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint32 bb = (uint32)(CLIP(b, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint32 ab = (uint32)(CLIP(a, 0.0f, 1.0f) * 255.0f + 0.5f);
	return (ab << 24) | (bb << 16) | (gb << 8) | rb;
}

bool pspGuCanUseHardwareLights(const LightEntryArray &lights) {
	if (lights.empty()) {
		return false;
	}
	uint32 nonAmbientCount = 0;
	for (uint32 li = 0; li < lights.size(); ++li) {
		const LightEntry *light = lights[li];
		if (!light) {
			return false;
		}
		if (light->color.x() < -0.0001f || light->color.y() < -0.0001f || light->color.z() < -0.0001f) {
			return false;
		}
		if (li == 0) {
			if (light->type != LightEntry::kAmbient) {
				return false;
			}
		} else {
			if (light->type == LightEntry::kAmbient) {
				return false;
			}
			++nonAmbientCount;
			if (nonAmbientCount > 4) {
				return false;
			}
		}
	}
	return true;
}

void pspGuDisableAllLights() {
	sceGuDisable(GU_LIGHT0);
	sceGuDisable(GU_LIGHT1);
	sceGuDisable(GU_LIGHT2);
	sceGuDisable(GU_LIGHT3);
}

bool pspGuSetupLightingOrCpuMode(const LightEntryArray &lights, const Math::Vector3d &matRgb) {
	if (!pspGuCanUseHardwareLights(lights)) {
		sceGuDisable(GU_LIGHTING);
		pspGuDisableAllLights();
		return false;
	}

	pspGuDisableAllLights();
	sceGuEnable(GU_LIGHTING);
	sceGuShadeModel(GU_SMOOTH);
	sceGuSpecular(0.0f);
	sceGuColorMaterial(0);

	const uint32 matAbgr = pspGuPackAbgrFloats(matRgb.x(), matRgb.y(), matRgb.z(), 1.0f);
	sceGuModelColor(0x00000000u, matAbgr, matAbgr, 0x00000000u);

	const LightEntry *amb = lights[0];
	sceGuAmbient(pspGuPackAbgrFloats(amb->color.x(), amb->color.y(), amb->color.z(), 1.0f));

	for (uint32 li = 1; li < lights.size(); ++li) {
		const int slot = (int)(li - 1);
		const LightEntry *light = lights[li];
		const uint32 lCol = pspGuPackAbgrFloats(light->color.x(), light->color.y(), light->color.z(), 1.0f);
		sceGuLightColor(slot, GU_AMBIENT, 0xFF000000u);
		sceGuLightColor(slot, GU_DIFFUSE, lCol);
		sceGuLightColor(slot, GU_SPECULAR, 0xFF000000u);

		switch (light->type) {
		case LightEntry::kDirectional: {
			Math::Vector3d toLight = -light->direction;
			if (toLight.getSquareMagnitude() > 1e-8f) {
				toLight.normalize();
			}
			const ScePspFVector3 lPos = { toLight.x(), toLight.y(), toLight.z() };
			sceGuLight(slot, GU_DIRECTIONAL, GU_DIFFUSE, &lPos);
			sceGuLightAtt(slot, 1.0f, 0.0f, 0.0f);
			sceGuEnable(GU_LIGHT0 + slot);
			break;
		}
		case LightEntry::kPoint: {
			const ScePspFVector3 lPos = { light->position.x(), light->position.y(), light->position.z() };
			sceGuLight(slot, GU_POINTLIGHT, GU_DIFFUSE, &lPos);
			const float halfDist = MAX(1.0f, 0.5f * (MAX(0.0f, light->falloffNear) + MAX(1.0f, light->falloffFar)));
			sceGuLightAtt(slot, 1.0f, 0.0f, 1.0f / (halfDist * halfDist));
			sceGuEnable(GU_LIGHT0 + slot);
			break;
		}
		case LightEntry::kSpot: {
			const ScePspFVector3 lPos = { light->position.x(), light->position.y(), light->position.z() };
			sceGuLight(slot, GU_SPOTLIGHT, GU_DIFFUSE, &lPos);
			const float halfDist = MAX(1.0f, 0.5f * (MAX(0.0f, light->falloffNear) + MAX(1.0f, light->falloffFar)));
			sceGuLightAtt(slot, 1.0f, 0.0f, 1.0f / (halfDist * halfDist));

			Math::Vector3d spotDir = -light->direction;
			if (spotDir.getSquareMagnitude() > 1e-8f) {
				spotDir.normalize();
			}
			const ScePspFVector3 lDir = { spotDir.x(), spotDir.y(), spotDir.z() };
			const float cosOuter = CLIP(light->outerConeAngle.getCosine(), -1.0f, 1.0f);
			const float cosInner = CLIP(light->innerConeAngle.getCosine(), cosOuter, 1.0f);
			const float cosMid = CLIP(0.5f * (cosInner + cosOuter), 0.05f, 0.9995f);
			const float exponent = CLIP(logf(0.5f) / logf(cosMid), 1.0f, 64.0f);
			// sceGuLightSpot writes raw float cutoff directly to GE_CMD_LIGHTCUTOFF without cosf().
			sceGuLightSpot(slot, &lDir, exponent, cosOuter);
			sceGuEnable(GU_LIGHT0 + slot);
			break;
		}
		default:
			break;
		}
	}
	return true;
}

uint32 pspGuEvaluateLightsCpu(const Math::Matrix4 &modelView, const Math::Matrix4 &normalRot,
                              const Math::Vector3d &modelPos, const Math::Vector3d &modelNormal,
                              const Math::Vector3d &baseRgb, const LightEntryArray &lights) {
	if (lights.empty()) {
		return pspGuPackAbgrFloats(baseRgb.x(), baseRgb.y(), baseRgb.z(), 1.0f);
	}

	Math::Vector4d eyePos4;
	eyePos4(0, 0) = modelPos.x();
	eyePos4(1, 0) = modelPos.y();
	eyePos4(2, 0) = modelPos.z();
	eyePos4(3, 0) = 1.0f;
	eyePos4 = modelView * eyePos4;
	const Math::Vector3d modelEyePos(eyePos4(0, 0), eyePos4(1, 0), eyePos4(2, 0));

	// normalRot was inverted from modelView; transposing its 3x3 matches inverse-transpose normal transform.
	Math::Matrix3 normalMat = normalRot.getRotation();
	normalMat.transpose();
	Math::Vector3d modelEyeNormal = normalMat * modelNormal;
	if (modelEyeNormal.getSquareMagnitude() > 1e-8f) {
		modelEyeNormal.normalize();
	}

	Math::Vector3d lightColor(0.0f, 0.0f, 0.0f);
	for (uint32 li = 0; li < lights.size(); ++li) {
		const LightEntry *light = lights[li];
		if (!light) {
			continue;
		}
		switch (light->type) {
		case LightEntry::kAmbient:
			lightColor += light->color;
			break;
		case LightEntry::kPoint: {
			Math::Vector3d vertexToLight = light->eyePosition.getXYZ() - modelEyePos;
			const float dist = vertexToLight.length();
			if (dist > 1e-5f) {
				vertexToLight /= dist;
			}
			const float attn = CLIP((light->falloffFar - dist) / MAX(0.001f, light->falloffFar - light->falloffNear), 0.0f, 1.0f);
			const float incidence = MAX(0.0f, Math::Vector3d::dotProduct(modelEyeNormal, vertexToLight));
			lightColor += light->color * (attn * incidence);
			break;
		}
		case LightEntry::kDirectional: {
			Math::Vector3d toLight = -light->eyeDirection;
			if (toLight.getSquareMagnitude() > 1e-8f) {
				toLight.normalize();
			}
			const float incidence = MAX(0.0f, Math::Vector3d::dotProduct(modelEyeNormal, toLight));
			lightColor += light->color * incidence;
			break;
		}
		case LightEntry::kSpot: {
			Math::Vector3d vertexToLight = light->eyePosition.getXYZ() - modelEyePos;
			const float dist = vertexToLight.length();
			const float attn = CLIP((light->falloffFar - dist) / MAX(0.001f, light->falloffFar - light->falloffNear), 0.0f, 1.0f);
			if (dist > 1e-5f) {
				vertexToLight /= dist;
			}
			const float incidence = MAX(0.0f, Math::Vector3d::dotProduct(modelEyeNormal, vertexToLight));
			Math::Vector3d eyeSpotToLight = -light->eyeDirection;
			if (eyeSpotToLight.getSquareMagnitude() > 1e-8f) {
				eyeSpotToLight.normalize();
			}
			const float cosAngle = MAX(0.0f, Math::Vector3d::dotProduct(vertexToLight, eyeSpotToLight));
			const float cosOuter = light->outerConeAngle.getCosine();
			const float cosInner = light->innerConeAngle.getCosine();
			const float cone = (cosInner > cosOuter)
			                   ? CLIP((cosAngle - cosOuter) / MAX(0.001f, cosInner - cosOuter), 0.0f, 1.0f)
			                   : (cosAngle >= cosOuter ? 1.0f : 0.0f);
			lightColor += light->color * (attn * incidence * cone);
			break;
		}
		default:
			break;
		}
	}

	lightColor.x() = CLIP(lightColor.x(), 0.0f, 1.0f);
	lightColor.y() = CLIP(lightColor.y(), 0.0f, 1.0f);
	lightColor.z() = CLIP(lightColor.z(), 0.0f, 1.0f);
	return pspGuPackAbgrFloats(baseRgb.x() * lightColor.x(),
	                           baseRgb.y() * lightColor.y(),
	                           baseRgb.z() * lightColor.z(), 1.0f);
}

uint32 pspGuNextPow2Clamped(uint32 value, uint32 minPow2, uint32 maxPow2) {
	uint32 p = minPow2;
	while (p < value && p < maxPow2) {
		p <<= 1;
	}
	return p;
}

int pspGuClassifySurfacePsm(const Graphics::Surface *src, const byte *palette) {
	if (!src || src->w <= 0 || src->h <= 0 || !src->getPixels()) {
		return GU_PSM_8888;
	}
	if ((uint32)src->w * (uint32)src->h < 4096) {
		return GU_PSM_8888;
	}
	if (palette && src->format.bytesPerPixel == 1) {
		return GU_PSM_5650;
	}
	if (src->format != Driver::getRGBAPixelFormat()) {
		return (src->format.aBits() == 0) ? GU_PSM_5650 : GU_PSM_8888;
	}

	bool hasZeroAlpha = false;
	for (int32 y = 0; y < src->h; ++y) {
		const uint32 *row = (const uint32 *)src->getBasePtr(0, y);
		for (int32 x = 0; x < src->w; ++x) {
			const uint8 a = (uint8)(row[x] >> 24);
			if (a == 0) {
				hasZeroAlpha = true;
			} else if (a != 255) {
				return GU_PSM_8888;
			}
		}
	}
	return hasZeroAlpha ? GU_PSM_5551 : GU_PSM_5650;
}

namespace {

inline uint16 packDither5650(uint32 abgr, int32 x, int32 y) {
	static const int8 kBayer2x2[2][2] = {{-2, 2}, {4, 0}};
	const int32 d5 = kBayer2x2[y & 1][x & 1];
	const int32 d6 = d5 >> 1;
	const uint32 r = (uint32)CLIP<int32>((int32)(abgr & 0xFFu) + d5, 0, 255) >> 3;
	const uint32 g = (uint32)CLIP<int32>((int32)((abgr >> 8) & 0xFFu) + d6, 0, 255) >> 2;
	const uint32 b = (uint32)CLIP<int32>((int32)((abgr >> 16) & 0xFFu) + d5, 0, 255) >> 3;
	return (uint16)((b << 11) | (g << 5) | r);
}

inline uint16 packDither5551(uint32 abgr, int32 x, int32 y) {
	const uint8 a = (uint8)(abgr >> 24);
	if (a < 128) {
		return 0;
	}
	static const int8 kBayer2x2[2][2] = {{-2, 2}, {4, 0}};
	const int32 d5 = kBayer2x2[y & 1][x & 1];
	const uint32 r = (uint32)CLIP<int32>((int32)(abgr & 0xFFu) + d5, 0, 255) >> 3;
	const uint32 g = (uint32)CLIP<int32>((int32)((abgr >> 8) & 0xFFu) + d5, 0, 255) >> 3;
	const uint32 b = (uint32)CLIP<int32>((int32)((abgr >> 16) & 0xFFu) + d5, 0, 255) >> 3;
	return (uint16)(0x8000u | (b << 10) | (g << 5) | r);
}

} // namespace

void pspGuCopySurfaceRectToTile(const Graphics::Surface *src, const byte *palette,
                                int32 srcX, int32 srcY, int32 copyW, int32 copyH,
                                void *dst, uint32 dstStride, int psm) {
	if (!src || !dst || copyW <= 0 || copyH <= 0) {
		return;
	}
	if (psm == GU_PSM_8888) {
		pspGuCopySurfaceRectToRgba8888(src, palette, srcX, srcY, copyW, copyH,
		                               static_cast<uint32 *>(dst), dstStride);
		return;
	}

	const bool fastRgba = (!palette && src->format == Driver::getRGBAPixelFormat());
	uint16 *dst16Base = static_cast<uint16 *>(dst);
	for (int32 row = 0; row < copyH; ++row) {
		const int32 sy = CLIP<int32>(srcY + row, 0, src->h - 1);
		uint16 *dstRow = dst16Base + row * dstStride;
		const uint32 *src32Row = fastRgba ? (const uint32 *)src->getBasePtr(0, sy) : nullptr;
		for (int32 col = 0; col < copyW; ++col) {
			const int32 sx = CLIP<int32>(srcX + col, 0, src->w - 1);
			const uint32 abgr = src32Row ? src32Row[sx] : readPixelAbgrFromSurface(src, palette, sx, sy);
			dstRow[col] = (psm == GU_PSM_5650) ? packDither5650(abgr, sx, sy) : packDither5551(abgr, sx, sy);
		}
	}
}

void pspGuCopySurfaceRectToRgba8888(const Graphics::Surface *src, const byte *palette,
                                    int32 srcX, int32 srcY, int32 copyW, int32 copyH,
                                    uint32 *dst, uint32 dstStride) {
	if (!src || !dst || copyW <= 0 || copyH <= 0) {
		return;
	}

	const bool fastRgba = (!palette && src->format == Driver::getRGBAPixelFormat());
	const bool fastClut = (palette && src->format.bytesPerPixel == 1);

	for (int32 row = 0; row < copyH; ++row) {
		const int32 sy = CLIP<int32>(srcY + row, 0, src->h - 1);
		uint32 *dstRow = dst + row * dstStride;

		if (fastRgba) {
			if (srcX >= 0 && srcX + copyW <= src->w) {
				memcpy(dstRow, src->getBasePtr(srcX, sy), (size_t)copyW * sizeof(uint32));
			} else {
				for (int32 col = 0; col < copyW; ++col) {
					const int32 sx = CLIP<int32>(srcX + col, 0, src->w - 1);
					dstRow[col] = *(const uint32 *)src->getBasePtr(sx, sy);
				}
			}
		} else if (fastClut) {
			const byte *srcRow = (const byte *)src->getBasePtr(0, sy);
			for (int32 col = 0; col < copyW; ++col) {
				const int32 sx = CLIP<int32>(srcX + col, 0, src->w - 1);
				const byte idx = srcRow[sx];
				const byte r = palette[idx * 3 + 0];
				const byte g = palette[idx * 3 + 1];
				const byte b = palette[idx * 3 + 2];
				dstRow[col] = (0xFFu << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
			}
		} else {
			for (int32 col = 0; col < copyW; ++col) {
				const int32 sx = CLIP<int32>(srcX + col, 0, src->w - 1);
				dstRow[col] = readPixelAbgrFromSurface(src, palette, sx, sy);
			}
		}
	}
}

void pspGuResampleSurfaceToRgba8888(const Graphics::Surface *src, const byte *palette,
                                    uint32 dstW, uint32 dstH, uint32 *dst, uint32 dstStride) {
	if (!src || !dst || dstW == 0 || dstH == 0) {
		return;
	}

	const bool exactBox2x = ((uint32)src->w == dstW * 2 && (uint32)src->h == dstH * 2);
	for (uint32 dy = 0; dy < dstH; ++dy) {
		uint32 *dstRow = dst + dy * dstStride;
		if (exactBox2x) {
			const int32 sy = (int32)(dy * 2);
			for (uint32 dx = 0; dx < dstW; ++dx) {
				const int32 sx = (int32)(dx * 2);
				const uint32 p00 = readPixelAbgrFromSurface(src, palette, sx, sy);
				const uint32 p10 = readPixelAbgrFromSurface(src, palette, sx + 1, sy);
				const uint32 p01 = readPixelAbgrFromSurface(src, palette, sx, sy + 1);
				const uint32 p11 = readPixelAbgrFromSurface(src, palette, sx + 1, sy + 1);
				uint32 packed = 0;
				for (uint32 shift = 0; shift < 32; shift += 8) {
					const uint32 avg = (((p00 >> shift) & 0xFFu) + ((p10 >> shift) & 0xFFu) +
					                    ((p01 >> shift) & 0xFFu) + ((p11 >> shift) & 0xFFu) + 2u) >> 2;
					packed |= (avg << shift);
				}
				dstRow[dx] = packed;
			}
		} else {
			const int32 sy = MIN<int32>(src->h - 1, (int32)((dy * (uint32)src->h) / dstH));
			for (uint32 dx = 0; dx < dstW; ++dx) {
				const int32 sx = MIN<int32>(src->w - 1, (int32)((dx * (uint32)src->w) / dstW));
				dstRow[dx] = readPixelAbgrFromSurface(src, palette, sx, sy);
			}
		}
	}
}

void pspGuTrackBitmapBytes(int32 deltaBytes) {
	if (deltaBytes >= 0) {
		s_liveBitmapBytes += (uint32)deltaBytes;
		if (s_liveBitmapBytes > s_peakBitmapBytes) {
			s_peakBitmapBytes = s_liveBitmapBytes;
		}
	} else {
		const uint32 sub = (uint32)(-deltaBytes);
		s_liveBitmapBytes = (sub <= s_liveBitmapBytes) ? (s_liveBitmapBytes - sub) : 0;
	}
	checkBudgetCeiling();
}

void pspGuTrackTextureBytes(int32 deltaBytes) {
	if (deltaBytes >= 0) {
		s_liveTextureBytes += (uint32)deltaBytes;
		if (s_liveTextureBytes > s_peakTextureBytes) {
			s_peakTextureBytes = s_liveTextureBytes;
		}
	} else {
		const uint32 sub = (uint32)(-deltaBytes);
		s_liveTextureBytes = (sub <= s_liveTextureBytes) ? (s_liveTextureBytes - sub) : 0;
	}
	checkBudgetCeiling();
}

void pspGuTrackAuxBytes(int32 deltaBytes) {
	if (deltaBytes >= 0) {
		s_liveAuxBytes += (uint32)deltaBytes;
	} else {
		const uint32 sub = (uint32)(-deltaBytes);
		s_liveAuxBytes = (sub <= s_liveAuxBytes) ? (s_liveAuxBytes - sub) : 0;
	}
	checkBudgetCeiling();
}

uint32 pspGuGetLiveBitmapBytes() { return s_liveBitmapBytes; }
uint32 pspGuGetPeakBitmapBytes() { return s_peakBitmapBytes; }
uint32 pspGuGetLiveTextureBytes() { return s_liveTextureBytes; }
uint32 pspGuGetPeakTextureBytes() { return s_peakTextureBytes; }
uint32 pspGuGetLiveAuxBytes() { return s_liveAuxBytes; }

void PspGuDriver::ensureResourceSafeToModify(uint32 drawnFrameSeq) {
	if (!s_activeDriver || drawnFrameSeq == 0 || drawnFrameSeq != s_activeDriver->_frameSeq) {
		return;
	}
	PspGuSession *sess = s_activeDriver->_session;
	if (sess && sess->isActive() && sess->isFrameOpen()) {
		sess->flushSceneSegment();
		sess->resumeSceneSegment();
		s_activeDriver->applySceneViewport();
	}
}

PspGuDriver::PspGuDriver() :
		_session(nullptr),
		_sessionReady(false),
		_clearRequested(false),
		_reverseDepth(true),
		_frameSeq(1),
		_unimplementedMask(0) {
}

PspGuDriver::~PspGuDriver() {
	if (s_activeDriver == this) {
		s_activeDriver = nullptr;
	}
	if (_session && _sessionReady) {
		const PspGuSessionStats &st = _session->getStats();
		debug("STARK PSP shutdown: frames=%u sceneSegs=%u lcdPresents=%u starts=%u finishes=%u swaps=%u "
		      "peakList=%u peakVerts=%u liveBmp=%u(peak=%u) liveTex=%u(peak=%u) liveAux=%u uord=%u",
		      _frameSeq - 1, st.sceneSegments, st.lcdPresents,
		      st.starts, st.finishes, st.swaps, st.peakListBytes, st.peakVertexBytes,
		      s_liveBitmapBytes, s_peakBitmapBytes,
		      s_liveTextureBytes, s_peakTextureBytes,
		      s_liveAuxBytes, (uint32)mallinfo().uordblks);
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
	sceGuViewport(vpCenterX, vpCenterY, rw, rh);
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
	const float denomW = (float)(_unscaledViewport.width() > 0 ? _unscaledViewport.width() : kOriginalWidth);
	const float denomH = (float)(_unscaledViewport.height() > 0 ? _unscaledViewport.height() : kOriginalHeight);
	const float scaleX = (float)_rtViewport.width() / denomW;
	const float scaleY = (float)_rtViewport.height() / denomH;
	outX = (float)_rtViewport.left + lx * scaleX;
	outY = (float)_rtViewport.top + ly * scaleY;
}

void PspGuDriver::mapLogical2DRectToRt(int32 destX, int32 destY, uint32 rectW, uint32 rectH,
                                       bool noScalingOverride, bool snapToGrid,
                                       float &outX0, float &outY0, float &outX1, float &outY1) const {
	const float origDenomW = (float)(_unscaledViewport.width() > 0 ? _unscaledViewport.width() : kOriginalWidth);
	const float origDenomH = (float)(_unscaledViewport.height() > 0 ? _unscaledViewport.height() : kOriginalHeight);
	const float curDenomW = (float)(_viewport.width() > 0 ? _viewport.width() : kOriginalWidth);
	const float curDenomH = (float)(_viewport.height() > 0 ? _viewport.height() : kOriginalHeight);

	const float scaleX = (float)_rtViewport.width() / origDenomW;
	const float scaleY = (float)_rtViewport.height() / origDenomH;
	const float sizeScaleX = noScalingOverride ? ((float)_rtViewport.width() / curDenomW) : scaleX;
	const float sizeScaleY = noScalingOverride ? ((float)_rtViewport.height() / curDenomH) : scaleY;

	outX0 = (float)_rtViewport.left + (float)destX * scaleX;
	outY0 = (float)_rtViewport.top + (float)destY * scaleY;
	if (snapToGrid) {
		outX0 = floorf(outX0 + 0.5f);
		outY0 = floorf(outY0 + 0.5f);
	}
	outX1 = outX0 + (float)rectW * sizeScaleX;
	outY1 = outY0 + (float)rectH * sizeScaleY;
}

bool PspGuDriver::verifyStep03SurfaceAndTextureContracts(uint32 *readbackBuf, uint32 readbackStride) {
	if (!readbackBuf) {
		return false;
	}

	bool tileSeamPass = false;
	bool premulFadePass = false;
	bool texMipPass = false;

	const uint32 liveBmpBefore = s_liveBitmapBytes;
	const uint32 liveTexBefore = s_liveTextureBytes;

	// 1. 640x365 large bitmap tiling across x=510 and y=255 seams + padding isolation
	{
		Graphics::Surface bigSurf;
		bigSurf.create(640, 365, getRGBAPixelFormat());
		if (bigSurf.getPixels()) {
			for (int y = 0; y < 365; ++y) {
				uint32 *row = (uint32 *)bigSurf.getBasePtr(0, y);
				for (int x = 0; x < 640; ++x) {
					const uint8 r = (x >= 500 && x <= 520) ? 200 : 40;
					const uint8 g = (y >= 245 && y <= 265) ? 220 : 80;
					const uint8 b = (uint8)((x + y) & 0xFF);
					row[x] = (0xFFu << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
				}
			}
			PspGuBitmap *bmp = new PspGuBitmap(&bigSurf);
			PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
			setScreenViewport(false);
			if (ensureFrameBegun()) {
				sceGuClearColor(0xFF000000u);
				sceGuClear(GU_COLOR_BUFFER_BIT);
				sr->render(bmp, Common::Point(0, 0));
				if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
					// Seam sits around rtX = 255, rtY = 127.5 -> should see R>=195, G>=215 seamlessly (no black drop!)
					const uint32 seamPx = readbackBuf[127 * readbackStride + 255];
					const uint8 srR = (uint8)(seamPx & 0xFF);
					const uint8 srG = (uint8)((seamPx >> 8) & 0xFF);
					volatile uint32 *fb = static_cast<volatile uint32 *>(_session->getSceneColorUncached());
					const uint32 padRight = fb ? fb[100 * PspGuSession::kSceneColorStride + 320] : kPaddingSentinelAbgr;
					tileSeamPass = (bmp->getTileCount() == 4) && (srR >= 190) && (srG >= 210) && (padRight == kPaddingSentinelAbgr);
				}
			}
			delete sr;
			delete bmp;
			bigSurf.free();
		}
	}

	// 2. Premultiplied blend + positive fade (+0.5) + negative fade (-0.3) + fill() + FadeRenderer
	{
		Graphics::Surface spr;
		spr.create(64, 64, getRGBAPixelFormat());
		if (spr.getPixels()) {
			for (int y = 0; y < 64; ++y) {
				uint32 *row = (uint32 *)spr.getBasePtr(0, y);
				for (int x = 0; x < 64; ++x) {
					if (x < 32) {
						// Premultiplied (100, 100, 100, 200)
						row[x] = (200u << 24) | (100u << 16) | (100u << 8) | 100u;
					} else {
						// Fully transparent (0, 0, 0, 0)
						row[x] = 0u;
					}
				}
			}
			PspGuBitmap *bmp = new PspGuBitmap(&spr);
			PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
			PspGuFadeRenderer *fade = new PspGuFadeRenderer(this);
			setScreenViewport(false);
			if (ensureFrameBegun()) {
				sceGuClearColor(0xFF000000u);
				sceGuClear(GU_COLOR_BUFFER_BIT);
				// Background solid (40, 80, 120, 255)
				sr->fill(Color(40, 80, 120, 255), Common::Point(0, 0), 320, 240);
				// Normal blend at x=0: dst*(1 - 200/255) + 100 ~= (109, 117, 126)
				sr->setFadeLevel(0.0f);
				sr->render(bmp, Common::Point(0, 0));
				// Positive fade (+0.5f) at x=128 over black
				sr->fill(Color(0, 0, 0, 255), Common::Point(128, 0), 64, 64);
				sr->setFadeLevel(0.5f);
				sr->render(bmp, Common::Point(128, 0));
				// Negative fade (-0.3f) at x=256 over black -> 100 - 0.3*200 = 40
				sr->setFadeLevel(0.0f);
				sr->fill(Color(0, 0, 0, 255), Common::Point(256, 0), 64, 64);
				sr->setFadeLevel(-0.3f);
				sr->render(bmp, Common::Point(256, 0));
				// Screen fade at bottom strip using fill(120, 120, 120, 255) -> fade(0.5f) ~= 60
				sr->setFadeLevel(0.0f);
				sr->fill(Color(120, 120, 120, 255), Common::Point(0, 300), 640, 180);
				fade->render(0.5f);

				if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
					const uint32 blendOpaque = readbackBuf[10 * readbackStride + 8];
					const int rNorm = (int)(blendOpaque & 0xFF);
					const int rPos = (int)(readbackBuf[10 * readbackStride + 72] & 0xFF);
					const int rNeg = (int)(readbackBuf[10 * readbackStride + 136] & 0xFF);
					const uint32 transPos = readbackBuf[10 * readbackStride + 88] & 0x00FFFFFFu;
					const int rScrFade = (int)(readbackBuf[200 * readbackStride + 100] & 0xFF);
					premulFadePass = (abs(rNorm - 54) <= 6) && (rPos >= 92 && rPos <= 108) &&
					                 (rNeg >= 14 && rNeg <= 26) && (transPos == 0u) &&
					                 (rScrFade >= 54 && rScrFade <= 66);
					if (!premulFadePass) {
						warning("STARK PSP Step03 premulFade detail: rNorm=%d rPos=%d rNeg=%d trans=0x%06x rScrFade=%d",
						        rNorm, rPos, rNeg, transPos, rScrFade);
					}
				}
			}
			delete fade;
			delete sr;
			delete bmp;
			spr.free();
		}
	}

	// 3. Oversized 1024x512 CLUT8 Texture with >8 mips clamped cleanly to 512x512 & <= 8 levels, zero leak
	{
		PspGuTexture *tex = new PspGuTexture();
		tex->setLevelCount(11);
		byte palette[256 * 3];
		for (uint32 i = 0; i < 256; ++i) {
			palette[i * 3 + 0] = (byte)i;
			palette[i * 3 + 1] = (byte)(255 - i);
			palette[i * 3 + 2] = (byte)((i * 3) & 0xFF);
		}
		Graphics::Surface mip0;
		mip0.create(1024, 512, Graphics::PixelFormat::createFormatCLUT8());
		if (mip0.getPixels()) {
			memset(mip0.getPixels(), 128, (size_t)mip0.pitch * mip0.h);
			tex->addLevel(0, &mip0, palette);
			mip0.free();
		}
		Graphics::Surface mip1;
		mip1.create(512, 256, Graphics::PixelFormat::createFormatCLUT8());
		if (mip1.getPixels()) {
			memset(mip1.getPixels(), 64, (size_t)mip1.pitch * mip1.h);
			tex->addLevel(1, &mip1, palette);
			mip1.free();
		}
		tex->bind();
		const bool oversizePass = (tex->getBaseWidth() == 512) && (tex->getBaseHeight() == 512) &&
		                          (tex->getLevelCount() == 2) && !tex->isIndexedT8();
		delete tex;

		PspGuTexture *texT8 = new PspGuTexture();
		texT8->setLevelCount(2);
		Graphics::Surface t0;
		t0.create(128, 128, Graphics::PixelFormat::createFormatCLUT8());
		if (t0.getPixels()) {
			memset(t0.getPixels(), 12, (size_t)t0.pitch * t0.h);
			texT8->addLevel(0, &t0, palette);
			t0.free();
		}
		Graphics::Surface t1;
		t1.create(64, 64, Graphics::PixelFormat::createFormatCLUT8());
		if (t1.getPixels()) {
			memset(t1.getPixels(), 24, (size_t)t1.pitch * t1.h);
			texT8->addLevel(1, &t1, palette);
			t1.free();
		}
		texT8->bind();
		texMipPass = oversizePass && texT8->isIndexedT8() && (texT8->getLevelCount() == 2);
		delete texT8;
	}

	const bool zeroLeakPass = (s_liveBitmapBytes == liveBmpBefore) && (s_liveTextureBytes == liveTexBefore);

	// Clear scene cleanly back to black and submit/wait once before STARK renders frame 0
	if (ensureFrameBegun()) {
		setScreenViewport(false);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_BLEND);
		sceGuClearColor(0xFF000000u);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	}
	_session->submitFrame(false);
	_session->waitFrame();

	debug("[STARK-PSP-STEP03] contracts: tileSeam=%s premulFade=%s texMips=%s zeroLeak=%s liveBmp=%u liveTex=%u liveAux=%u",
	      tileSeamPass ? "OK" : "FAIL",
	      premulFadePass ? "OK" : "FAIL",
	      texMipPass ? "OK" : "FAIL",
	      zeroLeakPass ? "OK" : "FAIL",
	      s_liveBitmapBytes, s_liveTextureBytes, s_liveAuxBytes);

	return tileSeamPass && premulFadePass && texMipPass && zeroLeakPass;
}

bool PspGuDriver::verifyStep04HardwareTLContracts(uint32 *readbackBuf, uint32 readbackStride) {
	const uint32 liveAuxBefore = s_liveAuxBytes;
	bool matParityPass = false;
	bool clipPlanePass = false;
	bool hwLightsPass = false;
	bool cpuOverflowPass = false;
	bool skin2BonePass = false;
	bool propSplitPass = false;

	if (!ensureFrameBegun()) {
		return false;
	}

	// 1. Asymmetric matrix + GU_CLIP_PLANES near-plane clipping + reflected depth ordering
	{
		set3DMode();
		sceGuDisable(GU_TEXTURE_2D);
		sceGuClearColor(0xFF000000u);
		sceGuClearDepth(0);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

		// Perspective projection with asymmetric off-center frustum matching Stark camera (Z-up, +Y look)
		Scene clipScene(this);
		clipScene.initCamera(Math::Vector3d(0.0f, -100.0f, 0.0f),
		                     Math::Vector3d(0.0f, 1.0f, 0.0f),
		                     60.0f, Common::Rect(640, 365), 10.0f, 1000.0f);
		clipScene.scrollCamera(Common::Rect(20, 10, 620, 355));
		const Math::Matrix4 proj = clipScene.getProjectionMatrix();
		const Math::Matrix4 view = clipScene.getViewMatrix();

		// Reflected scale(1, 1, -1) like getModelMatrix()
		Math::Matrix4 model;
		model.setToIdentity();
		model(2, 2) = -1.0f;

		pspGuLoadMatrix4(GU_PROJECTION, proj);
		pspGuLoadMatrix4(GU_VIEW, view);
		pspGuLoadMatrix4(GU_MODEL, model);

		// Camera is at y=-100 looking +Y (near plane y=-90, origin y=0 is 100 units ahead); model flips Z -> -Z:
		// Far green triangle at y=20 (dist=120), near-clipped red triangle straddles near plane (one vertex at y=-98 -> dist=2 < 10).
		struct Vert3DCol {
			uint32 color;
			float x, y, z;
		};
		Vert3DCol *clipTris = static_cast<Vert3DCol *>(_session->allocVertices(6 * sizeof(Vert3DCol), 16));
		if (clipTris) {
			clipTris[0] = { 0xFF00FF00u, -30.0f,  20.0f,  30.0f };
			clipTris[1] = { 0xFF00FF00u,  30.0f,  20.0f,  30.0f };
			clipTris[2] = { 0xFF00FF00u,   0.0f,  20.0f, -30.0f };
			clipTris[3] = { 0xFF0000FFu, -40.0f,   0.0f,  25.0f };
			clipTris[4] = { 0xFF0000FFu,  40.0f,   0.0f,  25.0f };
			clipTris[5] = { 0xFF0000FFu,   0.0f, -98.0f, -30.0f };
			_session->flushDataRange(clipTris, 6 * sizeof(Vert3DCol));
			applyDepthConfig(true);
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, clipTris);
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, clipTris + 3);
		}

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();

		if (readOk) {
			// Scene viewport centers vertically inside 320x240; check top corner untouched + red near-clipped triangle occluding far green.
			const uint32 cornerPx = readbackBuf[10 * readbackStride + 10];
			bool sawRed = false;
			for (int32 py = 90; py <= 150 && !sawRed; py += 4) {
				for (int32 px = 130; px <= 190; px += 4) {
					const uint32 c = readbackBuf[py * readbackStride + px];
					if ((c & 0xFFu) > 200 && ((c >> 8) & 0xFFu) < 30 && ((c >> 16) & 0xFFu) < 30) {
						sawRed = true;
						break;
					}
				}
			}
			clipPlanePass = ((cornerPx & 0x00FFFFFFu) == 0u) && sawRed;

			// Also verify matrix conversion directly places translation in column w (out.w.x/y/z)
			Math::Matrix4 testMat;
			testMat.setToIdentity();
			testMat.setPosition(Math::Vector3d(12.5f, -7.25f, 3.75f));
			ScePspFMatrix4 sceM;
			pspGuToSceMatrix(testMat, sceM);
			matParityPass = (fabsf(sceM.w.x - 12.5f) < 1e-4f) &&
			                (fabsf(sceM.w.y + 7.25f) < 1e-4f) &&
			                (fabsf(sceM.w.z - 3.75f) < 1e-4f) &&
			                (fabsf(sceM.w.w - 1.0f) < 1e-4f) &&
			                sawRed;
		}
	}

	// 2. Hardware GE lights: GU_DIRECTIONAL, GU_POINTLIGHT, GU_SPOTLIGHT (axis & cosf cutoff)
	{
		set3DMode();
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuClearColor(0xFF000000u);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

		Math::Matrix4 idMat;
		idMat.setToIdentity();
		// Simple [-1, 1] NDC view without clipping z=0
		pspGuLoadMatrix4(GU_PROJECTION, idMat);
		pspGuLoadMatrix4(GU_VIEW, idMat);
		pspGuLoadMatrix4(GU_MODEL, idMat);

		struct LitQuadVert {
			float nx, ny, nz;
			float x, y, z;
		};

		auto makeLight = [](LightEntry::Type t, const Math::Vector3d &col) -> LightEntry {
			LightEntry le;
			le.type = t;
			le.color = col;
			le.position = Math::Vector3d(0.0f, 0.0f, 0.0f);
			le.direction = Math::Vector3d(0.0f, 0.0f, -1.0f);
			le.innerConeAngle = Math::Angle(15.0f);
			le.outerConeAngle = Math::Angle(30.0f);
			le.falloffNear = 0.5f;
			le.falloffFar = 10.0f;
			le.worldPosition = Math::Vector4d(0.0f, 0.0f, 0.0f, 1.0f);
			le.eyePosition = Math::Vector4d(0.0f, 0.0f, 0.0f, 1.0f);
			le.eyeDirection = Math::Vector3d(0.0f, 0.0f, -1.0f);
			return le;
		};

		LightEntry amb = makeLight(LightEntry::kAmbient, Math::Vector3d(0.0f, 0.0f, 0.0f));

		// Test A (left): GU_DIRECTIONAL pointing -Z -> surface normal +Z -> pure green
		LightEntry dirL = makeLight(LightEntry::kDirectional, Math::Vector3d(0.0f, 1.0f, 0.0f));
		dirL.direction = Math::Vector3d(0.0f, 0.0f, -1.0f);
		LightEntryArray dirArr;
		dirArr.push_back(&amb);
		dirArr.push_back(&dirL);
		LitQuadVert *qDir = static_cast<LitQuadVert *>(_session->allocVertices(16 * sizeof(LitQuadVert), 16));
		if (qDir) {
			LitQuadVert *qPt = qDir + 4;
			LitQuadVert *qSpotIn = qDir + 8;
			LitQuadVert *qSpotOut = qDir + 12;

			qDir[0] = { 0, 0, 1, -0.90f, -0.4f, 0.0f };
			qDir[1] = { 0, 0, 1, -0.50f, -0.4f, 0.0f };
			qDir[2] = { 0, 0, 1, -0.90f,  0.4f, 0.0f };
			qDir[3] = { 0, 0, 1, -0.50f,  0.4f, 0.0f };

			qPt[0] = { 0, 0, 1, -0.40f, -0.4f, 0.0f };
			qPt[1] = { 0, 0, 1,  0.00f, -0.4f, 0.0f };
			qPt[2] = { 0, 0, 1, -0.40f,  0.4f, 0.0f };
			qPt[3] = { 0, 0, 1,  0.00f,  0.4f, 0.0f };

			qSpotIn[0] = { 0, 0, 1, 0.25f, -0.06f, 0.0f };
			qSpotIn[1] = { 0, 0, 1, 0.35f, -0.06f, 0.0f };
			qSpotIn[2] = { 0, 0, 1, 0.25f,  0.06f, 0.0f };
			qSpotIn[3] = { 0, 0, 1, 0.35f,  0.06f, 0.0f };

			qSpotOut[0] = { 0, 0, 1, 0.70f, -0.4f, 0.0f };
			qSpotOut[1] = { 0, 0, 1, 0.95f, -0.4f, 0.0f };
			qSpotOut[2] = { 0, 0, 1, 0.70f,  0.4f, 0.0f };
			qSpotOut[3] = { 0, 0, 1, 0.95f,  0.4f, 0.0f };

			_session->flushDataRange(qDir, 16 * sizeof(LitQuadVert));

			pspGuSetupLightingOrCpuMode(dirArr, Math::Vector3d(1.0f, 1.0f, 1.0f));
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, qDir);

			// Test B (middle-left): GU_POINTLIGHT at (-0.2, 0, +0.5) -> pure red
			LightEntry ptL = makeLight(LightEntry::kPoint, Math::Vector3d(1.0f, 0.0f, 0.0f));
			ptL.position = Math::Vector3d(-0.20f, 0.0f, 0.5f);
			ptL.worldPosition = Math::Vector4d(-0.20f, 0.0f, 0.5f, 1.0f);
			ptL.falloffNear = 0.2f;
			ptL.falloffFar = 4.0f;
			LightEntryArray ptArr;
			ptArr.push_back(&amb);
			ptArr.push_back(&ptL);
			pspGuSetupLightingOrCpuMode(ptArr, Math::Vector3d(1.0f, 1.0f, 1.0f));
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, qPt);

			// Test C (right): GU_SPOTLIGHT at (0.30, 0, +0.4) pointing -Z, outer=25 deg
			LightEntry spL = makeLight(LightEntry::kSpot, Math::Vector3d(0.0f, 0.0f, 1.0f));
			spL.position = Math::Vector3d(0.30f, 0.0f, 0.4f);
			spL.worldPosition = Math::Vector4d(0.30f, 0.0f, 0.4f, 1.0f);
			spL.direction = Math::Vector3d(0.0f, 0.0f, -1.0f);
			spL.innerConeAngle = Math::Angle(10.0f);
			spL.outerConeAngle = Math::Angle(25.0f);
			spL.falloffNear = 0.2f;
			spL.falloffFar = 5.0f;
			LightEntryArray spArr;
			spArr.push_back(&amb);
			spArr.push_back(&spL);
			pspGuSetupLightingOrCpuMode(spArr, Math::Vector3d(1.0f, 1.0f, 1.0f));
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, qSpotIn);
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, qSpotOut);
		}

		sceGuDisable(GU_LIGHTING);
		pspGuDisableAllLights();

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();

		if (readOk) {
			// Sample center of each quad inside the RT viewport
			const int32 cy = _rtViewport.top + _rtViewport.height() / 2;
			const auto sampleX = [&](float ndcX) -> uint32 {
				const int32 px = _rtViewport.left + (int32)((ndcX * 0.5f + 0.5f) * (float)_rtViewport.width());
				return readbackBuf[cy * readbackStride + CLIP<int32>(px, 0, 319)];
			};
			const uint32 pxDir = sampleX(-0.70f);
			const uint32 pxPt = sampleX(-0.20f);
			const uint32 pxSpIn = sampleX(0.30f);
			const uint32 pxSpOut = sampleX(0.82f);
			const uint8 dirG = (uint8)((pxDir >> 8) & 0xFFu);
			const uint8 ptR = (uint8)(pxPt & 0xFFu);
			const uint8 spInB = (uint8)((pxSpIn >> 16) & 0xFFu);
			const uint8 spOutB = (uint8)((pxSpOut >> 16) & 0xFFu);
			hwLightsPass = (dirG > 200) && (ptR > 160) && (spInB > 160) && (spOutB < 30);
			if (!hwLightsPass) {
				debug("[STARK-PSP-STEP04] hwLight probe: dirG=%u ptR=%u spotInB=%u spotOutB=%u",
				      dirG, ptR, spInB, spOutB);
			}
		}
	}

	// 3. >4 light CPU overflow + negative x_neg light fallback + 2-bone sceGuBoneMatrix skinning
	{
		set3DMode();
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDisable(GU_LIGHTING);
		pspGuDisableAllLights();
		sceGuClearColor(0xFF000000u);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

		Math::Matrix4 idMat;
		idMat.setToIdentity();
		pspGuLoadMatrix4(GU_PROJECTION, idMat);
		pspGuLoadMatrix4(GU_VIEW, idMat);
		pspGuLoadMatrix4(GU_MODEL, idMat);

		LightEntry amb;
		amb.type = LightEntry::kAmbient;
		amb.color = Math::Vector3d(0.0f, 0.0f, 0.0f);
		LightEntry dOff;
		dOff.type = LightEntry::kDirectional;
		dOff.color = Math::Vector3d(0.0f, 0.0f, 0.0f);
		dOff.direction = Math::Vector3d(0.0f, 0.0f, -1.0f);
		dOff.eyeDirection = dOff.direction;
		LightEntry d5 = dOff;
		d5.color = Math::Vector3d(0.0f, 1.0f, 1.0f); // 5th light: cyan (would be lost if truncated to 4)

		LightEntryArray overflowArr;
		overflowArr.push_back(&amb);
		overflowArr.push_back(&dOff);
		overflowArr.push_back(&dOff);
		overflowArr.push_back(&dOff);
		overflowArr.push_back(&dOff);
		overflowArr.push_back(&d5);

		LightEntry dNeg = dOff;
		dNeg.color = Math::Vector3d(-0.6f, 0.0f, 0.0f);
		LightEntry ambWhite = amb;
		ambWhite.color = Math::Vector3d(1.0f, 0.5f, 0.0f);
		LightEntryArray negArr;
		negArr.push_back(&ambWhite);
		negArr.push_back(&dNeg);

		const bool rejectOverflow = !pspGuCanUseHardwareLights(overflowArr);
		const bool rejectNeg = !pspGuCanUseHardwareLights(negArr);
		const uint32 cOverflow = pspGuEvaluateLightsCpu(idMat, idMat,
		                                                Math::Vector3d(0, 0, 0), Math::Vector3d(0, 0, 1),
		                                                Math::Vector3d(1.0f, 1.0f, 1.0f), overflowArr);
		const uint32 cNeg = pspGuEvaluateLightsCpu(idMat, idMat,
		                                           Math::Vector3d(0, 0, 0), Math::Vector3d(0, 0, 1),
		                                           Math::Vector3d(1.0f, 1.0f, 1.0f), negArr);

		struct VertCpuCol {
			uint32 color;
			float x, y, z;
		};
		VertCpuCol *cpuQuads = static_cast<VertCpuCol *>(_session->allocVertices(8 * sizeof(VertCpuCol), 16));
		if (cpuQuads) {
			cpuQuads[0] = { cOverflow, -0.8f, -0.4f, 0.0f }; cpuQuads[1] = { cOverflow, -0.2f, -0.4f, 0.0f };
			cpuQuads[2] = { cOverflow, -0.8f,  0.4f, 0.0f }; cpuQuads[3] = { cOverflow, -0.2f,  0.4f, 0.0f };
			cpuQuads[4] = { cNeg,       0.2f, -0.4f, 0.0f }; cpuQuads[5] = { cNeg,       0.8f, -0.4f, 0.0f };
			cpuQuads[6] = { cNeg,       0.2f,  0.4f, 0.0f }; cpuQuads[7] = { cNeg,       0.8f,  0.4f, 0.0f };
			_session->flushDataRange(cpuQuads, 8 * sizeof(VertCpuCol));
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, cpuQuads);
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, cpuQuads + 4);
		}

		// 2-bone hardware skinning quad shifted to upper-right by bone1 translation (+0.4, +0.6, 0)
		struct Skin2Vert {
			float w0, w1;
			uint32 color;
			float x, y, z;
		};
		Math::Matrix4 bone0, bone1;
		bone0.setToIdentity();
		bone1.setToIdentity();
		bone1.setPosition(Math::Vector3d(0.4f, 0.6f, 0.0f));
		alignas(16) ScePspFMatrix4 sceB0, sceB1;
		pspGuToSceMatrix(bone0, sceB0);
		pspGuToSceMatrix(bone1, sceB1);
		sceGuBoneMatrix(0, &sceB0);
		sceGuBoneMatrix(1, &sceB1);

		Skin2Vert *skinQuad = static_cast<Skin2Vert *>(_session->allocVertices(4 * sizeof(Skin2Vert), 16));
		if (skinQuad) {
			skinQuad[0] = { 0.0f, 1.0f, 0xFF00FF00u, -0.2f, -0.2f, 0.0f };
			skinQuad[1] = { 0.0f, 1.0f, 0xFF00FF00u,  0.2f, -0.2f, 0.0f };
			skinQuad[2] = { 0.0f, 1.0f, 0xFF00FF00u, -0.2f,  0.2f, 0.0f };
			skinQuad[3] = { 0.0f, 1.0f, 0xFF00FF00u,  0.2f,  0.2f, 0.0f };
			_session->flushDataRange(skinQuad, 4 * sizeof(Skin2Vert));
			sceGumDrawArray(GU_TRIANGLE_STRIP,
			                GU_WEIGHTS(2) | GU_WEIGHT_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
			                4, nullptr, skinQuad);
		}

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();

		if (readOk) {
			const int32 cy = _rtViewport.top + _rtViewport.height() / 2;
			const int32 xOv = _rtViewport.left + (int32)((-0.50f * 0.5f + 0.5f) * (float)_rtViewport.width());
			const int32 xNg = _rtViewport.left + (int32)(( 0.50f * 0.5f + 0.5f) * (float)_rtViewport.width());
			const uint32 pxOv = readbackBuf[cy * readbackStride + xOv];
			const uint32 pxNg = readbackBuf[cy * readbackStride + xNg];
			const uint8 ovR = (uint8)(pxOv & 0xFFu);
			const uint8 ovG = (uint8)((pxOv >> 8) & 0xFFu);
			const uint8 ovB = (uint8)((pxOv >> 16) & 0xFFu);
			const uint8 ngR = (uint8)(pxNg & 0xFFu);
			const uint8 ngG = (uint8)((pxNg >> 8) & 0xFFu);
			cpuOverflowPass = rejectOverflow && rejectNeg &&
			                  (ovR < 20 && ovG > 220 && ovB > 220) &&
			                  (ngR >= 85 && ngR <= 120 && ngG >= 115 && ngG <= 145);

			const int32 skY = _rtViewport.top + (int32)((1.0f - (0.60f * 0.5f + 0.5f)) * (float)_rtViewport.height());
			const int32 skX = _rtViewport.left + (int32)(( 0.40f * 0.5f + 0.5f) * (float)_rtViewport.width());
			const uint32 pxSk = readbackBuf[CLIP<int32>(skY, 0, 239) * readbackStride + CLIP<int32>(skX, 0, 319)];
			skin2BonePass = (((pxSk >> 8) & 0xFFu) > 220) && ((pxSk & 0xFFu) < 20);
			if (!skin2BonePass) {
				int32 minGx = 999, maxGx = -1, minGy = 999, maxGy = -1;
				for (int32 py = 0; py < 240; py += 2) {
					for (int32 px = 0; px < 320; px += 2) {
						const uint32 c = readbackBuf[py * readbackStride + px];
						if (((c >> 8) & 0xFFu) > 220 && (c & 0xFFu) < 20 && ((c >> 16) & 0xFFu) < 20) {
							if (px < minGx) minGx = px;
							if (px > maxGx) maxGx = px;
							if (py < minGy) minGy = py;
							if (py > maxGy) maxGy = py;
						}
					}
				}
				debug("[STARK-PSP-STEP04] skin2Bone probe (%d,%d)=0x%08x greenBbox=[%d..%d, %d..%d]",
				      skX, skY, pxSk, minGx, maxGx, minGy, maxGy);
			}
		}
	}

	// 4. Synthetic >65535-vertex / >49140-index prop split + zero-leak teardown
	{
		const uint32 kBigVerts = 65541;
		Common::Array<Formats::BiffMesh::Vertex> srcVerts;
		srcVerts.resize(kBigVerts);
		for (uint32 vi = 0; vi < kBigVerts; ++vi) {
			srcVerts[vi].position = Math::Vector3d((float)(vi % 16), (float)((vi / 16) % 16), 0.0f);
			srcVerts[vi].normal = Math::Vector3d(0.0f, 0.0f, 1.0f);
			srcVerts[vi].texturePosition = Math::Vector3d(0.0f, 0.0f, 0.0f);
		}
		Face bigFace;
		bigFace.materialId = 0;
		bigFace.vertexIndices.resize(kBigVerts);
		for (uint32 vi = 0; vi < kBigVerts; ++vi) {
			bigFace.vertexIndices[vi] = vi;
		}
		Common::Array<Face> faces;
		faces.push_back(bigFace);
		Material mat = Material();
		mat.r = 1.0f; mat.g = 1.0f; mat.b = 1.0f;
		mat.doubleSided = false;
		Common::Array<Material> mats;
		mats.push_back(mat);

		Formats::BiffMesh *bigMesh = new Formats::BiffMesh(srcVerts, faces, mats);
		PspGuPropRenderer *propR = new PspGuPropRenderer(this);
		propR->setModel(bigMesh); // VisualProp takes ownership of bigMesh
		propR->buildBatches();

		bool allWithinLimit = (propR->_batches.size() >= 2);
		uint32 sumIndices = 0;
		for (uint32 bi = 0; bi < propR->_batches.size(); ++bi) {
			const PspGuPropRenderer::Batch &b = propR->_batches[bi];
			if (b.vertexCount > 65535 || b.indexCount > 49140) {
				allWithinLimit = false;
			}
			sumIndices += b.indexCount;
		}
		propSplitPass = allWithinLimit && (sumIndices == kBigVerts);
		delete propR; // frees batches and bigMesh via ~VisualProp()
	}

	const bool zeroLeakPass = (s_liveAuxBytes == liveAuxBefore);

	// Reset cleanly to black before returning to startup/frame 0
	if (ensureFrameBegun()) {
		setScreenViewport(false);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_BLEND);
		sceGuDisable(GU_LIGHTING);
		sceGuDisable(GU_CLIP_PLANES);
		pspGuDisableAllLights();
		sceGuClearColor(0xFF000000u);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	}
	_session->submitFrame(false);
	_session->waitFrame();

	debug("[STARK-PSP-STEP04] contracts: matParity=%s clipPlane=%s hwLights=%s cpuOverflow=%s skin2Bone=%s propSplit=%s zeroLeak=%s liveAux=%u",
	      matParityPass ? "OK" : "FAIL",
	      clipPlanePass ? "OK" : "FAIL",
	      hwLightsPass ? "OK" : "FAIL",
	      cpuOverflowPass ? "OK" : "FAIL",
	      skin2BonePass ? "OK" : "FAIL",
	      propSplitPass ? "OK" : "FAIL",
	      zeroLeakPass ? "OK" : "FAIL",
	      s_liveAuxBytes);

	return matParityPass && clipPlanePass && hwLightsPass &&
	       cpuOverflowPass && skin2BonePass && propSplitPass && zeroLeakPass;
}

void PspGuDriver::verifyStartupContracts() {
	if (!_session) {
		return;
	}

	// 1. Duplicate acquire reject + transactional failure unwind contracts (Step 02)
	const bool dupOk = _session->acquire("stark-pspgu-duplicate");
	const bool activeAfterDup = _session->isActive();

	_session->release();
	const int usedBeforeVramFail = mallinfo().uordblks;
	_session->setInjectVramFail(true);
	const bool vramFailOk = _session->acquire("stark-pspgu-vram-fail");
	const int usedAfterVramFail = mallinfo().uordblks;

	_session->setInjectAllocFail(true);
	const bool ramFailOk = _session->acquire("stark-pspgu-ram-fail");
	const int usedAfterRamFail = mallinfo().uordblks;
	const bool activeAfterFail = _session->isActive();

	_sessionReady = _session->acquire(kNativeOwner);
	if (!_sessionReady) {
		return;
	}
	_session->onSuspend();
	_session->onResume();
	_session->primeScenePadding(kPaddingSentinelAbgr);

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
		ident.x.x = 1.0f; ident.y.y = 1.0f; ident.z.z = 1.0f; ident.w.w = 1.0f;
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
			_session->flushDataRange(v, 3 * sizeof(SmokeVertex3D));
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, v);
		};

		applyDepthConfig(true);
		sceGuClearColor(0xFF080808);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
		drawNdcTri(0xFF0000FFu, 0.5f, 0.8f);
		drawNdcTri(0xFF00FF00u, -0.5f, 0.8f);
		drawNdcTri(0xFFFF0000u, 0.8f, 0.8f);
		if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
			revDepthPass = (readbackBuf[120 * readbackStride + 160] & 0x00FFFFFFu) == 0x0000FF00u;
		}

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
			drawNdcTri(0xFFFFFF00u, -0.5f, 0.8f);
			drawNdcTri(0xFFFF0000u, 0.8f, 0.8f);
			if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
				fwdDepthPass = (readbackBuf[120 * readbackStride + 160] & 0x00FFFFFFu) == 0x00FFFF00u;
			}
		}

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
			_session->flushDataRange(c, 8 * sizeof(ColorVertex2D));
			sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 8, nullptr, c);
			if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
				const uint32 tl = readbackBuf[0 * readbackStride + 0] & 0x00FFFFFFu;
				const uint32 tr = readbackBuf[0 * readbackStride + 319] & 0x00FFFFFFu;
				const uint32 bl = readbackBuf[239 * readbackStride + 0] & 0x00FFFFFFu;
				const uint32 br = readbackBuf[239 * readbackStride + 319] & 0x00FFFFFFu;
				const uint32 clipCenter = readbackBuf[120 * readbackStride + 160] & 0x00FFFFFFu;
				cornersAndClipPass = (tl == 0x000000FFu && tr == 0x0000FF00u &&
				                      bl == 0x00FF0000u && br == 0x00FFFFFFu &&
				                      clipCenter == 0x0033AA55u);
			}
		}

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
			_session->flushDataRange(preQuad, 2 * sizeof(ColorVertex2D));
			sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, preQuad);

			Graphics::Surface *shot = getViewportScreenshot(160, 91);
			const uint32 swapsAfterShot = _session->getStats().swaps;

			ColorVertex2D *postQuad = static_cast<ColorVertex2D *>(_session->allocVertices(2 * sizeof(ColorVertex2D), 16));
			mapLogical2DToRt(400.0f, 0.0f, postQuad[0].x, postQuad[0].y);
			mapLogical2DToRt(600.0f, 200.0f, postQuad[1].x, postQuad[1].y);
			postQuad[0].color = 0xFFDD4422u; postQuad[0].z = 0.0f;
			postQuad[1].color = 0xFFDD4422u; postQuad[1].z = 0.0f;
			_session->flushDataRange(postQuad, 2 * sizeof(ColorVertex2D));
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

		verifyStep03SurfaceAndTextureContracts(readbackBuf, readbackStride);
		verifyStep04HardwareTLContracts(readbackBuf, readbackStride);
	}
	free(readbackBuf);

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
		const Common::Point projCenter = testScene.convertPosition3DToGameScreenOriginal(Math::Vector3d(0.0f, 0.0f, 150.0f));
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

	s_activeDriver = this;
	_sessionReady = true;
	_computeLights = false;
	computeScreenViewport();
	setScreenViewport(false);

	verifyStartupContracts();
	setScreenViewport(false);

	debug("[STARK-PSP-STEP03] PspGuDriver ready (session=%s, listCap=%u, reserve=%u, vertexArena=%u, logical=%dx%d, rt=%ux%u)",
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

void PspGuDriver::flipBuffer() {
	_clearRequested = false;
	if (!_session || !_sessionReady || !_session->isActive()) {
		return;
	}
	if (!ensureFrameBegun()) {
		return;
	}
	if (!_session->submitFrame(true)) {
		warning("PspGuDriver: submitFrame failed, unwinding cleanly");
		return;
	}
	_session->waitFrame();

	if (_frameSeq <= 5 || (_frameSeq % 240) == 0) {
		const PspGuSessionStats &st = _session->getStats();
		debug("[STARK-PSP-STEP03] frame=%u swaps=%u list=%u/%u verts=%u/%u liveBmp=%u(peak=%u) liveTex=%u(peak=%u) liveAux=%u uord=%u",
		      _frameSeq, st.swaps, st.lastListBytes, st.peakListBytes,
		      st.lastVertexBytes, st.peakVertexBytes,
		      s_liveBitmapBytes, s_peakBitmapBytes,
		      s_liveTextureBytes, s_peakTextureBytes,
		      s_liveAuxBytes, (uint32)mallinfo().uordblks);
	}
	_frameSeq++;
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
		sceGuEnable(GU_CLIP_PLANES);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_BLEND);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_STENCIL_TEST);
		sceGuDisable(GU_LIGHTING);
		pspGuDisableAllLights();
		sceGuShadeModel(GU_SMOOTH);
		sceGuSpecular(0.0f);
		sceGuPixelMask(0x00000000);
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
		warning("STARK PSP Step04: %s is developmentally unavailable (unimplemented until Step 05+)",
		        methodName ? methodName : "unknown");
	}
}

} // End of namespace Gfx
} // End of namespace Stark
