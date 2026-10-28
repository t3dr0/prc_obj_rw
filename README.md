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
                    [--triangles] [--no-normals] [--no-materials]
                    [--quant-tolerance F]
```

- Polygons with more than three corners are triangulated by ear clipping, which handles non-convex
  polygons correctly.
- Each OBJ object (or group, if the file has no objects) becomes a part in the PRC model tree, with
  one tessellation per material.
- MTL diffuse colour (`Kd`), opacity (`d`) and shininess (`Ns`) become the PRC material, and so do
  the ambient, specular and emissive colours (`Ka`, `Ks`, `Ke`) when the MTL gives them.
- A `map_Kd` image in PNG or JPEG becomes a diffuse texture, placed by the map's `-s` and `-o`
  options and clamped by `-clamp on`. A `map_d` image becomes an opacity mask and a
  `refl -type sphere` image a reflection (at the strength `Ks` gives), each applied after the
  diffuse texture, or on its own when there is none. Other map options are ignored, and map types
  PRC has no place for (bump, specular, emissive, cube reflections) are left out.
- Geometry is written as PRC compressed tessellation, textured or not. `--triangles` writes plain
  triangles instead.
- `--quant-tolerance` sets the compressed encoding's tolerance as a fraction of the bounding-box
  diagonal (default `1e-6`).
- Build against a nanoPRC that includes its pull requests #153, #157 and #168. Older versions can
  write files that Adobe Acrobat shows with an empty model tree, depending on where a section of the
  file happens to end; that is why earlier versions of this tool moved parts slightly and suggested
  trying another tolerance.

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
python tests/check_c99.py --nanoprc /path/to/nanoPRC
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
