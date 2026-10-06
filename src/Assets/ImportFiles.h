// ============================================================================
// Assets/ImportFiles.h - which files in the pool belong to one editor import.
//
// An editor import lands in the shared pool (assets/textures, assets/models)
// and is recorded in its world's imports.cat by POOL NAME: a texture set as
// "<set>_2k" (the resolution-tagged name LoadPbrSet falls back to), a model as
// "<name>". A packaged build's "To source" copies those files into the source
// tree (Game::SyncProjectToSource), and it used to find them by a bare name
// PREFIX (code-review C336): a texture set's worn meshes were looked for as
// worn_<set>_2k*, which nothing writes - the baker names them by the BASE,
// worn_<set>_<tier> - so a surface reached the source tree without them and a
// fresh clone aborted on the first level that painted it; and a model named
// `pot` swept up pottery.gltf, potion_vial.glb and pottery_2k's maps beside
// its own. This states the files exactly, by the names each writer gives them:
//   * texture set <set> (AssetBaker import): <set>.png / .dds, <set>_n.png /
//     .dds and <set>_mr.png / .dds;
//   * its worn meshes (AssetBaker wornblock): worn_<base>_<low|med|high>.gltf,
//     plus a wall's panels worn_<base>_<tier>_<phase>[l][r].gltf
//     (Assets/WornPanel.h), <base> being the set less its resolution tag;
//   * model <name> (AssetBaker import-model): <name>.gltf or .glb, its
//     embedded-image sidecars <name>.gltf.<n>.dds (model-images), and the
//     texture set it brought in, <name>_2k.
// Pure (std::string_view, no file system), so tools/RollTest checks the rule.
// ============================================================================
#pragma once

#include <string_view>

namespace dungeon::assets {

// The pool folder a file sits in.
enum class PoolDir { Textures, Models };

// A texture set's pool name less its resolution tag: "marble_2k" -> "marble"
// (_1k / _2k / _4k / _8k). Any other name is its own base.
std::string_view TextureSetBase(std::string_view poolName);

// Whether `file` - a bare file name, exactly as the pool holds it - in `dir`
// belongs to the import recorded under `poolName`: an import-model record
// (imports.cat `kind = model`) when `model`, else a texture set's.
bool ImportOwnsFile(std::string_view poolName, bool model, PoolDir dir, std::string_view file);

} // namespace dungeon::assets
