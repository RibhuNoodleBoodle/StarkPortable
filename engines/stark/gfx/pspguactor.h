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

namespace Stark {
namespace Gfx {

class PspGuDriver;

/**
 * Step 01 placeholder 3D actor renderer for native PSP sceGu driver.
 */
class PspGuActorRenderer : public VisualActor {
public:
	explicit PspGuActorRenderer(PspGuDriver *gfx);
	~PspGuActorRenderer() override;

	void render(const Math::Vector3d &position, float direction, const Common::Array<Gfx::LightEntry *> &lights) override;

private:
	PspGuDriver *_gfx;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif // STARK_GFX_PSPGU_ACTOR_H
