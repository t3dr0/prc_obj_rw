# Third-party notices

prc_obj_rw includes or depends on the following third-party components.

## fast_obj

- Purpose: Wavefront OBJ and MTL parsing, used by `obj_import`.
- Location: `thirdparty/fast_obj` (git submodule, pinned to tag `v1.3`).
- Source: https://github.com/thisistherk/fast_obj
- Licence: MIT

```
MIT License

Copyright (c) 2018 thisistherk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## stb_image_write

- Purpose: PNG writing for texture export, used by `obj_export`.
- Location: `obj_export/src/stb_image_write.h` (v1.16, vendored).
- Source: https://github.com/nothings/stb
- Licence: public domain (Unlicense) or MIT, at the user's choice. The full text is at the end of the
  header file.

## nanoPRC

- Purpose: PRC reading and writing; both tools link it.
- Not included in this repository. Supplied as a separate checkout (see README).
- Source: https://github.com/mvrhel/nanoPRC
- Licence: GNU AGPL v3, with a commercial licence available from its owner.
