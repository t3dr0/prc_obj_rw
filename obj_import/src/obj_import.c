/* Copyright (C) 2026 Ian Curington, Thames 3D Research

    prc_obj_rw is free software: you can redistribute it and/or modify it
    under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or (at
    your option) any later version.

    prc_obj_rw is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
    General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with prc_obj_rw. If not, see <https://www.gnu.org/licenses/>.
*/

/* ====================================================================
 * obj_import: read a Wavefront OBJ (and its MTL) and write it as a .prc
 * file and a minimal 3D-annotated .pdf, through nanoPRC's public write
 * API only (include/prc_api.h).
 *
 * PROTOTYPE. Parsing is done by fast_obj. What this file adds:
 *
 *   - polygons of more than three vertices are triangulated by ear
 *     clipping, which is correct for non-convex polygons where a fan is not;
 *   - the mesh is split into parts, one per OBJ object (or group, when the
 *     file has no objects), and each part into one tessellation per
 *     material, since a PRC representation item carries one material;
 *   - each MTL material becomes the item's material: Kd its colour, d its
 *     opacity, Ns its shininess, and Ka, Ks and Ke its ambient, specular and
 *     emissive colours when the MTL gives them;
 *   - a map_Kd image in PNG or JPEG becomes the item's diffuse texture,
 *     placed by the map's -s and -o options and clamped by -clamp on; a
 *     map_d image becomes an opacity layer and a "refl -type sphere" image a
 *     reflection layer after it, or the item's only texture when there is
 *     no map_Kd.
 *   - vertex colours given as "v x y z r g b", the extension MeshLab,
 *     Blender and obj_export write, become the tessellation's vertex
 *     colours, one per triangle corner. Readers apply the material's Kd on
 *     top of them, as the original PRC did. A part whose vertices are all
 *     white, as fast_obj fills in where a file gives no colour, gets none.
 *
 * fast_obj v1.3 ignores a map line that has options and never reads refl,
 * so the MTL files are read a second time here for those (read_mtl_extras).
 *
 * MTL libraries and texture images are found with model_path_resolve,
 * never by joining the OBJ's folder to the name in the file: a name such
 * as "../../x.png" would reach outside that folder, and one such as
 * "C:\Users\me\x.png", common in OBJ files, is not found as written. A
 * name that is not a plain path down from the OBJ's folder is looked for
 * by its file name in that folder and its subfolders, and the
 * substitution is reported. fast_obj's own file opening goes through the
 * same lookup (open_beside_obj).
 *
 * Geometry goes out as compressed tessellation, textured or not.
 * --triangles writes every part as plain triangles instead.
 *
 * Usage:
 *     nano_prc_obj_import <input.obj> [output.prc] [output.pdf]
 *                         [--triangles] [--no-normals] [--no-materials]
 *                         [--no-colors] [--quant-tolerance F]
 *
 * Output paths default to the input's name with .obj replaced.
 *
 * Not yet done: instancing of repeated parts, units, and the map types
 * PRC has no place for (bump, specular, emissive and cube reflection maps).
 * ==================================================================== */

#include <prc_api.h>
#include <fast_obj.h>

#include "model_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define OBJ_NO_INDEX 0xFFFFFFFFu

/* Quantization tolerance of a compressed tessellation, as a fraction of the
   bounding-box diagonal: the write API's own default. --quant-tolerance
   changes it. */
#define OBJ_IMPORT_QUANT_TOLERANCE 1e-6

/* -------------------------------------------------------------------- */
/* Small growable array of uint32_t                                      */
/* -------------------------------------------------------------------- */

typedef struct
{
    uint32_t *data;
    uint32_t count;
    uint32_t capacity;
} u32_array;

