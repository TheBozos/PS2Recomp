# Native packs: a game's models and textures converted offline

A PS2 game keeps its 3D models as prebuilt VIF / GIF packets (positions, strips, GIF tags, register blocks) and its
textures as GS rasters. A native port draws them without reading a packet while the game runs: a converter turns the
user's own disc files into a **mesh pack** and a **texture pack** once, offline, into the build tree (never committed),
and the runtime looks objects and textures up there by keys the game's own code gives it at a few hooks.

The fork supplies everything that is not the game's: the pack containers, their writers (Python) and loaders (C++),
the object store, the draw representation, the per-view ordered submission with the GS state, the CPU clip against
the microprograms' clip volume, the live diagnostic toggles, and the PS2 texture decoders. A port supplies its format
reader (which bytes of its files are an object), its hook addresses and its key scheme. Twisted Metal: Black (TMB) is
the worked example throughout.

| Piece | Where |
| --- | --- |
| Mesh pack writer / reader (`PS2XMSH1`) | `tools/native_pack/mesh.py` |
| Texture pack writer / reader (`PS2XTEX1`), texel unpacking | `tools/native_pack/texture.py` |
| VIF codes, DMA chains in a linked image, GS A+D register blocks, ELF bytes | `tools/native_pack/ps2.py` |
| Mesh pack loader, object store, loaded images | `ps2xRuntime/include/runtime/native/native_mesh_pack.h` (`ps2x::native::packs`) |
| Draws, view order, GS state, clipping, live toggles | `ps2xRuntime/include/runtime/native/native_mesh_draw.h` (`ps2x::native`) |
| Texture pack loader, CSM1 palettes, TEXA alpha, decode | `ps2xRuntime/include/runtime/native/native_texture_pack.h` (`ps2x::native::packs`) |

## The mesh pack

One **file** per game file the game loads models from, named as the port's runtime will name it when the game loads
it (TMB: the disc file's name, `JUNKYARD.NGP`), with the image's size and CRC32 as the game has it in memory: at
runtime the loaded bytes are checked against them, so a pack made from another disc or version is never used. One
**object** per model, keyed by its offset in that image. An object holds:

- `kind` and `attributes`: the port's (a type number and a fixed block of fields the runtime reads back by offset:
  TMB's are the hierobject's flags, bounding sphere, texture numbers and the pool object of a pool-less object);
