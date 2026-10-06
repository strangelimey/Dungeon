#pragma once

#include <string>

namespace dungeon::baker {

// Writes all .gltf models (dungeon blocks, props, monsters) into <modelsDir>.
// The worn block variants displace by the scanned height maps found in
// <texturesDir> (<texture>_1k_n.png alpha), falling back to procedural wear
// with a warning when a set is not installed. Every shipped set bakes at its
// record in Assets/WornSets.h.
bool BakeModels(const std::string& modelsDir, const std::string& texturesDir);

// Bakes the worn block meshes (3 tiers) for a single surface texture set -
// `kind` is "wall", "floor", or "ceiling". Used after importing a new texture
// set so a level can reference it, and by the type editor's save. The set's
// kind, relief and seed come from its record (Assets/WornSets.h): a shipped
// set asked for as another kind is REFUSED, an unlisted one is derived.
// `relief` is the displacement AMPLITUDE in metres - how far the stones stand
// proud of the panel - and `wearScale` (a type's `wear`) scales it, 0 = flat.
// Together they scale the WHOLE displacement against the record's relief.
// Splitting the two is the point: `wear` alone could only ever take relief
// away, since 1.0 meant "the amount baked in". A negative `relief` keeps the
// record's, so a type that sets neither bakes what `BakeModels` does.
bool BakeWornBlocks(const std::string& kind, const std::string& name,
					const std::string& modelsDir, const std::string& texturesDir,
					float wearScale = 1.0f, float relief = -1.0f);

} // namespace dungeon::baker