static int
u32_push(u32_array *a, uint32_t v)
{
    if (a->count == a->capacity)
    {
        uint32_t cap = a->capacity ? a->capacity * 2 : 64;
        uint32_t *grown = (uint32_t *)realloc(a->data, (size_t)cap * sizeof(uint32_t));

        if (grown == NULL)
            return -1;
        a->data = grown;
        a->capacity = cap;
    }
    a->data[a->count++] = v;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Triangulation                                                         */
/* -------------------------------------------------------------------- */

/* Twice the signed area of the 2D triangle (a, b, c). */
static double
cross2(const double *a, const double *b, const double *c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

/* Is p inside or on triangle (a, b, c), which has orientation sign `s`? */
static int
point_in_triangle(const double *p, const double *a, const double *b,
    const double *c, double s)
{
    return cross2(a, b, p) * s >= 0.0 &&
           cross2(b, c, p) * s >= 0.0 &&
           cross2(c, a, p) * s >= 0.0;
}

/* Triangulate one polygon of `n` corners. `corner` holds the corners'
   positions in polygon order. Triangles are appended to `out` as triples of
   corner numbers 0..n-1, in the polygon's own winding.

   Ear clipping in the plane the polygon most nearly lies in, chosen from
   its Newell normal. If no ear can be found -- a degenerate or
   self-intersecting polygon -- the remaining corners are fanned, so every
   polygon still produces n-2 triangles. */
static int
triangulate_polygon(const double (*corner)[3], uint32_t n, u32_array *out,
    double (*proj)[2], uint32_t *ring)
{
    double nx = 0.0, ny = 0.0, nz = 0.0, s;
    int ax, ay;
    uint32_t i, m;

    if (n == 3)
        return u32_push(out, 0) || u32_push(out, 1) || u32_push(out, 2);

    for (i = 0; i < n; i++)
    {
        const double *p = corner[i];
        const double *q = corner[(i + 1) % n];

        nx += (p[1] - q[1]) * (p[2] + q[2]);
        ny += (p[2] - q[2]) * (p[0] + q[0]);
        nz += (p[0] - q[0]) * (p[1] + q[1]);
    }

    /* Drop the axis the normal is largest along. */
    if (fabs(nx) >= fabs(ny) && fabs(nx) >= fabs(nz))
    {
        ax = 1; ay = 2; s = nx;
    }
    else if (fabs(ny) >= fabs(nz))
    {
        ax = 2; ay = 0; s = ny;
    }
    else
    {
        ax = 0; ay = 1; s = nz;
    }
    s = (s >= 0.0) ? 1.0 : -1.0;

    for (i = 0; i < n; i++)
    {
        proj[i][0] = corner[i][ax];
        proj[i][1] = corner[i][ay];
        ring[i] = i;
    }

    m = n;
    while (m > 3)
    {
        uint32_t k, found = 0;

        for (k = 0; k < m && !found; k++)
        {
            uint32_t ia = ring[(k + m - 1) % m];
            uint32_t ib = ring[k];
            uint32_t ic = ring[(k + 1) % m];
            uint32_t j;
            int blocked = 0;

            /* Convex corner in the polygon's own orientation. */
            if (cross2(proj[ia], proj[ib], proj[ic]) * s <= 0.0)
                continue;

            for (j = 0; j < m && !blocked; j++)
            {
                uint32_t iv = ring[j];

                if (iv == ia || iv == ib || iv == ic)
                    continue;
                if (point_in_triangle(proj[iv], proj[ia], proj[ib], proj[ic], s))
                    blocked = 1;
            }
            if (blocked)
                continue;

            if (u32_push(out, ia) || u32_push(out, ib) || u32_push(out, ic))
                return -1;
            memmove(&ring[k], &ring[k + 1], (size_t)(m - k - 1) * sizeof(uint32_t));
            m--;
            found = 1;
        }

        if (!found)
        {
            /* No ear: fan what remains rather than drop it. */
            for (k = 1; k + 1 < m; k++)
            {
                if (u32_push(out, ring[0]) || u32_push(out, ring[k]) ||
                    u32_push(out, ring[k + 1]))
                    return -1;
            }
            return 0;
        }
    }
    return u32_push(out, ring[0]) || u32_push(out, ring[1]) || u32_push(out, ring[2]);
}

/* -------------------------------------------------------------------- */
/* One tessellation entry: the faces of one part that use one material   */
/* -------------------------------------------------------------------- */

typedef struct
{
    /* fast_obj indices of each triangle corner, 3 per triangle */
    u32_array p, t, n;
    uint32_t material;          /* fast_obj material index, OBJ_NO_INDEX for none */

    /* Built for the writer */
    double   *positions;
    uint32_t  num_positions;
    double   *normals;
    uint32_t  num_normals;
    double   *tex_coords;
    uint32_t  num_tex_coords;
    uint32_t *tri_indices;
    uint32_t *norm_indices;
    uint32_t *tex_indices;
    uint8_t  *colors;           /* r,g,b per triangle corner, or NULL */
    uint32_t  face_tri_count;
    double    bbox_min[3], bbox_max[3];
    uint8_t  *texture_bytes;
    size_t    texture_size;
    uint32_t  texture_format;

    /* Layers after the main texture: an opacity mask and a reflection */
    prc_api_write_texture_layer layers[2];
    uint8_t  *layer_bytes[2];
    uint32_t  num_layers;
} submesh;

static void
submesh_free(submesh *s)
{
    free(s->p.data); free(s->t.data); free(s->n.data);
    free(s->positions); free(s->normals); free(s->tex_coords);
    free(s->tri_indices); free(s->norm_indices); free(s->tex_indices);
    free(s->colors);
    free(s->texture_bytes);
    free(s->layer_bytes[0]); free(s->layer_bytes[1]);
}

/* Compact one fast_obj attribute stream: every index used by the submesh is
   given a new dense index, in first-use order, and the values are copied
   out as doubles. `dim` is 3 for positions and normals, 2 for UVs. */
static int
compact_stream(const u32_array *idx, const float *src, uint32_t src_count,
    uint32_t dim, double **values_out, uint32_t *count_out, uint32_t **indices_out)
{
    uint32_t *remap = (uint32_t *)malloc((size_t)src_count * sizeof(uint32_t));
    double *values = (double *)malloc((size_t)idx->count * dim * sizeof(double));
    uint32_t *indices = (uint32_t *)malloc((size_t)idx->count * sizeof(uint32_t));
    uint32_t i, used = 0;

    if (remap == NULL || values == NULL || indices == NULL)
    {
        free(remap); free(values); free(indices);
        return -1;
    }
    for (i = 0; i < src_count; i++)
        remap[i] = OBJ_NO_INDEX;

    for (i = 0; i < idx->count; i++)
    {
        uint32_t v = idx->data[i];

        if (remap[v] == OBJ_NO_INDEX)
        {
            uint32_t d;

            remap[v] = used;
            for (d = 0; d < dim; d++)
                values[used * dim + d] = (double)src[v * dim + d];
            used++;
        }
        indices[i] = remap[v];
    }
    free(remap);
    *values_out = values;
    *count_out = used;
    *indices_out = indices;
    return 0;
}

static int
read_file(const char *path, uint8_t **bytes, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *buf;

    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *bytes = buf;
    *size = (size_t)len;
    return 0;
}

/* PNG or JPEG, by signature rather than by file extension. */
static int
image_format(const uint8_t *b, size_t n, uint32_t *format, uint32_t *w, uint32_t *h)
{
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    *w = *h = 0;
    if (n >= 24 && memcmp(b, png_sig, 8) == 0)
    {
        *format = PRC_API_WRITE_TEXTURE_PNG;
        *w = ((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19];
        *h = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23];
        return 0;
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    {
        *format = PRC_API_WRITE_TEXTURE_JPEG;
        return 0;
    }
    return -1;
}

/* Find the texture image the material names as ref, within the OBJ's
   folder (model_path_resolve), read it and check that it is PNG or JPEG.
   `what` names the map in the warnings. */
static int
load_image(const char *obj_path, const char *ref, const char *what, uint8_t **bytes,
    size_t *size, uint32_t *format, uint32_t *w, uint32_t *h)
{
    char note[1024];
    char *path = NULL;
    int r = model_path_resolve(obj_path, ref, &path, note, sizeof(note));

    if (r == MODEL_PATH_NOT_FOUND || r == MODEL_PATH_NO_MEMORY)
    {
        if (r == MODEL_PATH_NOT_FOUND)
            fprintf(stderr, "warning: %s %s; left out\n", what, note);
        return -1;
    }
    if (r == MODEL_PATH_SUBSTITUTED)
        fprintf(stderr, "warning: %s %s\n", what, note);
    if (read_file(path, bytes, size) != 0)
    {
        fprintf(stderr, "warning: %s %s cannot be read, left out\n", what, path);
        free(path);
        return -1;
    }
    if (image_format(*bytes, *size, format, w, h) != 0)
    {
        fprintf(stderr, "warning: %s %s is not PNG or JPEG, left out\n", what, path);
        free(*bytes);
        *bytes = NULL;
        free(path);
        return -1;
    }
    free(path);
    return 0;
}

/* -------------------------------------------------------------------- */
/* What fast_obj does not read from the MTL                              */
/* -------------------------------------------------------------------- */

/* One texture map line: its image, and the options PRC can carry. */
typedef struct
{
    char  *path;            /* the file name as the MTL gives it, resolved
                               when loaded; NULL when there is no such map */
    double scale[2];        /* -s u v; 1 1 when not given */
    double offset[2];       /* -o u v; 0 0 when not given */
    int    clamp;           /* -clamp on */
    int    placed;          /* the line gave -s, -o or -clamp */
} map_line;

/* Per material: which colours the MTL actually gives (fast_obj fills the
   others with zeros, so it cannot tell black from absent), and the maps. */
typedef struct
{
    char    *name;
    int      has_Ka, has_Ks, has_Ke, has_Ns;
    map_line map_Kd, map_d, refl;
} mtl_extra;

typedef struct
{
    mtl_extra *items;
    uint32_t   count;
    uint32_t   capacity;
} mtl_extras;

/* Read one line of any length into *buf, without its line ending. Returns 1
   for a line, 0 at the end of the file, -1 if out of memory. */
static int
read_line(FILE *f, char **buf, size_t *cap)
{
    size_t len = 0;
    int c;

    if (*buf == NULL)
    {
        *cap = 256;
        *buf = (char *)malloc(*cap);
        if (*buf == NULL)
            return -1;
    }
    while ((c = fgetc(f)) != EOF && c != '\n')
    {
        if (len + 1 >= *cap)
        {
            char *grown = (char *)realloc(*buf, *cap * 2);

            if (grown == NULL)
                return -1;
            *buf = grown;
            *cap *= 2;
        }
        (*buf)[len++] = (char)c;
    }
    if (c == EOF && len == 0)
        return 0;
    if (len > 0 && (*buf)[len - 1] == '\r')
        len--;
    (*buf)[len] = '\0';
    return 1;
}

static const char *
skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* If the line at p starts with the keyword kw followed by a blank or the end
   of the line, return what follows it; otherwise NULL. */
static const char *
after_keyword(const char *p, const char *kw)
{
    size_t n = strlen(kw);

    p = skip_blanks(p);
    if (strncmp(p, kw, n) != 0 || (p[n] != ' ' && p[n] != '\t' && p[n] != '\0'))
        return NULL;
    return skip_blanks(p + n);
}

/* A copy of `base` followed by the rest of the line at p without trailing
   blanks: how fast_obj reads names and file names, spaces included. */
static char *
rest_of_line(const char *base, const char *p)
{
    size_t nb = strlen(base), n;
    char *out;

    p = skip_blanks(p);
    n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    if (n == 0)
        return NULL;
    out = (char *)malloc(nb + n + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, base, nb);
    memcpy(out + nb, p, n);
    out[nb + n] = '\0';
    return out;
}

/* Read up to `max` numbers at p into v (if not NULL); returns how many. */
static int
read_numbers(const char **p, double *v, int max)
{
    int k = 0;

    while (k < max)
    {
        const char *s = skip_blanks(*p);
        char *e;
        double x = strtod(s, &e);

        if (e == s || (*e != ' ' && *e != '\t' && *e != '\0'))
            break;
        if (v != NULL)
            v[k] = x;
        k++;
        *p = e;
    }
    return k;
}

/* Skip one word at p. */
static void
skip_word(const char **p)
{
    const char *s = skip_blanks(*p);

    while (*s != '\0' && *s != ' ' && *s != '\t')
        s++;
    *p = s;
}

/* Parse the rest of a map line: options, then the file name. Only -s, -o,
   -clamp and -type are kept; the others are skipped with their arguments.
   `type` receives the -type value's first letters ("sphere" or "cube..."),
   for refl. Returns -1 if there is no file name. */
static int
parse_map(const char *p, const char *base, map_line *m, char *type, size_t type_size)
{
    m->scale[0] = m->scale[1] = 1.0;
    m->offset[0] = m->offset[1] = 0.0;
    m->clamp = 0;
    m->placed = 0;
    if (type != NULL && type_size > 0)
        type[0] = '\0';

    for (;;)
    {
        const char *o = skip_blanks(p);
        const char *v;
        double xyz[3];

        if (*o != '-')
            break;
        if ((v = after_keyword(o, "-s")) != NULL || (v = after_keyword(o, "-o")) != NULL)
        {
            int is_scale = (o[1] == 's');
            int k;

            p = v;
            k = read_numbers(&p, xyz, 3);
            if (k >= 1)
            {
                double *dst = is_scale ? m->scale : m->offset;

                m->placed = 1;
                dst[0] = xyz[0];
                /* One number scales or moves u only; v keeps its default. */
                if (k >= 2)
                    dst[1] = xyz[1];
            }
        }
        else if ((v = after_keyword(o, "-t")) != NULL)
        {
            p = v;
            read_numbers(&p, NULL, 3);
        }
        else if ((v = after_keyword(o, "-clamp")) != NULL)
        {
            m->clamp = (strncmp(v, "on", 2) == 0);
            m->placed = 1;
            p = v;
            skip_word(&p);
        }
        else if ((v = after_keyword(o, "-type")) != NULL)
        {
            size_t k = 0;

            while (type != NULL && k + 1 < type_size && v[k] != '\0' && v[k] != ' ' && v[k] != '\t')
            {
                type[k] = v[k];
                k++;
            }
            if (type != NULL && type_size > 0)
                type[k] = '\0';
            p = v;
            skip_word(&p);
        }
        else if ((v = after_keyword(o, "-mm")) != NULL)
        {
            p = v;
            read_numbers(&p, NULL, 2);
        }
        else if ((v = after_keyword(o, "-blendu")) != NULL || (v = after_keyword(o, "-blendv")) != NULL ||
                 (v = after_keyword(o, "-cc")) != NULL || (v = after_keyword(o, "-bm")) != NULL ||
                 (v = after_keyword(o, "-boost")) != NULL || (v = after_keyword(o, "-texres")) != NULL ||
                 (v = after_keyword(o, "-imfchan")) != NULL)
        {
            p = v;
            skip_word(&p);
        }
        else
        {
            /* Not a known option: take it as the start of the file name. */
            break;
        }
    }

    free(m->path);
    m->path = rest_of_line(base, p);
    return m->path != NULL ? 0 : -1;
}

static void
mtl_extras_free(mtl_extras *x)
{
    uint32_t i;

    for (i = 0; i < x->count; i++)
    {
        free(x->items[i].name);
        free(x->items[i].map_Kd.path);
        free(x->items[i].map_d.path);
        free(x->items[i].refl.path);
    }
    free(x->items);
}

static const mtl_extra *
mtl_extras_find(const mtl_extras *x, const char *name)
{
    uint32_t i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < x->count; i++)
        if (x->items[i].name != NULL && strcmp(x->items[i].name, name) == 0)
            return &x->items[i];
    return NULL;
}

/* Read one MTL file for what fast_obj leaves out. A missing file is not an
   error here: fast_obj has already reported on the materials it found. Map
   file names are kept as the MTL gives them, and resolved when loaded. */
static int
read_mtl_file(const char *path, mtl_extras *x)
{
    FILE *f = fopen(path, "rb");
    char *line = NULL;
    size_t cap = 0;
    mtl_extra *cur = NULL;
    int r;

    if (f == NULL)
        return 0;
    while ((r = read_line(f, &line, &cap)) == 1)
    {
        const char *v;
        char type[16];

        if ((v = after_keyword(line, "newmtl")) != NULL)
        {
            if (x->count == x->capacity)
            {
                uint32_t c = x->capacity ? x->capacity * 2 : 16;
                mtl_extra *grown = (mtl_extra *)realloc(x->items, (size_t)c * sizeof(mtl_extra));

                if (grown == NULL)
                {
                    r = -1;
                    break;
                }
                x->items = grown;
                x->capacity = c;
            }
            cur = &x->items[x->count++];
            memset(cur, 0, sizeof(*cur));
            cur->name = rest_of_line("", v);
        }
        else if (cur == NULL)
            continue;
        else if (after_keyword(line, "Ka") != NULL)
            cur->has_Ka = 1;
        else if (after_keyword(line, "Ks") != NULL)
            cur->has_Ks = 1;
        else if (after_keyword(line, "Ke") != NULL)
            cur->has_Ke = 1;
        else if (after_keyword(line, "Ns") != NULL)
            cur->has_Ns = 1;
        else if ((v = after_keyword(line, "map_Kd")) != NULL)
            parse_map(v, "", &cur->map_Kd, NULL, 0);
        else if ((v = after_keyword(line, "map_d")) != NULL)
            parse_map(v, "", &cur->map_d, NULL, 0);
        else if ((v = after_keyword(line, "refl")) != NULL)
        {
            if (parse_map(v, "", &cur->refl, type, sizeof(type)) == 0 && strcmp(type, "sphere") != 0)
            {
                fprintf(stderr, "warning: material %s: only \"refl -type sphere\" is supported, "
                        "reflection map left out\n", cur->name ? cur->name : "(unnamed)");
                free(cur->refl.path);
                cur->refl.path = NULL;
            }
        }
    }
    free(line);
    fclose(f);
    return r < 0 ? -1 : 0;
}

/* Find the OBJ's mtllib lines and read each library, found within the OBJ's
   folder by model_path_resolve. This pass reports a library found by name or
   not found; fast_obj, reading the same lines, finds the same file quietly. */
static int
read_mtl_extras(const char *obj_path, mtl_extras *x)
{
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    int r, rc = 0;

    f = fopen(obj_path, "rb");
    if (f == NULL)
        return 0;
    while ((r = read_line(f, &line, &cap)) == 1)
    {
        const char *v = after_keyword(line, "mtllib");
        char note[1024];
        char *ref, *lib = NULL;
        int found;

        if (v == NULL)
            continue;
        ref = rest_of_line("", v);
        if (ref == NULL)
            continue;
        found = model_path_resolve(obj_path, ref, &lib, note, sizeof(note));
        free(ref);
        if (found == MODEL_PATH_NO_MEMORY)
        {
            rc = -1;
            break;
        }
        if (found == MODEL_PATH_NOT_FOUND)
            fprintf(stderr, "warning: mtllib %s\n", note);
        else
        {
            if (found == MODEL_PATH_SUBSTITUTED)
                fprintf(stderr, "warning: mtllib %s\n", note);
            if (read_mtl_file(lib, x) != 0)
                rc = -1;
            free(lib);
        }
    }
    if (r < 0)
        rc = -1;
    free(line);
    fclose(f);
    return rc;
}

/* fast_obj's file access, routed through model_path_resolve. fast_obj opens
   the OBJ by the path it was given, and every other file (its mtllib
   libraries) as the OBJ's folder followed by the name in the file, with the
   separators made the system's own; that folder is taken off again here and
   the name resolved like any other. */
typedef struct
{
    const char *obj_path;
    size_t      folder_len;      /* the OBJ path's folder part, with its separator */
} obj_files;

static void *
open_beside_obj(const char *path, void *user)
{
    const obj_files *of = (const obj_files *)user;
    char *found = NULL;
    FILE *f;
    size_t k;

    if (strcmp(path, of->obj_path) == 0)
        return fopen(path, "rb");
    for (k = 0; k < of->folder_len; k++)
    {
        char a = path[k], b = of->obj_path[k];

        if (a == '\\')
            a = '/';
        if (b == '\\')
            b = '/';
        if (a != b)
            return NULL;
    }
    if (model_path_resolve(of->obj_path, path + of->folder_len, &found, NULL, 0) < 0)
        return NULL;
    f = fopen(found, "rb");
    free(found);
    return f;
}

static void
close_file(void *file, void *user)
{
    (void)user;
    fclose((FILE *)file);
}

static size_t
read_bytes(void *file, void *dst, size_t bytes, void *user)
{
    (void)user;
    return fread(dst, 1, bytes, (FILE *)file);
}

static unsigned long
size_of_file(void *file, void *user)
{
    FILE *f = (FILE *)file;
    long here = ftell(f), end;

    (void)user;
    if (here < 0 || fseek(f, 0, SEEK_END) != 0)
        return 0;
    end = ftell(f);
    fseek(f, here, SEEK_SET);
    return end < 0 ? 0 : (unsigned long)end;
}

/* fast_obj_read, with every file opened through open_beside_obj. */
static fastObjMesh *
read_obj(const char *obj_path)
{
    fastObjCallbacks cb;
    obj_files of;
    const char *slash = strrchr(obj_path, '/');
    const char *bslash = strrchr(obj_path, '\\');

    if (bslash > slash)
        slash = bslash;
    of.obj_path = obj_path;
    of.folder_len = slash != NULL ? (size_t)(slash - obj_path + 1) : 0;
    cb.file_open = open_beside_obj;
    cb.file_close = close_file;
    cb.file_read = read_bytes;
    cb.file_size = size_of_file;
    return fast_obj_read_with_callbacks(obj_path, &cb, &of);
}

static uint8_t
color_byte(float c)
{
    if (!(c > 0.0f))
        return 0;
    if (c >= 1.0f)
        return 255;
    return (uint8_t)(c * 255.0f + 0.5f);
}

/* One colour per triangle corner, from the corner's position, as the writer
   takes them. Left NULL when every corner is white: fast_obj gives white to
   every position the file does not colour, so that is no colour at all. */
static int
build_corner_colors(const fastObjMesh *mesh, submesh *s)
{
    uint8_t *c;
    uint32_t i;
    int any = 0;

    if (mesh->color_count == 0 || mesh->colors == NULL || s->p.count == 0)
        return 0;
    c = (uint8_t *)malloc((size_t)s->p.count * 3);
    if (c == NULL)
        return -1;
    for (i = 0; i < s->p.count; i++)
    {
        const float *src = &mesh->colors[s->p.data[i] * 3];
        uint32_t d;

        for (d = 0; d < 3; d++)
        {
            c[i * 3 + d] = color_byte(src[d]);
            if (c[i * 3 + d] != 255)
                any = 1;
        }
    }
    if (!any)
    {
        free(c);
        return 0;
    }
    s->colors = c;
    return 0;
}

/* Turn the collected corner indices into the arrays the writer takes, and
   work out which optional channels the submesh can carry. */
static int
build_submesh(const fastObjMesh *mesh, submesh *s, int want_texture,
    int want_colors, uint32_t *has_normals, uint32_t *has_uvs)
{
    uint32_t i, d;

    *has_normals = 1;
    *has_uvs = want_texture ? 1 : 0;
    for (i = 0; i < s->p.count; i++)
    {
        if (s->n.data[i] == 0)
            *has_normals = 0;
        if (s->t.data[i] == 0)
            *has_uvs = 0;
    }

    if (compact_stream(&s->p, mesh->positions, mesh->position_count, 3,
                       &s->positions, &s->num_positions, &s->tri_indices) != 0)
        return -1;
    if (*has_normals &&
        compact_stream(&s->n, mesh->normals, mesh->normal_count, 3,
                       &s->normals, &s->num_normals, &s->norm_indices) != 0)
        return -1;
    if (*has_uvs &&
        compact_stream(&s->t, mesh->texcoords, mesh->texcoord_count, 2,
                       &s->tex_coords, &s->num_tex_coords, &s->tex_indices) != 0)
        return -1;
    if (want_colors && build_corner_colors(mesh, s) != 0)
        return -1;

    s->face_tri_count = s->p.count / 3;
    if (s->num_positions == 0 || s->positions == NULL)
        return -1;

    for (d = 0; d < 3; d++)
    {
        s->bbox_min[d] = s->positions[d];
        s->bbox_max[d] = s->positions[d];
    }
    for (i = 1; i < s->num_positions; i++)
    {
        for (d = 0; d < 3; d++)
        {
            double v = s->positions[i * 3 + d];

            if (v < s->bbox_min[d]) s->bbox_min[d] = v;
            if (v > s->bbox_max[d]) s->bbox_max[d] = v;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------- */
/* Main                                                                  */
/* -------------------------------------------------------------------- */

static char *
replace_extension(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    size_t stem;
    char *out;

    if (bslash > slash)
        slash = bslash;
    stem = (dot != NULL && (slash == NULL || dot > slash)) ? (size_t)(dot - path) : strlen(path);
    out = (char *)malloc(stem + strlen(ext) + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, path, stem);
    strcpy(out + stem, ext);
    return out;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <input.obj> [output.prc] [output.pdf] [options]\n"
        "  Writes the OBJ (and its MTL materials and PNG/JPEG maps) as PRC.\n"
        "  --triangles     write uncompressed triangles for every part (default:\n"
        "                  compressed)\n"
        "  --no-normals    ignore the file's vn normals (the reader computes them)\n"
        "  --no-materials  ignore the MTL file (no colours, no textures)\n"
        "  --no-colors     ignore vertex colours given as v x y z r g b\n"
        "  --quant-tolerance F\n"
        "                  compressed tessellations' quantization tolerance, as a\n"
        "                  fraction of the bounding-box diagonal (default %g)\n",
        prog, OBJ_IMPORT_QUANT_TOLERANCE);
}

int
main(int argc, char *argv[])
{
    const char *in_path = NULL;
    const char *prc_path = NULL;
    const char *pdf_path = NULL;
    char *prc_default = NULL, *pdf_default = NULL;
    int force_triangles = 0, ignore_normals = 0, ignore_materials = 0, ignore_colors = 0;
    int a, rc = 1;
    double quant_tolerance = OBJ_IMPORT_QUANT_TOLERANCE;

    fastObjMesh *mesh = NULL;
    mtl_extras extras = { NULL, 0, 0 };
    prc_context *ctx = NULL;

    submesh *subs = NULL;
    uint32_t num_subs = 0;
    uint32_t *part_first = NULL;     /* first submesh of each part */
    uint32_t *part_count = NULL;     /* submeshes in each part */
    const fastObjGroup *parts = NULL;
    uint32_t num_parts = 0;

    prc_api_write_tessellation *tess = NULL;
    prc_api_write_rep_item *items = NULL;
    prc_api_write_node *nodes = NULL;
    prc_api_write_node **children = NULL;
    prc_api_write_node root;
    uint8_t *prc_buf = NULL;
    size_t prc_size = 0;

    double (*corner)[3] = NULL;
    double (*proj)[2] = NULL;
    uint32_t *ring = NULL;
    uint32_t max_face_vertices = 0;
    u32_array tri = { NULL, 0, 0 };
    uint32_t total_tris = 0, polygons_split = 0, degenerate_skipped = 0;
    uint32_t i, k;

    for (a = 1; a < argc; a++)
    {
        if (strcmp(argv[a], "--triangles") == 0)
            force_triangles = 1;
        else if (strcmp(argv[a], "--no-normals") == 0)
            ignore_normals = 1;
        else if (strcmp(argv[a], "--no-materials") == 0)
            ignore_materials = 1;
        else if (strcmp(argv[a], "--no-colors") == 0)
            ignore_colors = 1;
        else if (strcmp(argv[a], "--quant-tolerance") == 0 && a + 1 < argc)
        {
            quant_tolerance = atof(argv[++a]);
            if (!(quant_tolerance > 0.0))
            {
                usage(argv[0]);
                return 1;
            }
        }
        else if (argv[a][0] == '-' && argv[a][1] == '-')
        {
            usage(argv[0]);
            return 1;
        }
        else if (in_path == NULL)
            in_path = argv[a];
        else if (prc_path == NULL)
            prc_path = argv[a];
        else if (pdf_path == NULL)
            pdf_path = argv[a];
        else
        {
            usage(argv[0]);
            return 1;
        }
    }
    if (in_path == NULL)
    {
        usage(argv[0]);
        return 1;
    }
    if (prc_path == NULL)
        prc_path = prc_default = replace_extension(in_path, ".prc");
    if (pdf_path == NULL)
        pdf_path = pdf_default = replace_extension(in_path, ".pdf");
    if (prc_path == NULL || pdf_path == NULL)
        goto done;

    mesh = read_obj(in_path);
    if (mesh == NULL)
    {
        fprintf(stderr, "Could not read %s\n", in_path);
        goto done;
    }
    if (mesh->face_count == 0 || mesh->position_count <= 1)
    {
        fprintf(stderr, "%s contains no faces\n", in_path);
        goto done;
    }
    if (!ignore_materials && read_mtl_extras(in_path, &extras) != 0)
    {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }
    printf("%s: %u positions, %u normals, %u texcoords, %u faces, %u materials, "
           "%u objects, %u groups\n", in_path,
           mesh->position_count - 1, mesh->normal_count - 1, mesh->texcoord_count - 1,
           mesh->face_count, mesh->material_count, mesh->object_count, mesh->group_count);

    /* Parts: objects if the file names any, else groups, else the whole file. */
    if (mesh->object_count > 0)
    {
        parts = mesh->objects;
        num_parts = mesh->object_count;
    }
    else if (mesh->group_count > 0)
    {
        parts = mesh->groups;
        num_parts = mesh->group_count;
    }

    for (i = 0; i < mesh->face_count; i++)
        if (mesh->face_vertices[i] > max_face_vertices)
            max_face_vertices = mesh->face_vertices[i];
    if (max_face_vertices < 3)
        max_face_vertices = 3;
    corner = (double (*)[3])malloc((size_t)max_face_vertices * sizeof(*corner));
    proj = (double (*)[2])malloc((size_t)max_face_vertices * sizeof(*proj));
    ring = (uint32_t *)malloc((size_t)max_face_vertices * sizeof(uint32_t));
    part_first = (uint32_t *)calloc(num_parts ? num_parts : 1, sizeof(uint32_t));
    part_count = (uint32_t *)calloc(num_parts ? num_parts : 1, sizeof(uint32_t));
    /* At most one submesh per (part, material) pair, plus "no material". */
    subs = (submesh *)calloc((size_t)(num_parts ? num_parts : 1) * (mesh->material_count + 1),
                             sizeof(submesh));
    if (corner == NULL || proj == NULL || ring == NULL || part_first == NULL ||
        part_count == NULL || subs == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }

    {
        uint32_t p, nparts = num_parts ? num_parts : 1;

        for (p = 0; p < nparts; p++)
        {
            uint32_t face_begin = num_parts ? parts[p].face_offset : 0;
            uint32_t face_end = num_parts ? face_begin + parts[p].face_count : mesh->face_count;
            uint32_t index_pos = num_parts ? parts[p].index_offset : 0;
            uint32_t f;

            part_first[p] = num_subs;

            for (f = face_begin; f < face_end; f++)
            {
                uint32_t nv = mesh->face_vertices[f];
                uint32_t mat = (mesh->material_count > 0 && mesh->face_materials != NULL)
                    ? mesh->face_materials[f] : OBJ_NO_INDEX;
                const fastObjIndex *fi = &mesh->indices[index_pos];
                submesh *s = NULL;
                uint32_t c;

                index_pos += nv;

                /* fast_obj v1.3 ignores OBJ line records, so every face here
                   is a polygon; only a face of fewer than 3 corners is skipped. */
                if (nv < 3)
                {
                    degenerate_skipped++;
                    continue;
                }

                for (c = part_first[p]; c < num_subs; c++)
                {
                    if (subs[c].material == mat)
                    {
                        s = &subs[c];
                        break;
                    }
                }
                if (s == NULL)
                {
                    s = &subs[num_subs++];
                    s->material = mat;
                }

                for (c = 0; c < nv; c++)
                {
                    const float *v = &mesh->positions[fi[c].p * 3];

                    corner[c][0] = v[0];
                    corner[c][1] = v[1];
                    corner[c][2] = v[2];
                }
                tri.count = 0;
                if (triangulate_polygon((const double (*)[3])corner, nv, &tri, proj, ring) != 0)
                {
                    fprintf(stderr, "Out of memory\n");
                    goto done;
                }
                if (nv > 3)
                    polygons_split++;

                for (c = 0; c < tri.count; c++)
                {
                    const fastObjIndex *x = &fi[tri.data[c]];

                    if (u32_push(&s->p, x->p) || u32_push(&s->t, x->t) || u32_push(&s->n, x->n))
                    {
                        fprintf(stderr, "Out of memory\n");
                        goto done;
                    }
                }
            }
            part_count[p] = num_subs - part_first[p];
        }
        num_parts = nparts;
    }

    if (num_subs == 0)
    {
        fprintf(stderr, "%s contains no polygons\n", in_path);
        goto done;
    }

    tess = (prc_api_write_tessellation *)calloc(num_subs, sizeof(*tess));
    items = (prc_api_write_rep_item *)calloc(num_subs, sizeof(*items));
    nodes = (prc_api_write_node *)calloc(num_parts, sizeof(*nodes));
    children = (prc_api_write_node **)calloc(num_parts, sizeof(*children));
    if (tess == NULL || items == NULL || nodes == NULL || children == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }

    ctx = prc_api_new_context(NULL);
    if (ctx == NULL)
    {
        fprintf(stderr, "prc_api_new_context failed\n");
        goto done;
    }

    for (i = 0; i < num_subs; i++)
    {
        submesh *s = &subs[i];
        const fastObjMaterial *m = (s->material != OBJ_NO_INDEX && !ignore_materials)
            ? &mesh->materials[s->material] : NULL;
        const mtl_extra *mx = (m != NULL) ? mtl_extras_find(&extras, m->name) : NULL;
        int want_texture = 0;
        uint32_t main_kind = PRC_API_WRITE_TEXTURE_KIND_DIFFUSE;
        const map_line *placement = NULL;
        double refl_intensity = 1.0;
        uint32_t has_normals, has_uvs, tw = 0, th = 0;
        prc_api_write_tessellation *t = &tess[i];
        prc_api_write_rep_item *ri = &items[i];

        /* The maps, in the order they are applied: diffuse, opacity,
           reflection. The first image that loads is the item's texture and
           the rest become layers after it. A map is attached only if its
           image exists and is PNG or JPEG. */
        if (m != NULL)
        {
            const map_line *maps[3];
            const char *paths[3];
            static const char *const names[3] = { "map_Kd", "map_d", "refl" };
            static const uint32_t kinds[3] = { PRC_API_WRITE_TEXTURE_KIND_DIFFUSE,
                PRC_API_WRITE_TEXTURE_KIND_OPACITY, PRC_API_WRITE_TEXTURE_KIND_SPHERICAL_REFLECTION };
            uint32_t j;

            maps[0] = (mx != NULL && mx->map_Kd.path != NULL) ? &mx->map_Kd : NULL;
            maps[1] = (mx != NULL && mx->map_d.path != NULL) ? &mx->map_d : NULL;
            maps[2] = (mx != NULL && mx->refl.path != NULL) ? &mx->refl : NULL;
            /* fast_obj's texture name, not its path: the path is the OBJ's
               folder joined to the name, which load_image must not trust. */
            paths[0] = maps[0] ? maps[0]->path
                : ((m->map_Kd != 0) ? mesh->textures[m->map_Kd].name : NULL);
            paths[1] = maps[1] ? maps[1]->path
                : ((m->map_d != 0) ? mesh->textures[m->map_d].name : NULL);
            paths[2] = maps[2] ? maps[2]->path : NULL;

            /* MTL scales a reflection map by Ks. With no Ks it applies in full. */
            if (mx != NULL && mx->has_Ks)
                refl_intensity = (m->Ks[0] + m->Ks[1] + m->Ks[2]) / 3.0;
            if (paths[2] != NULL && !(refl_intensity > 0.0))
            {
                fprintf(stderr, "warning: material %s: Ks is black, so its reflection map "
                        "has no effect; left out\n", m->name ? m->name : "(unnamed)");
                paths[2] = NULL;
            }

            for (j = 0; j < 3; j++)
            {
                uint8_t *bytes = NULL;
                size_t size = 0;
                uint32_t fmt = 0, w = 0, h = 0;

                if (paths[j] == NULL ||
                    load_image(in_path, paths[j], names[j], &bytes, &size, &fmt, &w, &h) != 0)
                    continue;
                if (!want_texture)
                {
                    want_texture = 1;
                    main_kind = kinds[j];
                    placement = maps[j];
                    s->texture_bytes = bytes;
                    s->texture_size = size;
                    s->texture_format = fmt;
                    tw = w;
                    th = h;
                }
                else
                {
                    prc_api_write_texture_layer *L = &s->layers[s->num_layers];

                    /* A layer uses the main texture's placement and wrap. */
                    if (maps[j] != NULL && maps[j]->placed &&
                        (placement == NULL ||
                         maps[j]->scale[0] != placement->scale[0] || maps[j]->scale[1] != placement->scale[1] ||
                         maps[j]->offset[0] != placement->offset[0] || maps[j]->offset[1] != placement->offset[1] ||
                         maps[j]->clamp != placement->clamp))
                        fprintf(stderr, "warning: material %s: %s has its own -s/-o/-clamp; "
                                "it is placed as the first map instead\n",
                                m->name ? m->name : "(unnamed)", names[j]);
                    s->layer_bytes[s->num_layers] = bytes;
                    L->kind = kinds[j];
                    L->image = bytes;
                    L->image_size = size;
                    L->format = fmt;
                    L->width = w;
                    L->height = h;
                    L->intensity = (kinds[j] == PRC_API_WRITE_TEXTURE_KIND_SPHERICAL_REFLECTION)
                        ? refl_intensity : 1.0;
                    s->num_layers++;
                }
            }
        }

        if (build_submesh(mesh, s, want_texture, !ignore_colors, &has_normals, &has_uvs) != 0)
        {
            fprintf(stderr, "Out of memory\n");
            goto done;
        }
        if (want_texture && !has_uvs)
        {
            fprintf(stderr, "warning: material %s has a texture but its faces have no UVs; "
                    "left untextured\n", m->name ? m->name : "(unnamed)");
            want_texture = 0;
        }

        if (ignore_normals)
            has_normals = 0;

        t->kind = force_triangles
            ? PRC_API_WRITE_TESS_KIND_TRIANGLES : PRC_API_WRITE_TESS_KIND_COMPRESSED;
        t->positions = s->positions;
        t->num_positions = s->num_positions;
        t->tri_indices = s->tri_indices;
        t->num_triangles = s->face_tri_count;

        if (t->kind == PRC_API_WRITE_TESS_KIND_COMPRESSED)
            t->tolerance = prc_write_tol_relative(quant_tolerance);
        t->face_tri_counts = &s->face_tri_count;
        t->num_faces = 1;
        if (has_normals)
        {
            t->normals = s->normals;
            t->num_normals = s->num_normals;
            t->norm_indices = s->norm_indices;
        }
        else if (t->kind == PRC_API_WRITE_TESS_KIND_TRIANGLES)
        {
            /* Without supplied normals TRIANGLES would give the whole face
               one flat normal; have the reader compute per-vertex ones. */
            t->must_calculate_normals = 1;
        }
        if (want_texture)
        {
            t->tex_coords = s->tex_coords;
            t->num_tex_coords = s->num_tex_coords;
            t->tex_indices = s->tex_indices;
        }
        if (s->colors != NULL)
        {
            t->vertex_colors = s->colors;
            t->num_vertex_colors = s->p.count;
        }

        ri->kind = PRC_API_WRITE_RI_SURFACE;
        ri->biased_tessellation_index = i + 1;
        if (m != NULL)
        {
            ri->has_material = 1;
            ri->material_color[0] = m->Kd[0];
            ri->material_color[1] = m->Kd[1];
            ri->material_color[2] = m->Kd[2];
            ri->material_alpha = (m->d > 0.0f && m->d <= 1.0f) ? m->d : 1.0;
            /* MTL Ns runs 0..1000; the writer takes 0..1. */
            ri->material_shininess = m->Ns <= 0.0f ? 0.0 : (m->Ns >= 1000.0f ? 1.0 : m->Ns / 1000.0);
            /* fast_obj gives a material without Ns the value 1, which would be
               written as 0.001, a highlight spread over the whole surface.
               Without Ns, leave it to the writer's default instead. */
            if (mx != NULL && !mx->has_Ns)
                ri->material_shininess = 0.0;

            /* Ambient, specular and emissive, but only if the MTL gives at
               least one of them; a slot it leaves out keeps the writer's
               default, as when none is given. */
            if (mx != NULL && (mx->has_Ka || mx->has_Ks || mx->has_Ke))
            {
                uint32_t c;

                ri->has_material_slots = 1;
                for (c = 0; c < 3; c++)
                {
                    ri->material_ambient[c] = mx->has_Ka ? m->Ka[c] : m->Kd[c];
                    ri->material_specular[c] = mx->has_Ks ? m->Ks[c] : 0.2;
                    ri->material_emissive[c] = mx->has_Ke ? m->Ke[c] : 0.0;
                }
                /* With slots the shininess is written exactly as given, so
                   an absent Ns takes the 0.3 the writer uses otherwise. */
                if (!mx->has_Ns)
                    ri->material_shininess = 0.3;
            }

            if (want_texture)
            {
                ri->has_texture = 1;
                ri->texture_image = s->texture_bytes;
                ri->texture_image_size = s->texture_size;
                ri->texture_format = s->texture_format;
                ri->texture_width = tw;
                ri->texture_height = th;
                ri->texture_kind = main_kind;
                if (main_kind == PRC_API_WRITE_TEXTURE_KIND_SPHERICAL_REFLECTION)
                    ri->texture_intensity = refl_intensity;
                if (placement != NULL)
                {
                    ri->texture_scale[0] = placement->scale[0];
                    ri->texture_scale[1] = placement->scale[1];
                    ri->texture_offset[0] = placement->offset[0];
                    ri->texture_offset[1] = placement->offset[1];
                    if (placement->clamp)
                    {
                        /* MTL's clamp keeps the image to 0..1; the nearest
                           wrap PRC readers show is clamp to border. */
                        ri->texture_wrap_u = PRC_API_WRITE_TEXTURE_WRAP_CLAMP_TO_BORDER;
                        ri->texture_wrap_v = PRC_API_WRITE_TEXTURE_WRAP_CLAMP_TO_BORDER;
                    }
                }
                if (s->num_layers > 0)
                {
                    ri->texture_layers = s->layers;
                    ri->num_texture_layers = s->num_layers;
                }
            }
        }
        total_tris += s->face_tri_count;
    }

    /* Tree: one root, one child node per part, each holding its submeshes. */
    memset(&root, 0, sizeof(root));
    for (k = 0; k < num_parts; k++)
    {
        prc_api_write_node *node = &nodes[k];
        uint32_t d;

        if (part_count[k] == 0)
            continue;
        node->rep_items = &items[part_first[k]];
        node->num_rep_items = part_count[k];
        node->name = (parts != NULL && parts[k].name != NULL) ? parts[k].name : NULL;
        node->part_name = node->name;
        for (d = 0; d < 3; d++)
        {
            node->bbox_min[d] = subs[part_first[k]].bbox_min[d];
            node->bbox_max[d] = subs[part_first[k]].bbox_max[d];
        }
        for (i = part_first[k] + 1; i < part_first[k] + part_count[k]; i++)
        {
            for (d = 0; d < 3; d++)
            {
                if (subs[i].bbox_min[d] < node->bbox_min[d]) node->bbox_min[d] = subs[i].bbox_min[d];
                if (subs[i].bbox_max[d] > node->bbox_max[d]) node->bbox_max[d] = subs[i].bbox_max[d];
            }
        }
        if (root.num_children == 0)
        {
            memcpy(root.bbox_min, node->bbox_min, sizeof(root.bbox_min));
            memcpy(root.bbox_max, node->bbox_max, sizeof(root.bbox_max));
        }
        else
        {
            for (d = 0; d < 3; d++)
            {
                if (node->bbox_min[d] < root.bbox_min[d]) root.bbox_min[d] = node->bbox_min[d];
                if (node->bbox_max[d] > root.bbox_max[d]) root.bbox_max[d] = node->bbox_max[d];
            }
        }
        children[root.num_children++] = node;
    }
    /* The root is an empty part. Its box is still written, so it is given the
       union of its children's boxes. */
    root.has_empty_part = 1;
    root.children = children;

    if (prc_api_write_prc_buffer(ctx, NULL, &root, tess, num_subs, &prc_buf, &prc_size) != 0)
    {
        fprintf(stderr, "Writing PRC failed\n");
        prc_api_print_error_stack(ctx);
        goto done;
    }
    {
        FILE *out = fopen(prc_path, "wb");

        if (out == NULL || fwrite(prc_buf, 1, prc_size, out) != prc_size)
        {
            fprintf(stderr, "Could not write %s\n", prc_path);
            if (out != NULL)
                fclose(out);
            goto done;
        }
        fclose(out);
    }
    {
        /* A default view aimed at the model. Without a camera Adobe Acrobat
           often shows nothing until the model is fitted by hand, turns it
           about a point away from it, and does nothing on Home. No node here
           carries a transform, so the root's box is the placed model's. */
        prc_pdf_view_spec view;
        prc_pdf_write_options pdf_opts;

        prc_api_pdf_view_from_bounds(root.bbox_min, root.bbox_max, &view);
        memset(&pdf_opts, 0, sizeof(pdf_opts));
        pdf_opts.views = &view;
        pdf_opts.num_views = 1;
        if (prc_api_pdf_embed_prc(ctx, pdf_path, prc_buf, prc_size, &pdf_opts) != 0)
        {
            fprintf(stderr, "Writing PDF failed\n");
            prc_api_print_error_stack(ctx);
            goto done;
        }
    }

    {
        uint32_t textured = 0, compressed = 0, layers = 0, colored = 0;

        for (i = 0; i < num_subs; i++)
        {
            if (items[i].has_texture) textured++;
            if (tess[i].vertex_colors != NULL) colored++;
            layers += items[i].num_texture_layers;
            if (tess[i].kind == PRC_API_WRITE_TESS_KIND_COMPRESSED) compressed++;
        }
        printf("Wrote %s (%lu bytes) and %s: %u parts, %u tessellations "
               "(%u compressed, %u textured, %u texture layers, %u with vertex colours), "
               "%u triangles; %u polygons triangulated, %u degenerate faces skipped\n",
               prc_path, (unsigned long)prc_size, pdf_path, root.num_children, num_subs,
               compressed, textured, layers, colored, total_tris, polygons_split, degenerate_skipped);
    }
    rc = 0;

done:
    if (ctx != NULL)
    {
        if (prc_buf != NULL)
            prc_api_write_prc_buffer_free(ctx, prc_buf);
        prc_api_release_context(ctx);
    }
    if (subs != NULL)
        for (i = 0; i < num_subs; i++)
            submesh_free(&subs[i]);
    free(subs); free(part_first); free(part_count);
    free(tess); free(items); free(nodes); free(children);
    free(corner); free(proj); free(ring); free(tri.data);
    free(prc_default); free(pdf_default);
    mtl_extras_free(&extras);
    if (mesh != NULL)
        fast_obj_destroy(mesh);
    return rc;
}