- `positions`: x, y, z, w floats (the pools the packet unpacks, inline points);
- `strips`: PS2 triangle strips as the VU program reads them: GIF PRIM, GIF flags (PRE, EOP), a material word (TMB:
  the GIF tag's word 3, the texture header), the texture slot it selects (-1 keeps the current one) and 16-byte
  vertices (position index, raw normal bytes, flags: inline point / ADC / expanded corner, RGBA, raw s and t, a spare
  u16 for the port);
- `sections`: tagged byte blocks for whatever else the port's programs need (TMB: `AUX ` morph deltas, `SRCS` where
  each pool sits in the image, `PRMS` the environment map's parameters, `SKIN` bones and weights).

Per file, **register blocks**: GS register writes recorded at their offset in the image (an A+D block the game sends
with its draws: TMB's GS context blocks, ALPHA_1 / TEST_1 / ZBUF_1), looked up by the address the game hands over.

The byte layout is in `tools/native_pack/mesh.py`'s docstring. `MeshPack::load` checks the magic and every count
against the file's size.

## The texture pack

Per file the port names (TMB: `JUNKYARD.TEX`, `GAME.RTX`, `FONT`), its records in the order the port's key scheme
counts them: PSM, width, height and the texels unpacked (indexed formats one byte per texel, CT32 / CT24 four bytes,
CT16 two). `TexturePack::file(name)` and `nth(files, n)` (the n-th record across a list of files) find records;
`csm1Palette` builds an indexed texture's palette from the port's CLUT lookup (CSM1 order, CSA for T4) and
`decodeRecord` makes the RGBA image a TEX0 of that size shows (CT24 / CT16 alpha from TEXA as the GS does), ready for
`Renderer::draw(..., texture)`.

## What a port implements

1. **The converter** (Python, in the port): read the game's containers into the images the game has in memory, find
   the objects, walk each one's packet (`ps2.dma_chain` / `chain_stream` for a DMA chain linked at a fixed address,
   `ps2.vif_codes` for its VIF codes: UNPACKs to VU addresses, MSCAL / MSCNT), and emit positions and strips through
   `mesh.MeshPackWriter`; `ps2.find_gs_register_blocks` finds the A+D blocks. Textures: `texture.record` /
   `texture.TexturePackWriter`. The port's build script runs it into the build tree.
2. **Where files load** (a hook on the game's loader): `LoadedImages::bind(pack, rdram, name, base, size, replace)`.
   An EE address then resolves with `LoadedImages::find` (the last image holding it) to a file and an offset, and
   `MeshFile::object(offset)` / `MeshFile::block(offset)` find the object and the register block.
3. **The per-object hook** (the game's call that builds an object's packet): the EE state the packet header would
   carry (matrices, colours, light directions, fog, morph weights, bones: what the VU program reads), the program the
   game picks for the object, and from them the object's `MeshDraw`: the object-to-screen matrix, the clip volume
   when the program clips, the batches (one per material change: TEX0 and the GS registers a strip's register block
   sets) with the strip vertices shaded and projected as the program does. The shading is the port's: it is the
   meaning of its microprograms.
4. **The game's order**: a `MeshViewList<D>` (`D` = `MeshDraw` or the port's type derived from it) per view being
   built: opaque objects pushed in traversal order; an alpha object kept `pending` until the game links it into its
   depth-sorted list (`link(bucket)` with the bucket the game computes); at the view's kick
   `MeshSubmitter::submitInOrder` (opaque, then the buckets from the farthest, each bucket's newest first) follows
   the GS state (a draw's context block, then each batch's writes) into each batch's material, cuts triangles to the
   clip volume and to w > 0, and calls `Renderer::draw`. `MeshSubmitter::fogColour` gives FOGCOL, `observer` sees each
   draw and batch in submission order (a port's checks), `live` holds the diagnostic toggles (`MeshLiveToggles::read`
   from a file of words: nodepth, nofog, noclip, flat, opaque, noatest, notex, noguard, dump).
5. **The texture key scheme**: whatever the game's own code gives at its hooks (a texture id as it is set up, the
   n-th record a loader sets up, the VRAM block a palette is loaded to), mapped to pack records.

## TMB, the worked example

| | TMB |
| --- | --- |
| Converter | `tools/tmb_mesh_pack.py`: NGP files (a zip entry, "IE" for "PK"; linked at EE 0x980000, pointers listed in the `.PTR` beside), hierobjects (type 0 nodes whose +4 is their own packet), their DMA chains and VIF codes; `tools/tmb_texture_pack.py`: `.TEX` / `.RTX` records, the font from the ELF |
| Keys | mesh: (NGP file name, obj - load address); textures: texture id (`texmSetupTexture`'s n-th call is the n-th record of the level's `.TEX` files), palette by VRAM block (`texmLoadResTexture`'s n-th load), the font's block (`fontInit`) |
| File binding | `fileOnlyNgpFile` / `fileAddNgpFile` (address, size), the name from `fileReadf` / `zipInflateAll`'s destination |
| Object hook | `pktAddVu1ObjAsm` (hierTraverseAsm's call; s5 the object, s7 the instance, VU0 vf1-3 / vf6 the matrix) |
| Programs | rigid strips 0xcc8 / 0xd08 / 0xce8 / 0x1700, resume 0xc78, sprites 0x3000, ribbons 0x3270, environment map 0x1db8, skinned 0x70 / 0x90 / 0xb0, morph 0xd28 / 0xd48 / 0xd68 (`runtime/tmb_meshes.cpp`, `runtime/tmb_vu1_shading.h`) |
| Order | opaque at the hook (its opaque return address), alpha linked at `pktLinkAlphaObjAsm` (bucket vf30.x / 8 + 1 of 129), kick at `hierCheckVifComplete` (hier__Fi) and `hier2pFlushDma` (split screen), unkicked views at the frame's end |
| Register blocks | the GS context blocks (FLUSH, DIRECT, A+D of ALPHA_1 / TEST_1 / ZBUF_1) the traversal hands each header (s4) |
| Checks | `TMB_NATIVE_VIF=1 TMB_NATIVE_MESHES_CHECK=1`: the stream is read again and compared with the hook's draws (geometry per strip, VU 0-21, order, materials) |

What stays in TMB: the NGP and texture file parsing, the meaning of each VU1 setup program and its shading, the hier
hooks and their registers, `texInfo`, the skid marks' object (built each frame from the game's skid list as an
in-memory `MeshObject`).

## A game whose models sit in an archive

The pack does not care how the game stores its files: a port names each image as its runtime will see it loaded.
A game that reads its models out of one archive (Silent Hill 3's `ROOT.OLK`) names each entry (or each loaded chunk)
and binds it where its loader puts it in EE memory; if the game relocates pointers inside a loaded model, the
converter applies the same relocation to the image it hashes (the CRC is of the bytes as loaded), or binds the image
before the relocation runs. Objects are then keyed by their offset in that entry, and the per-object hook is wherever
the game's renderer hands VU1 an object's packet.
