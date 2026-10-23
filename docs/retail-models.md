# Retail (MD21) models from local files

Build option `WOWDECOMP_RETAIL_MODELS` (off by default) lets the client load models authored for
newer versions of the game, the chunked **MD21** format (M2 versions 272 to 274), together with
their external **.anim** files and shared **.skel** skeletons. Everything is read from the client's
own data: loose files and patch MPQs, in the usual priority order. Nothing is downloaded and
nothing is converted on disk: each file is prepared for the 3.3.5 engine in memory when it opens.

The model adapter (`Engine/Source/Retail/RetailModel.cpp`) is taken from the Esteria fork of
wowdecomp (STRHercules, `Esteria` branch, commits 24cd913, 003789a, 1f8a3e2, 3611992). Its CASC/CDN
source is replaced by `Engine/Source/Retail/LocalSource.cpp`; character customization, body
sliders, the CDN downloader and the item importers are not included.

## Build

1. In CMake, tick `WOWDECOMP_RETAIL_MODELS`, Configure, Generate, and rebuild.
2. Re-translate the Vulkan shaders with the larger vertex constant file and deploy them:

   ```
   python Tools/VulkanShaders/translate_shaders.py --client <client folder> --retail-palette --deploy
   ```

   Retail rigs draw with bigger bone palettes than 3.3.5 models. With this option the Vulkan device
   offers 512 vertex constant rows instead of 256. Stock models pick the same skin profiles either
   way, so old content is unaffected; Retail models with large palettes need the re-translated set.
   D3D9 and OpenGL keep the old limit, so run with Vulkan.

## Adding a Retail model

1. Export the model with wow.export (or similar) **with its original files**: the `.m2`, its
   `00.skin` (and other skins), its `.skel` if it has one, its `.anim` files and its `.blp`
   textures. Keep them at their listfile paths.
2. Put them in a patch MPQ or loose in `Data`. Point a CreatureModelData row (or any other model
   reference) at the `.m2` path as usual (`Creature\Foo\Foo.mdx` style works, as for stock models).
3. Give the client a FileDataID -> path table. Either works, and both can be present:
   - **`TextureFilePath.db2` and `ModelFilePath.db2`** in `DBFilesClient` (loose or in an MPQ), the
     tables WarcraftXL's wxl-db2 uses. If your client already has them, nothing else is needed. They
     are read once at the first Retail reference and searched in place (WDC1 layout).
   - **`Data\Retail\listfile.csv`** beside the executable, rows `FileDataID;path`. Its rows win
     over the DB2 tables. The community listfile works; a trimmed one loads faster:

     ```
     python Tools/RetailModels/make_listfile.py --listfile community-listfile.csv --export <export folder> --out <client>\Data\Retail\listfile.csv
     ```

     `WOW_RETAIL_LISTFILE` overrides the location.

### What needs a path table

Inside an MD21 model every dependency is a FileDataID.

| Reference | Without a path table entry |
|---|---|
| Skins (`00.skin` ...) | Falls back to `<model name>00.skin` next to the model |
| External animations | Falls back to `<model name><id>-<variation>.anim` next to the model |
| The model's skeleton | Falls back to `<model name>.skel` next to the model |
| Parent skeletons (SKPD) | **Needs an entry** |
| Textures | **Needs an entry** (they are opened as `retail/<FileDataID>.blp`); a missing one draws green |

A model is recognised by its header (`MD21`), whatever its path, so a stock 3.3.5 model with the
same name keeps loading as before.

## What is supported

- MD21 models, versions 272 to 274, including models without bones of their own (`SKID`): the
  `.skel` is spliced in, and parent skeletons (`SKPD`) are merged.
- External animations, both the flat format and the chunked `AFM2` / `AFSB` / `AFSA` format.
- Skins with more than 65535 indices (the high bits of the index start in the level field).
- Retail blend modes 7 to 14, mapped to the closest legacy mode.
- Flying creatures: FlyStand / FlyFly are swapped into the slots the 3.3.5 engine plays in flight.

## Limitations

- **Particle emitters are omitted.** Retail emitters use a layout the legacy emitter cannot read;
  the mesh, ribbons and animation are unaffected.
- **Glow layers are dropped.** Retail shader 0x8003 (a base layer plus an additive glow texture)
  draws only its base layers; the glow needs its own pixel shader, which is not ported.
- **Other multi-texture Retail shaders** draw with their first texture.
- Textures must be BLP2 version 1, the client's native format.
- No character customization: Retail player bodies load as models, but the customization system,
  equipment on Retail bodies and the race tables are not part of this.

## Troubleshooting

`Logs\RetailModels.log` names every file that failed and why ("FileDataID 123: cannot open ...",
"Model needs its SKEL skeleton file", "Supported Retail M2 versions are 272-274", ...). The first
lines say how many paths were loaded from listfile.csv and the FilePath DB2s. A reference that
cannot be found is logged once with the FileDataID or the paths that were tried.
