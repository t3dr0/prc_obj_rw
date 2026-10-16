# prc_obj_rw

Convert Wavefront OBJ to PRC / 3D PDF and back, using [nanoPRC](https://github.com/mvrhel/nanoPRC).

PRC (ISO 14739) is the 3D format embedded in 3D PDF files. This repository holds two command-line
tools built on nanoPRC's public C API:

| Tool | Direction | What it does |
|---|---|---|
| `nano_prc_obj_import` | OBJ → PRC and PDF | Reads an OBJ and its MTL materials, and writes a `.prc` file and a minimal 3D PDF |
| `nano_prc_obj_export` | PRC or PDF → OBJ | Extracts tessellated geometry, materials and textures as OBJ + MTL + PNG |

## obj_import

```
nano_prc_obj_import <input.obj> [output.prc] [output.pdf]
                    [--triangles] [--no-normals] [--no-materials] [--jitter F]
```

- Polygons with more than three corners are triangulated by ear clipping, which handles non-convex
  polygons correctly.
- Each OBJ object (or group, if the file has no objects) becomes a part in the PRC model tree, with
  one tessellation per material.
- MTL diffuse colour (`Kd`), opacity (`d`) and shininess (`Ns`) become the PRC material. A `map_Kd`
  image in PNG or JPEG becomes a diffuse texture.
- Geometry is written as PRC compressed tessellation where possible. Textured parts, and parts with a
  vertex shared by disconnected triangle fans, are written as plain triangles, because compressed
  tessellation cannot carry textures and some Adobe Acrobat builds misread that vertex case.
- Each part is moved by a tiny offset of its own (`--jitter`, default 1e-4 of the model diagonal).
  Some arrangements of separate parts, such as parts along one axis, otherwise give an empty model
  tree in Adobe Acrobat.

OBJ parsing uses [fast_obj](https://github.com/thisistherk/fast_obj) (MIT), pinned to `v1.3` as a
git submodule.

## obj_export

```
nano_prc_obj_export <input.pdf|input.prc> <output.obj>
```

Writes OBJ, MTL and PNG texture files, de-duplicating identical textures.

## Building

nanoPRC is required. Build either way:

**Inside a nanoPRC checkout**, where this repository is a submodule, nanoPRC's own CMake build
includes it.

**On its own**, pointing at a nanoPRC checkout:

```
git clone --recursive https://github.com/t3dr0/prc_obj_rw
cmake -S prc_obj_rw -B build -DNANOPRC_SOURCE_DIR=/path/to/nanoPRC
cmake --build build --config Release
```

Both tools are C99. Before opening a pull request, run the C99 check:

```
python tests/check_c99.py --nanoprc /path/to/nanoPRC --build-dir build
```

## Licence

Copyright (C) 2026 Ian Curington, Thames 3D Research.

prc_obj_rw is licensed under the GNU Affero General Public License v3.0; see [LICENSE](LICENSE).
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

### Commercial licensing

A commercial licence is available on request for uses where the AGPLv3 is not suitable. Contact
info@thames3.org.

prc_obj_rw links nanoPRC, which is licensed separately by its owner. A closed-source product built
with prc_obj_rw also needs a commercial nanoPRC licence; see nanoPRC's README.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Contributions require agreement to [CLA.md](CLA.md).
