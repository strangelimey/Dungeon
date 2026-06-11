#pragma once

#include "Assets/Model.h"

namespace dungeon::game {

// Builds a skinned "enchanted serpent pillar" entirely in code: a four-joint
// chain, a skinned cylinder mesh, and a looping sway animation clip. This
// exercises the full Assets -> Animation -> Graphics skinning path without
// needing a model file on disk. (Real .gltf/.glb/.obj files load through
// assets::LoadModel just the same.)
assets::ModelData BuildSerpentPillar();

} // namespace dungeon::game
