# HD texture packs

The **HD Texture Packs** mod loads compatible DuckStation texture PNGs and can
dump original textures encountered during gameplay. It is disabled by default.
Use **OpenGL** to display replacements. Software and Vulkan can collect dumps
but continue to display the original artwork. Tomba USA (`SCUS-94236`) is the
first integrated game target; game projects can supply their own manifest for
the same `psx.hd-textures` plugin.

## Install a pack

1. Open the launcher's **Mods** page and enable **HD Texture Packs**.
2. Under **Texture pack folder**, click **Open folder**. On first use the
   launcher creates `mods/texture-packs/<game-id>` beside the executable, with
   empty `replacements` and `dumps` directories.
3. Copy the pack's replacement PNGs into `replacements`, preserving their
   filenames. Subdirectories are supported. If the download already contains
   `replacements`, copy that directory's contents into this one.
4. Leave **Load replacements** enabled and **Dump textures** disabled.
5. Select **OpenGL** in Settings, then click **Play**.

**Change folder** selects another pack root. Empty folders are valid; their
`replacements` and `dumps` directories are created when the pack is configured.
You can also select a pack's `replacements` directory directly, with `dumps`
beside it. Keep editable packs outside `mods/bundled`, which builds regenerate.

## Dump and edit textures

1. Enable **HD Texture Packs**, disable **Load replacements**, and enable
   **Dump textures**.
2. Click **Play** and visit the scenes you want to capture. Original texture
   PNGs appear under `dumps` as the game uses them.
3. Exit to the launcher. Copy selected PNGs into `replacements`, then edit or
   enlarge them while keeping the filenames, aspect ratio, and an integer
   size multiplier.
4. Enable **Load replacements**, disable **Dump textures**, and click **Play**
   using OpenGL to check the artwork.

The switches apply on **Play**. Restart after adding or editing pack files so
the runtime scans them again. Disabling the mod restores the original artwork.
The plugin does not write replacement pixels into native VRAM or change the
save namespace.

## Compatibility and diagnostics

Pack artwork is supplied separately. Matching depends on native game pixels
and palettes; a different region or revision may not match. PNG is supported,
with a maximum of 8192 pixels per side, 64 MiB encoded, and 64 MiB decoded RGBA.
Unsupported identities, configuration features, or images fall back to the
original artwork. After loading a savestate, upload-based replacements need
fresh texture uploads from the game before they can match again; page-based
matching remains available. See [DuckStation texture format notes](DUCKSTATION_TEXTURE_FORMAT.md)
for the exact supported filenames, hashing, alpha behavior, and limitations.

Development builds expose the [TCP `hd_textures` command](TCP_COMMANDS.md).
It reports the selected root, switches, backend support, pack size, and matched,
ready, and applied draw counts. Optional integer fields `replacements: 0|1`,
`dump: 0|1`, and `reload: 1` control an already configured pack during a test
session. Use `present_shot` to capture the composed OpenGL window; native VRAM
screenshots retain the original artwork.
