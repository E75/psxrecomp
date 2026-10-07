# DuckStation texture-pack interoperability

psxrecomp implements a bounded subset of modern DuckStation texture packs.
The parser, matcher, cache and dumper are independently written here. The
reference version is upstream commit
`697599c47a646a6cfcc4d246018adf4904d55001`, observed on 2026-10-06.
This pin matters for palette-range and alpha behavior.

## Provenance

At the reference pin, DuckStation's
[license](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/LICENSE)
is CC-BY-NC-ND-4.0; its texture-cache and shader files carry the same SPDX
identifier. This project includes no DuckStation implementation, translated
implementation, patch, or binary. Its own code retains the project's
PolyForm Noncommercial terms. Reading format behavior does not provide a
license to incorporate DuckStation's source. Pack artwork has its own rights;
users supply it separately.

The hash implementation comes directly from
[xxHash v0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3), under
[BSD-2-Clause](https://github.com/Cyan4973/xxHash/blob/v0.8.3/LICENSE).
The unmodified `runtime/third_party/xxhash.h` has SHA-256
`17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.
Its license ships as `runtime/licenses/xxhash-NOTICES.txt`.
The [v0.8.0 release](https://github.com/Cyan4973/xxHash/releases/tag/v0.8.0)
made XXH3 output stable across future versions. Thus the upstream v0.8.3
implementation supplies the unseeded XXH3 behavior used by DuckStation's
vendored v0.8.0 without importing any emulator code.

## Supported files

Choose a game/package folder containing `replacements/` and `dumps/`, or choose
`replacements/` itself. Replacement PNGs can live in subdirectories. See the
[player workflow](HD_TEXTURE_PACKS.md) for dumping, editing and reloading.
DuckStation documents its serial-based folders and authoring process in its
[texture replacement wiki](https://github.com/stenzek/duckstation/wiki/Texture-Replacement).

Paletted filename stems follow this grammar:

```text
<kind>-<mode>-<sourceHash>-<paletteHash>-<sourceWords>x<sourceHeight>-<offsetX>-<offsetY>-<width>x<height>-P<min>-<max>
kind = texupload | texpage
mode = P4 | P8 | STP4 | STP8
```

Direct stems omit the palette fields:

```text
<kind>-<mode>-<sourceHash>-<sourceWords>x<sourceHeight>-<offsetX>-<offsetY>-<width>x<height>
mode = C16 | STC16
```

Append `.png`. Hash fields contain exactly 16 hexadecimal digits; either case
is accepted. Dimensions, offsets and palette endpoints are decimal. Source
width is in native VRAM words. Rectangle width and X offset are expanded
texels: four per word for P4, two for P8, one for C16. Height and Y offset are
rows. A page's source size is 64x256, 128x256 or 256x256 words respectively.
These definitions follow the
[upstream filename documentation](https://github.com/stenzek/duckstation/wiki/Texture-Replacement).

The loader validates nonzero sizes, native VRAM bounds, word-aligned horizontal
rectangles, page bounds and palette endpoints. The replacement PNG's actual
size controls its independent X/Y scale; integer and equal-axis scale factors
are not required. The renderer applies the declared native rectangle's origin
and extent when mapping UVs.

## Hash compatibility

Both texture and palette identities use unseeded XXH3-64 over packed native
PSX words. Words are serialized low byte first. Rectangles concatenate each
row's words without stride padding. `texupload` hashes the complete tracked
post-write source rectangle; `texpage` hashes its declared subrectangle.
Dimensions and palette data do not enter the texture hash.

A full CLUT hashes 16 P4 or 256 P8 words, including bit 15. A P8 CLUT extending
beyond VRAM's right edge hashes only the remaining words, without wrapping.
For reduced `Pmin-max` ranges, the reference implementation hashes the **first
`max-min+1` palette words**, without advancing by `min`; a range whose declared
maximum crosses the edge does not match. It does not revalidate used indices
against the declared range during lookup. psxrecomp preserves this behavior
for interoperability. Changing a palette entry outside that prefix can leave
a replacement matched; use full ranges when authoring new packs here. These
are observations of the pinned
[texture-cache behavior](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_hw_texture_cache.cpp).

## Alpha conventions

The decoder preserves raw RGBA bytes. Ordinary P4/P8/C16 replacements treat
alpha below 128 as cutout and alpha at least 128 as occupied, including black.
ST names encode PSX transparency classification: alpha at most 242 marks
STP, while alpha at least 243 marks an opaque texel. Exact RGBA zero is cutout;
ST black with alpha at least 243 also becomes PSX transparent zero. Alpha zero
with nonzero RGB in an ST image remains a semitransparent texel. Actual blending
still uses the primitive's PSX transparency mode. These rules follow the pinned
[replacement merge shader](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_hw_shadergen.cpp).

When both ordinary and ST records match, psxrecomp prefers the primitive's
convention; if only the other convention exists it can still supply the image.
Multiple matching records with the same preferred convention fall back to
native rendering. This deterministic policy avoids dependence on directory
enumeration or unordered-map order.

The dumper writes rounded 5-bit-to-8-bit native RGB, full CLUT ranges, alpha
zero for native color zero, alpha 255 for occupied ordinary texels, and alpha
128 for occupied STP texels in ST dumps. The pinned upstream dump produces
143 in that last case because of an incidental mask expression; both values
have the same replacement classification. Dumps here are compatible authoring
inputs, not a promise of byte-identical upstream dump PNGs. Native conversion
was checked against the pinned
[GPU helpers](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_helpers.h).

## Current compatibility limits

The module accepts a single replacement covering the complete draw query.
It supports ordinary unchanged uploads and nonwrapping page subrectangles.
Every touched upload loses its identity after a partial overwrite; it does
not reconstruct split or coalesced upload histories. A same-session pack reload
can copy intact upload identities into the newly indexed pack. Loading a
savestate clears them; page matching remains available, while upload matching
needs subsequent guest uploads. The module's rectangle and query caches must
be invalidated on every native VRAM write.

The following cases use native rendering or report an unsupported-file/option
diagnostic:

- Wrapped UV intervals, horizontally wrapped pages and wrapped uploads.
- Draws requiring composition from several replacement images.
- `vram-write-` XXH3-128 images, JPEG/WebP payloads and legacy unnamed layouts.
- Copy-to-upload conversion, upload coalescing and split-upload identities.
- Other `config.yaml` behavior options, including replacement bilinear filtering.

`config.yaml` supports only a bounded `Aliases:` mapping with literal or quoted
filename keys and relative PNG paths. A single-line literal/folded block value
is accepted. Escaped strings, multiline values, anchors and general YAML
features are outside the subset. Absolute paths and `..` traversal are
rejected. A direct canonical replacement takes precedence over its alias.
Unknown top-level options are reported by name and are not applied.

## Bounds and lifecycle

The loader hard-fails above 262144 candidate files or entries, scans at most
1048576 directory entries and limits recursive nesting to 16 levels. Duplicate
identities are marked ambiguous. Matching indexes upload hashes and page
geometry, with 256 rectangle hashes and 256 positive/negative query results.
Native page/CLUT writes invalidate affected cached queries. A query requiring
over 4096 candidate page geometries or more than 4 MiB of new hashing reports
an error and renders natively.

One low-priority background worker performs PNG decoding and dumping. The
decode queue is bounded to 32, decoded metadata to 512 entries, and decoded
pixels to a default 64 MiB budget. Encoded files are limited to 64 MiB and each
image dimension to 8192 before decoding. Pixel leases survive eviction and
pack destruction. Decoding failure leaves native rendering active.

Dumps use word-aligned draw rectangles, choose `texupload` when an intact
tracked source contains the draw, and otherwise choose `texpage`. The queue
holds at most eight snapshots. A bounded set deduplicates 8192 dump identities
per session; reaching it reports a diagnostic until dump tracking is reset.
PNG writing finishes on pack destruction. A temporary file is published
atomically without replacing an existing final file. Write failures remove
only the created temporary, print an error even on shutdown, and are returned
by the next dump call. All snapshots decode native VRAM rather than replacement
pixels.

## Independent validation

`runtime/tests/test_duckstation_texture_pack.cpp` contains fixed XXH3 vectors
generated with the separate Python xxhash 3.6.0 native wheel. Input word `i`
is `((i * 7919) ^ 0xa51c) & 0xffff`, serialized little endian. Examples:

| Word count | XXH3-64 |
| --- | --- |
| 0 | `2D06800538D394C2` |
| 4 | `3631D2A04EC85311` |
| 8 | `000CA76CB9B329DC` |
| 16 | `9CBCDEA9305D7528` |
| 256 | `0005D0B83A005F4E` |
| 8192 | `C68A00DCCA652920` |

The test also checks literal filenames, strided native rectangles, crop
coordinates, CLUT edges and reduced-prefix behavior, positive/negative cache
invalidation, reload identity copying, duplicate ambiguity, alias diagnostics,
raw alpha thresholds, Unicode folders, background decoding, native dump pixels,
worker shutdown and preservation of existing edited files. Renderer fixtures
exercise the alpha classification and native-VRAM authority separately.
