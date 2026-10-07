# DuckStation texture format and xxHash provenance

The independent format integration is described in
[DuckStation texture format notes](../../DUCKSTATION_TEXTURE_FORMAT.md).
Reference behavior was observed at DuckStation commit
[697599c47a646a6cfcc4d246018adf4904d55001](https://github.com/stenzek/duckstation/tree/697599c47a646a6cfcc4d246018adf4904d55001),
whose [license](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/LICENSE)
is CC-BY-NC-ND-4.0. DuckStation is authored by Stenzek and its contributors;
its authorship is credited by the pinned reference links. No DuckStation
implementation, translated implementation, patch, or binary is included,
and no DuckStation co-author trailer is claimed for this independent work.

Implemented behavior: bounded modern texture filenames and palette ranges,
XXH3 identities over native pixel sources and palettes, supported alpha
classification, replacement loading, and original-texture dumping. Related
coalescing, copy/split identities, multi-image composition, wrapped footprints,
JPEG/WebP decoding, and DuckStation rendering/cache implementation are excluded.
The format guide records the exact compatibility limits.

The only vendored implementation is unmodified
[xxHash v0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3), authored by Yann
Collet and contributors under
[BSD-2-Clause](https://github.com/Cyan4973/xxHash/blob/v0.8.3/LICENSE).
`runtime/third_party/xxhash.h` retains its original copyright and license;
its SHA-256 is
`17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.
Player packages carry `runtime/licenses/xxhash-NOTICES.txt` as a runtime
notice. Vendoring preserves authorship in the unmodified source and notices,
rather than attributing that source to this change's author.

Validation on current framework master `92c0f3cd`: registered DuckStation
module and OpenGL renderer tests pass, alongside mod package, builtin
manifest, runtime, and resident tests. Independent xxHash wheel vectors,
literal filenames, PNG pixels and file preservation, cache/reload behavior,
alpha handling, upload aborts, and native VRAM invariance are covered.
Tomba USA title and Village gameplay showed synthetic replacements, with
live off/reload/on and stable native texture atlas comparisons. Full scene
coverage and third-party artwork packs were not claimed; the first consumer
needs a framework pin update and runtime rebuild, without a game C regen
for the HD texture feature itself.
