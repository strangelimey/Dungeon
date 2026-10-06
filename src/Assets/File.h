#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

// The whole file, or why not: a missing file, a DIRECTORY, a short read or one
// that cannot be sized (a pipe, a device) is an error - never a throw or a
// SIZE_MAX buffer. Paths here are UTF-8 (Core/Paths.h).
std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path);

// Writes (replaces) a file; creates parent directories as needed. False when
// the open, the write or the closing flush fails.
bool WriteBinaryFile(const std::string& path, const void* data, size_t size);

// Whether a DERIVED file may stand in for its source: a .dds mip chain for the
// PNG beside it, a model's embedded-image sidecar (EmbeddedImageSidecar) for
// the model. It must exist and be no older than the source; a missing source
// cannot make it stale (a .dds shipped without its PNG is the only copy).
//
// The ONE statement of the rule (code-review C410): the bakers skip what it
// calls current, and the loaders refuse - and say so - what it does not. It was
// written out three times in the bakers and the model loader, and the texture
// loader did not apply it at all, so a `runes` re-bake that wrote only PNGs
// left the game quietly drawing the old .dds.
bool BakedIsCurrent(const std::string& baked, const std::string& source);

} // namespace dungeon::assets
