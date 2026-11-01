"""Offline conversion of a PS2 game's 3D models and textures into the native packs the fork's runtime loads
(runtime/native/native_mesh_pack.h, native_texture_pack.h): a port's converter parses its own game's files and emits
objects, strips and texture records through these writers. docs/native_packs.md describes the formats and what a port
supplies.

  mesh.py     the mesh pack writer and reader (PS2XMSH1)
  texture.py  the texture pack writer and reader (PS2XTEX1), PS2 texel formats unpacked
  ps2.py      PS2 data helpers a converter needs: VIF codes, DMA chains in a linked image, GS A+D register blocks, ELF
              segments

A port's tool imports them with the fork's tools directory on sys.path:

  sys.path.insert(0, os.path.join(<repo>, 'PS2Recomp', 'tools'))
  from native_pack import mesh, texture, ps2
"""
