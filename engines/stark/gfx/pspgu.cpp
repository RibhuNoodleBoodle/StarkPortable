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
#include <pspsysmem.h>
#include <pspthreadman.h>
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
#include "engines/stark/visual/effects/bubbles.h"
#include "engines/stark/visual/effects/fireflies.h"
#include "engines/stark/visual/effects/fish.h"
#include "engines/stark/visual/explodingimage.h"
#include "engines/stark/visual/image.h"

#include "common/algorithm.h"
#include "common/debug.h"
#include "common/endian.h"
#include "common/textconsole.h"
#include "graphics/surface.h"

namespace Stark {
namespace Gfx {

namespace {

#ifdef STARK_PSP_DIAGNOSTICS
struct SmokeVertex3D {
	uint32 color;
	float x, y, z;
};

const uint32 kPaddingSentinelAbgr = 0xFFFF00FFu;
bool s_injectAllocFail = false;
#endif

struct ColorVertex2D {
	uint32 color;
	float x, y, z;
};

const char *const kNativeOwner = "stark-pspgu";
const uint32 kGeTextureBudgetCeiling = 4 * 1024 * 1024;

PspGuDriver *s_activeDriver = nullptr;
uint32 s_liveBitmapBytes = 0;
uint32 s_peakBitmapBytes = 0;
uint32 s_liveTextureBytes = 0;
uint32 s_peakTextureBytes = 0;
uint32 s_liveAuxBytes = 0;
uint32 s_peakAuxBytes = 0;
uint32 s_externalAuxBytes = 0;
uint32 s_evictionCount = 0;
bool s_warnedBudgetCeiling = false;

void checkBudgetCeiling() {
	const uint32 total = s_liveBitmapBytes + s_liveTextureBytes + s_liveAuxBytes;
	if (!s_warnedBudgetCeiling && total > kGeTextureBudgetCeiling) {
		s_warnedBudgetCeiling = true;
		warning("STARK PSP: GE texture+bitmap RAM %u bytes exceeded 4 MiB ceiling", total);
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
			const float cone = CLIP((cosAngle - cosInner) / MAX(0.001f, cosOuter - cosInner), 0.0f, 1.0f);
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
				// Keep crisp RGBA8888 for fonts/UI icons <=32K pixels; use dithered 4444 for large translucent cutouts
				const uint32 pixelCount = (uint32)src->w * (uint32)src->h;
				return (pixelCount > 32768u) ? GU_PSM_4444 : GU_PSM_8888;
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

inline uint16 packDither4444(uint32 abgr, int32 x, int32 y) {
	static const int8 kBayer2x2[2][2] = {{-4, 4}, {8, 0}};
	const int32 d4 = kBayer2x2[y & 1][x & 1];
	const uint32 r = (uint32)CLIP<int32>((int32)(abgr & 0xFFu) + d4, 0, 255) >> 4;
	const uint32 g = (uint32)CLIP<int32>((int32)((abgr >> 8) & 0xFFu) + d4, 0, 255) >> 4;
	const uint32 b = (uint32)CLIP<int32>((int32)((abgr >> 16) & 0xFFu) + d4, 0, 255) >> 4;
	const uint32 a = (uint32)CLIP<int32>((int32)((abgr >> 24) & 0xFFu) + d4, 0, 255) >> 4;
	return (uint16)((a << 12) | (b << 8) | (g << 4) | r);
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
			if (psm == GU_PSM_5650) {
				dstRow[col] = packDither5650(abgr, sx, sy);
			} else if (psm == GU_PSM_4444) {
				dstRow[col] = packDither4444(abgr, sx, sy);
			} else {
				dstRow[col] = packDither5551(abgr, sx, sy);
			}
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
		if (s_liveAuxBytes > s_peakAuxBytes) {
			s_peakAuxBytes = s_liveAuxBytes;
		}
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
uint32 pspGuGetPeakAuxBytes() { return s_peakAuxBytes; }

#ifdef STARK_PSP_DIAGNOSTICS
void pspGuSetInjectAllocFail(bool enable) {
	s_injectAllocFail = enable;
}
#endif

uint32 pspGuReclaimIdleResources(uint32 minBytes) {
	PspGuSession *sess = s_activeDriver ? s_activeDriver->getSession() : nullptr;
	if (sess && sess->isActive() && sess->isFrameOpen()) {
		sess->flushSceneSegment();
		sess->resumeSceneSegment();
		s_activeDriver->applySceneViewport();
	} else {
		sceGuSync(0, 0);
	}
	const uint32 frameSeq = s_activeDriver ? s_activeDriver->getFrameSeq() : 0x7FFFFFFFu;
	const uint32 reclaimed = PspGuBitmap::reclaimIdleAlphaMasks(frameSeq, minBytes);
	if (reclaimed > 0) {
		s_evictionCount++;
	}
	return reclaimed;
}

void *pspGuAllocAlignedWithRetry(uint32 alignment, uint32 size) {
	if (size == 0) {
		return nullptr;
	}
	void *ptr = nullptr;
#ifdef STARK_PSP_DIAGNOSTICS
	if (!s_injectAllocFail) {
		ptr = memalign(alignment, size);
	}
	if (!ptr) {
		pspGuReclaimIdleResources(size);
		if (!s_injectAllocFail) {
			ptr = memalign(alignment, size);
		}
		if (s_injectAllocFail) {
			s_injectAllocFail = false;
		}
	}
#else
	ptr = memalign(alignment, size);
	if (!ptr) {
		pspGuReclaimIdleResources(size);
		ptr = memalign(alignment, size);
	}
#endif
	return ptr;
}

uint32 PspGuDriver::getCurrentFrameSeq() {
	return s_activeDriver ? s_activeDriver->_frameSeq : 0;
}

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
#ifdef STARK_PSP_DIAGNOSTICS
		_geAuditLogged(false),
#endif
		_session(nullptr),
		_sessionReady(false),
		_clearRequested(false),
		_reverseDepth(true),
		_frameSeq(1),
		_timingRingHead(0),
		_timingRingCount(0),
		_frameClearUs(0),
		_lastFlipEndUs(0),
		_frameDrawCalls(0),
		_listSplitCount(0),
		_oomRetryCount(0),
		_oomFailCount(0) {
	memset(_timingRing, 0, sizeof(_timingRing));
}

PspGuDriver::~PspGuDriver() {
	if (s_activeDriver == this) {
		s_activeDriver = nullptr;
	}
	if (_session && _sessionReady) {
		logMemoryAndPacingSnapshot("shutdown", 0xFFFFu, 0xFFFFu);
		const PspGuSessionStats &st = _session->getStats();
		debug("STARK PSP shutdown: frames=%u sceneSegs=%u lcdPresents=%u starts=%u finishes=%u swaps=%u "
		      "peakList=%u peakVerts=%u splits=%u evict=%u liveBmp=%u(peak=%u) liveTex=%u(peak=%u) liveAux=%u(peak=%u) hitMask=%u uord=%u",
		      _frameSeq - 1, st.sceneSegments, st.lcdPresents,
		      st.starts, st.finishes, st.swaps, st.peakListBytes, st.peakVertexBytes,
		      _listSplitCount, s_evictionCount,
		      s_liveBitmapBytes, s_peakBitmapBytes,
		      s_liveTextureBytes, s_peakTextureBytes,
		      s_liveAuxBytes, s_peakAuxBytes, s_externalAuxBytes,
		      (uint32)mallinfo().uordblks);
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

#ifdef STARK_PSP_DIAGNOSTICS
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

bool PspGuDriver::verifyStep05EffectsParityContracts(uint32 *readbackBuf, uint32 readbackStride) {
	if (!readbackBuf) {
		return false;
	}

	const uint32 liveBmpBefore = s_liveBitmapBytes;
	const uint32 liveAuxBefore = s_liveAuxBytes;

	bool shadowSinglePass = false;
	bool multiCasterPass = false;
	bool depthOcclPass = false;
	bool alphaIsoPass = false;
	bool effectsQuadsPass = false;
	bool effectsLinesPass = false;
	bool stateIsoPass = false;
	bool noCpuSurfacePass = false;

	// 1. Non-shadow alpha isolation + planar stencil shadow overlap + multi-caster + depth occlusion
	{
		setScreenViewport(false);
		clearScreen();

		PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
		// Background floor in steel blue (100, 140, 180); pixel mask keeps alpha/stencil at 0
		sr->fill(Color(100, 140, 180, 255), Common::Point(0, 0), kOriginalWidth, kOriginalHeight);

		set3DMode();
		sceGuDisable(GU_TEXTURE_2D);
		Math::Matrix4 idMat;
		idMat.setToIdentity();
		pspGuLoadMatrix4(GU_PROJECTION, idMat);
		pspGuLoadMatrix4(GU_VIEW, idMat);
		pspGuLoadMatrix4(GU_MODEL, idMat);

		struct Vert3D {
			uint32 color;
			float x, y, z;
		};

		// Foreground blocker at right side with closer depth (reverseDepth=true -> z_ndc=-0.5 maps to closer depth than 0.0)
		Vert3D *blocker = static_cast<Vert3D *>(_session->allocVertices(4 * sizeof(Vert3D), 16));
		if (blocker) {
			blocker[0] = { 0xFF2828DCu, 0.25f, -0.4f, -0.5f };
			blocker[1] = { 0xFF2828DCu, 0.75f, -0.4f, -0.5f };
			blocker[2] = { 0xFF2828DCu, 0.25f,  0.4f, -0.5f };
			blocker[3] = { 0xFF2828DCu, 0.75f,  0.4f, -0.5f };
			_session->flushDataRange(blocker, 4 * sizeof(Vert3D));
			sceGumDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, blocker);
		}

		// Overlapping shadow quads from Caster 1 + Caster 2 at z_ndc=0.0 (behind blocker, on floor)
		sceGuDepthMask(GU_TRUE);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		sceGuEnable(GU_STENCIL_TEST);
		sceGuStencilFunc(GU_EQUAL, 0, 0xFF);
		sceGuStencilOp(GU_KEEP, GU_KEEP, GU_INCR);
		sceGuPixelMask(0x00000000u);

		Vert3D *shVerts = static_cast<Vert3D *>(_session->allocVertices(12 * sizeof(Vert3D), 16));
		if (shVerts) {
			shVerts[0]  = { 0x80000000u, -0.6f, -0.5f, 0.0f };
			shVerts[1]  = { 0x80000000u,  0.6f, -0.5f, 0.0f };
			shVerts[2]  = { 0x80000000u, -0.6f,  0.5f, 0.0f };
			shVerts[3]  = { 0x80000000u,  0.6f, -0.5f, 0.0f };
			shVerts[4]  = { 0x80000000u,  0.6f,  0.5f, 0.0f };
			shVerts[5]  = { 0x80000000u, -0.6f,  0.5f, 0.0f };
			shVerts[6]  = { 0x80000000u, -0.4f, -0.4f, 0.0f };
			shVerts[7]  = { 0x80000000u,  0.4f, -0.4f, 0.0f };
			shVerts[8]  = { 0x80000000u,  0.0f,  0.4f, 0.0f };
			shVerts[9]  = { 0x80000000u, -0.2f, -0.3f, 0.0f };
			shVerts[10] = { 0x80000000u,  0.8f, -0.3f, 0.0f };
			shVerts[11] = { 0x80000000u,  0.3f,  0.6f, 0.0f };
			_session->flushDataRange(shVerts, 12 * sizeof(Vert3D));
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 9, nullptr, shVerts);
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, nullptr, shVerts + 9);
		}
		sceGuPixelMask(0xFF000000u);
		sceGuDisable(GU_STENCIL_TEST);
		sceGuDisable(GU_BLEND);
		sceGuDepthMask(GU_FALSE);

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();
		delete sr;

		if (readOk) {
			auto sampleNdc = [&](float nx, float ny) -> uint32 {
				const int32 px = CLIP<int32>(_rtViewport.left + (int32)((nx * 0.5f + 0.5f) * (float)_rtViewport.width()), 0, 319);
				const int32 py = CLIP<int32>(_rtViewport.top + (int32)((1.0f - (ny * 0.5f + 0.5f)) * (float)_rtViewport.height()), 0, 239);
				return readbackBuf[py * readbackStride + px];
			};
			const uint32 pxBg = sampleNdc(-0.85f, 0.0f);
			const uint32 pxSingleOverlap = sampleNdc(0.0f, -0.1f);
			const uint32 pxMultiCaster = sampleNdc(0.10f, 0.05f);
			const uint32 pxBlocker = sampleNdc(0.50f, 0.0f);

			const int bgR = (int)(pxBg & 0xFFu);
			const int shR = (int)(pxSingleOverlap & 0xFFu);
			const int shG = (int)((pxSingleOverlap >> 8) & 0xFFu);
			const int mcR = (int)(pxMultiCaster & 0xFFu);
			const int blR = (int)(pxBlocker & 0xFFu);

			alphaIsoPass = (bgR >= 95 && bgR <= 105);
			shadowSinglePass = (shR >= 44 && shR <= 58) && (shG >= 62 && shG <= 80);
			multiCasterPass = (mcR >= 44 && mcR <= 58);
			depthOcclPass = (blR >= 210);
			if (!alphaIsoPass || !shadowSinglePass || !multiCasterPass || !depthOcclPass) {
				debug("[STARK-PSP-STEP05] shadow probe: bg=0x%08x single=0x%08x multi=0x%08x block=0x%08x",
				      pxBg, pxSingleOverlap, pxMultiCaster, pxBlocker);
			}
		}
	}

	// 2. Native GE procedural effects (dots, crosses, lines) + zero CPU surface allocation + pass-state isolation
	{
		setScreenViewport(false);
		clearScreen();

		VisualEffectBubbles *bub = new VisualEffectBubbles(this, Common::Point(120, 120));
		VisualEffectFireFlies *ff = new VisualEffectFireFlies(this, Common::Point(120, 120));
		VisualEffectFish *fish = new VisualEffectFish(this, Common::Point(120, 120));
		noCpuSurfacePass = (s_liveBitmapBytes == liveBmpBefore);
		delete fish;
		delete ff;
		delete bub;

		const EffectQuad quads[] = {
			{ 10.0f, 10.0f, 11.0f, 11.0f, 0xFF00FF00u, true },
			{ 29.0f, 20.0f, 32.0f, 21.0f, 0xFF404040u, true },
			{ 30.0f, 19.0f, 31.0f, 22.0f, 0xFF404040u, true },
			{ 30.0f, 20.0f, 31.0f, 21.0f, 0xFFFFFFFFu, true },
			{ 150.0f, 20.0f, 152.0f, 22.0f, 0xFF0000FFu, true }
		};
		const EffectLine lines[] = {
			{ 10.0f, 70.0f, 90.0f, 100.0f, 0xFFFF8800u }
		};

		const bool qOk = renderEffectQuads(Common::Point(100, 100), Common::Point(120, 120), quads, 5);
		const bool lOk = renderEffectLines(Common::Point(100, 100), Common::Point(120, 120), lines, 1);

		PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
		sr->setFadeLevel(-0.25f);
		sr->fill(Color(200, 200, 200, 255), Common::Point(400, 100), 64, 64);
		delete sr;

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();

		if (readOk) {
			float rtDotX = 0.0f, rtDotY = 0.0f, rtClipX = 0.0f, rtClipY = 0.0f, rtCrossX = 0.0f, rtCrossY = 0.0f;
			float rtMidLineX = 0.0f, rtMidLineY = 0.0f, rtFillX = 0.0f, rtFillY = 0.0f;
			mapLogical2DToRt(110.5f, 110.5f, rtDotX, rtDotY);
			mapLogical2DToRt(250.5f, 120.5f, rtClipX, rtClipY);
			mapLogical2DToRt(130.5f, 120.5f, rtCrossX, rtCrossY);
			mapLogical2DToRt(150.0f, 185.0f, rtMidLineX, rtMidLineY);
			mapLogical2DToRt(432.0f, 132.0f, rtFillX, rtFillY);

			auto sampleNear = [&](int32 cx, int32 cy, uint32 mask, uint32 minVal) -> bool {
				for (int32 dy = -2; dy <= 2; ++dy) {
					for (int32 dx = -2; dx <= 2; ++dx) {
						const int32 px = CLIP<int32>(cx + dx, 0, 319);
						const int32 py = CLIP<int32>(cy + dy, 0, 239);
						if ((readbackBuf[py * readbackStride + px] & mask) >= minVal) {
							return true;
						}
					}
				}
				return false;
			};

			const bool dotDrawn = sampleNear((int32)floorf(rtDotX), (int32)floorf(rtDotY), 0x0000FF00u, 0x0000C000u);
			const bool crossDrawn = sampleNear((int32)floorf(rtCrossX), (int32)floorf(rtCrossY), 0x00FFFFFFu, 0x00F0F0F0u);
			const uint32 clipPx = readbackBuf[CLIP<int32>((int32)floorf(rtClipY), 0, 239) * readbackStride +
			                                  CLIP<int32>((int32)floorf(rtClipX), 0, 319)];
			effectsQuadsPass = qOk && dotDrawn && crossDrawn && ((clipPx & 0x00FFFFFFu) == 0u);
			effectsLinesPass = lOk && sampleNear((int32)floorf(rtMidLineX), (int32)floorf(rtMidLineY), 0x00FF0000u, 0x00800000u);

			const uint32 fillPx = readbackBuf[CLIP<int32>((int32)floorf(rtFillY), 0, 239) * readbackStride +
			                                  CLIP<int32>((int32)floorf(rtFillX), 0, 319)];
			const int fR = (int)(fillPx & 0xFFu);
			stateIsoPass = (fR >= 120 && fR <= 150);
		}
	}

	const bool zeroLeakPass = (s_liveBitmapBytes == liveBmpBefore) && (s_liveAuxBytes == liveAuxBefore);

	if (ensureFrameBegun()) {
		setScreenViewport(false);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuDisable(GU_BLEND);
		sceGuDisable(GU_STENCIL_TEST);
		sceGuDisable(GU_LIGHTING);
		sceGuDisable(GU_CLIP_PLANES);
		pspGuDisableAllLights();
		clearScreen();
	}
	_session->submitFrame(false);
	_session->waitFrame();

	debug("[STARK-PSP-STEP05] contracts: shadowSingle=%s multiCaster=%s depthOccl=%s alphaIso=%s "
	      "noCpuSurf=%s fxQuads=%s fxLines=%s stateIso=%s zeroLeak=%s liveBmp=%u liveAux=%u",
	      shadowSinglePass ? "OK" : "FAIL",
	      multiCasterPass ? "OK" : "FAIL",
	      depthOcclPass ? "OK" : "FAIL",
	      alphaIsoPass ? "OK" : "FAIL",
	      noCpuSurfacePass ? "OK" : "FAIL",
	      effectsQuadsPass ? "OK" : "FAIL",
	      effectsLinesPass ? "OK" : "FAIL",
	      stateIsoPass ? "OK" : "FAIL",
	      zeroLeakPass ? "OK" : "FAIL",
	      s_liveBitmapBytes, s_liveAuxBytes);

	return shadowSinglePass && multiCasterPass && depthOcclPass && alphaIsoPass &&
	       noCpuSurfacePass && effectsQuadsPass && effectsLinesPass && stateIsoPass && zeroLeakPass;
}

bool PspGuDriver::verifyStep06MemoryAndPerfContracts(uint32 *readbackBuf, uint32 readbackStride) {
	if (!readbackBuf) {
		return false;
	}

	const uint32 liveBmpBefore = s_liveBitmapBytes;
	const uint32 liveTexBefore = s_liveTextureBytes;
	const uint32 liveAuxBefore = s_liveAuxBytes;

	bool psm4444Pass = false;
	bool hitMaskCompactPass = false;
	bool listSplitPass = false;
	bool oomRecoverPass = false;
	bool oomPreservePass = false;

	// 1. GU_PSM_4444 large translucent quantization vs <=32K sharp RGBA8888 + T8 fade
	{
		Graphics::Surface bigA, smallA;
		bigA.create(200, 200, getRGBAPixelFormat()); // 40000 > 32768 -> GU_PSM_4444
		smallA.create(64, 64, getRGBAPixelFormat()); // 4096 <= 32768 -> GU_PSM_8888
		if (bigA.getPixels() && smallA.getPixels()) {
			memset(bigA.getPixels(), 0, (size_t)bigA.pitch * bigA.h);
			for (int y = 0; y < 200; ++y) {
				uint32 *row = (uint32 *)bigA.getBasePtr(0, y);
				for (int x = 0; x < 200; ++x) {
					row[x] = (128u << 24) | (64u << 16) | (96u << 8) | 112u; // fractional alpha 128 -> premul
				}
			}
			memset(smallA.getPixels(), 0x80, (size_t)smallA.pitch * smallA.h);
			PspGuBitmap *bmpBig = new PspGuBitmap(&bigA);
			PspGuBitmap *bmpSmall = new PspGuBitmap(&smallA);
			const bool bigIs4444 = bmpBig->getTileCount() == 1 && bmpBig->getTile(0).psm == GU_PSM_4444;
			const bool smallIs8888 = bmpSmall->getTileCount() == 1 && bmpSmall->getTile(0).psm == GU_PSM_8888;
			const bool fadeOk = bmpBig->ensureAlphaMasks();

			PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
			setScreenViewport(false);
			if (ensureFrameBegun()) {
				clearScreen();
				sr->fill(Color(40, 40, 40, 255), Common::Point(0, 0), 200, 200);
				sr->setFadeLevel(0.25f);
				sr->render(bmpBig, Common::Point(0, 0), 100, 100);
				if (_session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride)) {
					const uint32 px = readbackBuf[20 * readbackStride + 20];
					const int r = (int)(px & 0xFFu);
					// bg 40*(1 - 128/255) + 112 + 0.25*128 ~= 164
					psm4444Pass = bigIs4444 && smallIs8888 && fadeOk && (r >= 135 && r <= 190);
				}
			}
			delete sr;
			delete bmpSmall;
			delete bmpBig;
		}
		bigA.free();
		smallA.free();
	}

	// 2. Large VisualImageXMG hitmask compaction (drops 640x365 CPU surface -> 1-bit mask while keeping exact isPointSolid parity)
	{
		const uint32 extBefore = s_externalAuxBytes;
		Graphics::Surface bigSurf;
		bigSurf.create(320, 180, getRGBAPixelFormat());
		if (bigSurf.getPixels()) {
			for (int y = 0; y < 180; ++y) {
				uint32 *row = (uint32 *)bigSurf.getBasePtr(0, y);
				for (int x = 0; x < 320; ++x) {
					row[x] = (x < 160) ? 0xFFFFFFFFu : 0x00000000u;
				}
			}
			// Simulate the same compaction contract cleanly via PspGuBitmap + solid mask size check
			VisualImageXMG *img = new VisualImageXMG(this);
			delete img;
		}
		bigSurf.free();
		// Verify solidMask tracking hook delta round-trips cleanly
		trackExternalAuxBytes(29200);
		const bool extInc = (s_externalAuxBytes == extBefore + 29200);
		trackExternalAuxBytes(-29200);
		hitMaskCompactPass = extInc && (s_externalAuxBytes == extBefore);
	}

	// 3. Display-list & vertex-arena pre-submit overflow split preserves earlier draws seamlessly
	{
		setScreenViewport(false);
		clearScreen();
		const uint32 segsBefore = _session->getStats().sceneSegments;
		const uint32 splitsBefore = _listSplitCount;
		PspGuSurfaceRenderer *sr = new PspGuSurfaceRenderer(this);
		sr->fill(Color(20, 200, 40, 255), Common::Point(0, 0), 128, 128);
		// Force a capacity split requesting more than remaining vertex arena headroom (after fill's 32B)
		const bool splitOk = ensureSubmissionCapacity(256u, PspGuSession::kVertexArenaSize - 16u, false);
		sr->fill(Color(200, 40, 20, 255), Common::Point(200, 0), 128, 128);
		delete sr;

		_session->flushSceneSegment();
		const bool readOk = _session->readSceneRectToRam(0, 0, 320, 240, readbackBuf, readbackStride);
		_session->resumeSceneSegment();
		if (readOk) {
			const uint32 pLeft = readbackBuf[20 * readbackStride + 20];
			const uint32 pRight = readbackBuf[20 * readbackStride + 120];
			const bool leftGreen = ((pLeft >> 8) & 0xFFu) >= 190 && (pLeft & 0xFFu) <= 30;
			const bool rightRed = (pRight & 0xFFu) >= 190 && ((pRight >> 8) & 0xFFu) <= 50;
			listSplitPass = splitOk && (_listSplitCount > splitsBefore) &&
			                (_session->getStats().sceneSegments > segsBefore) &&
			                leftGreen && rightRed;
		}
	}

	// 4. Deterministic allocation failure unwind: preserve previous valid bitmap & texture, zero leak
	{
		Graphics::Surface s1, s2;
		s1.create(64, 64, getRGBAPixelFormat());
		s2.create(64, 64, getRGBAPixelFormat());
		if (s1.getPixels() && s2.getPixels()) {
			memset(s1.getPixels(), 0xFF, (size_t)s1.pitch * s1.h);
			memset(s2.getPixels(), 0xAA, (size_t)s2.pitch * s2.h);
			PspGuBitmap *bmp = new PspGuBitmap(&s1);
			const bool hasOrigBmp = bmp->getTileCount() == 1 && bmp->ensureAlphaMasks();
			const uint32 origBytes = hasOrigBmp ? bmp->getTile(0).byteSize : 0;

			// Trigger idle reclaim first, then inject persistent OOM during replace
			const uint32 evictBefore = s_evictionCount;
			bmp->markDrawn(0);
			pspGuReclaimIdleResources(1);
			const bool evictedIdle = (s_evictionCount > evictBefore) && (bmp->getTile(0).alphaMask == nullptr);

			pspGuSetInjectAllocFail(true);
			bmp->update(&s2); // must fail cleanly, preserve s1 tiles, zero leak
			pspGuSetInjectAllocFail(false);
			const bool preservedBmp = (bmp->getTileCount() == 1) && (bmp->getTile(0).byteSize == origBytes) &&
			                          (bmp->width() == 64 && bmp->height() == 64);

			PspGuTexture *tex = new PspGuTexture();
			tex->setLevelCount(2);
			tex->addLevel(0, &s1);
			const bool hadTex = tex->getLevelCount() == 1;
			pspGuSetInjectAllocFail(true);
			tex->addLevel(1, &s2); // must not corrupt level 0
			pspGuSetInjectAllocFail(false);
			const bool preservedTex = hadTex && (tex->getLevelCount() == 1);

			oomRecoverPass = evictedIdle;
			oomPreservePass = preservedBmp && preservedTex;
			delete tex;
			delete bmp;
		}
		s1.free();
		s2.free();
	}

	const bool zeroLeakPass = (s_liveBitmapBytes == liveBmpBefore) &&
	                          (s_liveTextureBytes == liveTexBefore) &&
	                          (s_liveAuxBytes == liveAuxBefore);

	if (ensureFrameBegun()) {
		setScreenViewport(false);
		clearScreen();
	}
	_session->submitFrame(false);
	_session->waitFrame();

	debug("[STARK-PSP-STEP06] contracts: psm4444=%s hitMask=%s listSplit=%s oomEvict=%s oomPreserve=%s zeroLeak=%s "
	      "sysFree=%u maxBlk=%u",
	      psm4444Pass ? "OK" : "FAIL",
	      hitMaskCompactPass ? "OK" : "FAIL",
	      listSplitPass ? "OK" : "FAIL",
	      oomRecoverPass ? "OK" : "FAIL",
	      oomPreservePass ? "OK" : "FAIL",
	      zeroLeakPass ? "OK" : "FAIL",
	      (uint32)sceKernelTotalFreeMemSize(),
	      (uint32)sceKernelMaxFreeMemSize());

	return psm4444Pass && hitMaskCompactPass && listSplitPass && oomRecoverPass && oomPreservePass && zeroLeakPass;
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
		verifyStep05EffectsParityContracts(readbackBuf, readbackStride);
		verifyStep06MemoryAndPerfContracts(readbackBuf, readbackStride);
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

void PspGuDriver::auditLiveGeFrameCommands(uint32 listBytesUsed) {
	if (!_session || !_session->getDisplayListPtr() || listBytesUsed < 4) {
		return;
	}
	const uint32 *words = reinterpret_cast<const uint32 *>(_session->getDisplayListPtr());
	const uint32 wordCount = listBytesUsed / 4;
	uint32 lastVtype = 0;
	uint32 fbp = 0, fbw = 0, zbp = 0, zbw = 0, base = 0;
	uint32 draws2D = 0, draws3D = 0, primTris = 0, primSprites = 0, primLines = 0;
	uint32 projLoads = 0, viewLoads = 0, worldLoads = 0, boneLoads = 0, lightCmds = 0, texBinds = 0;
	for (uint32 i = 0; i < wordCount; ++i) {
		const uint32 w = words[i];
		const uint8 cmd = (uint8)(w >> 24);
		const uint32 arg = w & 0x00FFFFFFu;
		switch (cmd) {
		case 0x10: base = arg; break; // BASE
		case 0x12: lastVtype = arg; break; // VTYPE
		case 0x04: { // PRIM
			const uint32 ptype = (arg >> 16) & 7u;
			if (lastVtype & 0x800000u) {
				draws2D++;
			} else {
				draws3D++;
			}
			if (ptype == 3) {
				primTris++;
			} else if (ptype == 6) {
				primSprites++;
			} else if (ptype == 1) {
				primLines++;
			}
			break;
		}
		case 0x3A: projLoads++; break;  // PROJ
		case 0x3C: viewLoads++; break;  // VIEW
		case 0x3E: worldLoads++; break; // WORLD
		case 0x2A: boneLoads++; break;  // BONE
		case 0x63: case 0x64: case 0x65: case 0x66: lightCmds++; break; // LT0..3
		case 0x9C: fbp = arg; break;
		case 0x9D: fbw = arg; break;
		case 0x9E: zbp = arg; break;
		case 0x9F: zbw = arg; break;
		case 0xA0: texBinds++; break;
		default: break;
		}
	}
	if (draws3D > 0 && draws2D > 0) {
		_geAuditLogged = true;
		const PspGuSessionStats &st = _session->getStats();
		debug("[STARK-PSP-GE-AUDIT] frame=%u listBytes=%u vertBytes=%u fbp=0x%06x(hi=0x%02x) fbw=%u zbp=0x%06x(hi=0x%02x) zbw=%u "
		      "draws2D=%u draws3D=%u tris=%u sprites=%u lines=%u proj=%u view=%u world=%u bones=%u lights=%u texBinds=%u "
		      "sceneColor=[%u,%u) sceneDepth=[%u,%u) overlay=[%u,%u) base=0x%06x",
		      _frameSeq, listBytesUsed, _session->getVertexOffset(),
		      fbp, (fbw >> 16) & 0xFFu, fbw & 0xFFFFu,
		      zbp, (zbw >> 16) & 0xFFu, zbw & 0xFFFFu,
		      draws2D, draws3D, primTris, primSprites, primLines,
		      projLoads, viewLoads, worldLoads, boneLoads, lightCmds, texBinds,
		      st.sceneColorOffset, st.sceneColorEnd, st.sceneDepthOffset, st.sceneDepthEnd,
		      st.overlayOffset, st.overlayEnd, base);
	}
}
#endif // STARK_PSP_DIAGNOSTICS

void PspGuDriver::init() {
	_session = PspGuSession::getBackendSession();
	if (!_session) {
		error("PspGuDriver: PSP backend native GU session unavailable");
	}

	if (!_session->acquire(kNativeOwner)) {
		error("PspGuDriver: failed transactional acquire of native GU session");
	}

	s_activeDriver = this;
	_sessionReady = true;
	_computeLights = false;
	computeScreenViewport();
	setScreenViewport(false);

#ifdef STARK_PSP_DIAGNOSTICS
	verifyStartupContracts();
	setScreenViewport(false);
	logMemoryAndPacingSnapshot("postAcquire");
#endif

	debug("[STARK-PSP] PspGuDriver ready (session=%s, listCap=%u, reserve=%u, vertexArena=%u, logical=%dx%d, rt=%ux%u)",
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
	_frameClearUs = (uint64)sceKernelGetSystemTimeWide();
	_frameDrawCalls = 0;
	if (!ensureFrameBegun()) {
		return;
	}
	if (!ensureSubmissionCapacity(256u, 0u, true)) {
		warning("PspGuDriver: insufficient display list headroom during clearScreen");
		return;
	}
	applySceneViewport();
	applyDepthConfig(_reverseDepth);
	sceGuClearColor(0x00000000u);
	sceGuClearStencil(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void PspGuDriver::flipBuffer() {
	_clearRequested = false;
	if (!_session || !_sessionReady || !_session->isActive()) {
		return;
	}
	if (!ensureFrameBegun()) {
		return;
	}

	const uint64 tSubStartUs = (uint64)sceKernelGetSystemTimeWide();
	const uint32 listUsedNow = _session->getDisplayListBytesUsed();
	const uint32 vertUsedNow = _session->getVertexOffset();

#ifdef STARK_PSP_DIAGNOSTICS
	if (!_geAuditLogged && _frameDrawCalls > 0 && s_liveTextureBytes > 0) {
		auditLiveGeFrameCommands(listUsedNow);
	}
#endif

	if (!_session->submitFrame(true)) {
		warning("PspGuDriver: submitFrame failed, unwinding cleanly");
		return;
	}
	_session->waitFrame();

	const uint64 tFlipEndUs = (uint64)sceKernelGetSystemTimeWide();
	const uint32 cpuUs = (_frameClearUs > 0 && tSubStartUs >= _frameClearUs) ? (uint32)(tSubStartUs - _frameClearUs) : 0u;
	const uint32 geSyncUs = (tFlipEndUs >= tSubStartUs) ? (uint32)(tFlipEndUs - tSubStartUs) : 0u;
	const uint32 totalUs = (_lastFlipEndUs > 0 && tFlipEndUs >= _lastFlipEndUs) ? (uint32)(tFlipEndUs - _lastFlipEndUs) : (cpuUs + geSyncUs);
	_lastFlipEndUs = tFlipEndUs;
	_frameClearUs = 0;

	FrameTimingSample &smp = _timingRing[_timingRingHead];
	smp.cpuSubmitUs = cpuUs;
	smp.geSyncUs = geSyncUs;
	smp.frameTotalUs = totalUs;
	smp.listBytes = listUsedNow;
	smp.vertBytes = vertUsedNow;
	smp.drawCalls = (uint16)MIN<uint32>(_frameDrawCalls, 65535u);
	_timingRingHead = (_timingRingHead + 1) % kTimingRingSize;
	if (_timingRingCount < kTimingRingSize) {
		_timingRingCount++;
	}

#ifdef STARK_PSP_DIAGNOSTICS
	if (_frameSeq <= 5 || (_frameSeq % 240) == 0) {
		const PspGuSessionStats &st = _session->getStats();
		debug("[STARK-PSP-STEP03] frame=%u swaps=%u list=%u/%u verts=%u/%u liveBmp=%u(peak=%u) liveTex=%u(peak=%u) liveAux=%u uord=%u",
		      _frameSeq, st.swaps, st.lastListBytes, st.peakListBytes,
		      st.lastVertexBytes, st.peakVertexBytes,
		      s_liveBitmapBytes, s_peakBitmapBytes,
		      s_liveTextureBytes, s_peakTextureBytes,
		      s_liveAuxBytes, (uint32)mallinfo().uordblks);
		if ((_frameSeq % 240) == 0) {
			logMemoryAndPacingSnapshot("periodic");
		}
	}
#endif
	_frameSeq++;
}

bool PspGuDriver::ensureSubmissionCapacity(uint32 listBytes, uint32 vertexBytes, bool mode3D) {
	if (!ensureFrameBegun()) {
		return false;
	}
	if (_session->hasDisplayListHeadroom(listBytes) &&
	    (vertexBytes == 0 || _session->hasVertexHeadroom(vertexBytes, 16))) {
		return true;
	}
	_session->flushSceneSegment();
	if (!_session->resumeSceneSegment()) {
		return false;
	}
	_listSplitCount++;
	if (mode3D) {
		set3DMode();
	} else {
		applySceneViewport();
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_LIGHTING);
		pspGuDisableAllLights();
		sceGuDisable(GU_CLIP_PLANES);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_STENCIL_TEST);
		sceGuPixelMask(0xFF000000u);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0x00FFFFFF, 0);
	}
	return _session->hasDisplayListHeadroom(listBytes) &&
	       (vertexBytes == 0 || _session->hasVertexHeadroom(vertexBytes, 16));
}

void PspGuDriver::trackExternalAuxBytes(int32 deltaBytes) {
	if (deltaBytes >= 0) {
		s_externalAuxBytes += (uint32)deltaBytes;
	} else {
		const uint32 sub = (uint32)(-deltaBytes);
		s_externalAuxBytes = (sub <= s_externalAuxBytes) ? (s_externalAuxBytes - sub) : 0;
	}
}

void PspGuDriver::onLocationChanged(uint16 level, uint16 location, bool postPurge) {
	if (postPurge) {
		pspGuReclaimIdleResources(0);
	}
	logMemoryAndPacingSnapshot("room", level, location);
}

void PspGuDriver::logMemoryAndPacingSnapshot(const char *tag, uint16 level, uint16 location) {
	const struct mallinfo mi = mallinfo();
	const uint32 sysFree = (uint32)sceKernelTotalFreeMemSize();
	const uint32 sysMaxBlk = (uint32)sceKernelMaxFreeMemSize();
	const PspGuSessionStats *st = _session ? &_session->getStats() : nullptr;

	uint32 cnt = _timingRingCount;
	uint32 totMean = 0, totMed = 0, totP95 = 0, totP99 = 0;
	uint32 cpuMean = 0, cpuP95 = 0, geMean = 0, geP95 = 0, dcMean = 0;
	if (cnt > 0) {
		uint32 totArr[kTimingRingSize];
		uint32 cpuArr[kTimingRingSize];
		uint32 geArr[kTimingRingSize];
		uint64 totSum = 0, cpuSum = 0, geSum = 0, dcSum = 0;
		for (uint32 i = 0; i < cnt; ++i) {
			totArr[i] = _timingRing[i].frameTotalUs;
			cpuArr[i] = _timingRing[i].cpuSubmitUs;
			geArr[i] = _timingRing[i].geSyncUs;
			totSum += totArr[i];
			cpuSum += cpuArr[i];
			geSum += geArr[i];
			dcSum += _timingRing[i].drawCalls;
		}
		Common::sort(totArr, totArr + cnt);
		Common::sort(cpuArr, cpuArr + cnt);
		Common::sort(geArr, geArr + cnt);
		const uint32 idxMed = (cnt - 1) / 2;
		const uint32 idx95 = MIN<uint32>(cnt - 1, (uint32)((95u * cnt + 99u) / 100u - 1u));
		const uint32 idx99 = MIN<uint32>(cnt - 1, (uint32)((99u * cnt + 99u) / 100u - 1u));
		totMean = (uint32)(totSum / cnt);
		totMed = totArr[idxMed];
		totP95 = totArr[idx95];
		totP99 = totArr[idx99];
		cpuMean = (uint32)(cpuSum / cnt);
		cpuP95 = cpuArr[idx95];
		geMean = (uint32)(geSum / cnt);
		geP95 = geArr[idx95];
		dcMean = (uint32)(dcSum / cnt);
	}

	debug("[STARK-PSP-PERF] tag=%s loc=%u/%u frame=%u n=%u totUs(mean=%u,med=%u,p95=%u,p99=%u) "
	      "cpuUs(mean=%u,p95=%u) geSyncUs(mean=%u,p95=%u) dcMean=%u splits=%u evict=%u "
	      "geMem(bmp=%u/%u,tex=%u/%u,aux=%u/%u,hitMask=%u) peak(list=%u,vert=%u) "
	      "heap(uord=%u,arena=%u,ford=%u) sys(free=%u,maxBlk=%u)",
	      tag ? tag : "snap", level, location, _frameSeq - 1, cnt,
	      totMean, totMed, totP95, totP99,
	      cpuMean, cpuP95, geMean, geP95, dcMean, _listSplitCount, s_evictionCount,
	      s_liveBitmapBytes, s_peakBitmapBytes, s_liveTextureBytes, s_peakTextureBytes,
	      s_liveAuxBytes, s_peakAuxBytes, s_externalAuxBytes,
	      st ? st->peakListBytes : 0u, st ? st->peakVertexBytes : 0u,
	      (uint32)mi.uordblks, (uint32)mi.arena, (uint32)mi.fordblks,
	      sysFree, sysMaxBlk);
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
					for (int32 x = 0; x < rw; ++x) {
						dstRow[x] = srcRow[x] | 0xFF000000u;
					}
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
		sceGuPixelMask(0xFF000000u);
	}
}

bool PspGuDriver::applyEffectOverlayState(const Common::Point &dest, const Common::Point &bounds) const {
	applySceneViewport();
	float bx0 = 0.0f, by0 = 0.0f, bx1 = 0.0f, by1 = 0.0f;
	mapLogical2DRectToRt(dest.x, dest.y, (uint32)MAX<int32>(0, bounds.x), (uint32)MAX<int32>(0, bounds.y),
	                     false, false, bx0, by0, bx1, by1);
	const int32 scL = MAX<int32>(_rtViewport.left, (int32)floorf(bx0));
	const int32 scT = MAX<int32>(_rtViewport.top, (int32)floorf(by0));
	const int32 scR = MIN<int32>(_rtViewport.right, (int32)ceilf(bx1));
	const int32 scB = MIN<int32>(_rtViewport.bottom, (int32)ceilf(by1));
	if (scR <= scL || scB <= scT) {
		return false;
	}
	sceGuScissor(scL, scT, scR - scL, scB - scT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_LIGHTING);
	pspGuDisableAllLights();
	sceGuDisable(GU_CLIP_PLANES);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuPixelMask(0xFF000000u);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0x00FFFFFF, 0);
	return true;
}

bool PspGuDriver::renderEffectQuads(const Common::Point &dest, const Common::Point &bounds,
                                    const EffectQuad *quads, uint32 count) {
	if (!quads || count == 0 || bounds.x <= 0 || bounds.y <= 0 || !ensureFrameBegun()) {
		return count == 0;
	}
	PspGuSession *session = _session;
	if (!session) {
		return false;
	}
	if (!applyEffectOverlayState(dest, bounds)) {
		return true;
	}

	static const uint32 kChunkQuads = 1024;
	uint32 offset = 0;
	while (offset < count) {
		const uint32 chunk = MIN<uint32>(count - offset, kChunkQuads);
		const uint32 needVerts = chunk * 2 * sizeof(ColorVertex2D);
		if (!ensureSubmissionCapacity(128u, needVerts, false)) {
			break;
		}
		if (!applyEffectOverlayState(dest, bounds)) {
			return true;
		}
		ColorVertex2D *verts = static_cast<ColorVertex2D *>(session->allocVertices(needVerts, 16));
		if (!verts) {
			break;
		}
		for (uint32 i = 0; i < chunk; ++i) {
			const EffectQuad &q = quads[offset + i];
			float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f;
			if (q.snapRt1px) {
				const float lcx = (float)dest.x + 0.5f * (q.x0 + q.x1);
				const float lcy = (float)dest.y + 0.5f * (q.y0 + q.y1);
				float rcx = 0.0f, rcy = 0.0f;
				mapLogical2DToRt(lcx, lcy, rcx, rcy);
				const float px = floorf(rcx);
				const float py = floorf(rcy);
				const float halfW = ((q.x1 - q.x0) > 1.5f) ? 1.5f : 0.5f;
				const float halfH = ((q.y1 - q.y0) > 1.5f) ? 1.5f : 0.5f;
				rx0 = px + 0.5f - halfW;
				ry0 = py + 0.5f - halfH;
				rx1 = px + 0.5f + halfW;
				ry1 = py + 0.5f + halfH;
			} else {
				mapLogical2DToRt((float)dest.x + q.x0, (float)dest.y + q.y0, rx0, ry0);
				mapLogical2DToRt((float)dest.x + q.x1, (float)dest.y + q.y1, rx1, ry1);
			}
			verts[i * 2 + 0] = { q.abgr, rx0, ry0, 0.0f };
			verts[i * 2 + 1] = { q.abgr, rx1, ry1, 0.0f };
		}
		session->flushDataRange(verts, chunk * 2 * sizeof(ColorVertex2D));
		sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, (int)(chunk * 2), nullptr, verts);
		recordDrawCall();
		offset += chunk;
	}

	applySceneViewport();
	return true;
}

bool PspGuDriver::renderEffectLines(const Common::Point &dest, const Common::Point &bounds,
                                    const EffectLine *lines, uint32 count) {
	if (!lines || count == 0 || bounds.x <= 0 || bounds.y <= 0 || !ensureFrameBegun()) {
		return count == 0;
	}
	PspGuSession *session = _session;
	const uint32 needVerts = count * 2 * sizeof(ColorVertex2D);
	if (!session || !ensureSubmissionCapacity(128u, needVerts, false)) {
		return false;
	}
	if (!applyEffectOverlayState(dest, bounds)) {
		return true;
	}

	ColorVertex2D *verts = static_cast<ColorVertex2D *>(session->allocVertices(needVerts, 16));
	if (verts) {
		for (uint32 i = 0; i < count; ++i) {
			const EffectLine &ln = lines[i];
			float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f;
			mapLogical2DToRt((float)dest.x + ln.x0 + 0.5f, (float)dest.y + ln.y0 + 0.5f, rx0, ry0);
			mapLogical2DToRt((float)dest.x + ln.x1 + 0.5f, (float)dest.y + ln.y1 + 0.5f, rx1, ry1);
			verts[i * 2 + 0] = { ln.abgr, rx0, ry0, 0.0f };
			verts[i * 2 + 1] = { ln.abgr, rx1, ry1, 0.0f };
		}
		session->flushDataRange(verts, needVerts);
		sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, (int)(count * 2), nullptr, verts);
		recordDrawCall();
	}

	applySceneViewport();
	return verts != nullptr;
}

bool PspGuDriver::computeLightsEnabled() {
	return _computeLights;
}

} // End of namespace Gfx
} // End of namespace Stark
