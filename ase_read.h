#pragma once

/* ase_read.h */
/* Provided under the terms of the MIT license. See the end of the file for license details. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Use this if you want to use libdeflate instead of sinfl, it's approximately 35% faster at decompressing but also a much larger and bulkier implementation.
//#define ASE_USE_LIBDEFLATE

/* ======================= High Level API ====================== */
struct AsePixelData
{
    uint8_t* data;
    int      length;
    uint16_t w, h;
    int      origin_cel;   // The first cel index this data is used in. This is used to determine a fallback palette when blitting the pure pixel data. Frame drawing using the frame palette instead.
    // These fields are set by ase_trim, excluding all parts of the sprite that are transparent.
    bool     is_trimmed;
    uint16_t trimmed_x, trimmed_y;
    uint16_t trimmed_w, trimmed_h;
};

struct AseUserData
{
    uint32_t    color;
    const char* text;
};

struct AseCel
{
    int pixel_data_index;
    int16_t         x, y;
    int16_t      z_index;
    int32_t   draw_order; // Combined layer + z_index. Sort by this to find draw order or use it as Z-Offset in 3D engines.
    uint8_t      opacity;

    struct AseUserData user_data;
};

struct AseLayer
{
    const char* name;
    bool     visible;
    uint8_t  opacity;
    int   blend_mode;
    bool    is_group; // For nested layers
    int       parent;
    int        depth;

    struct AseUserData user_data;
};

struct AseTag
{
    const char* name;
    int  start_frame;
    int    end_frame;

    struct AseUserData user_data;
};

struct AsePalette
{
    uint32_t colors[256];
};

struct AseFrame
{
    struct AsePalette* palette;
    float              duration;
};

struct AseSlice
{
    const    char* name;
    int32_t  slice_x, slice_y;
    uint32_t slice_w, slice_h;

    bool     is_9patch;
    int32_t  patch_center_x, patch_center_y;
    uint32_t patch_center_w, patch_center_h;

    bool    has_pivot;
    int32_t pivot_x, pivot_y;

    struct AseUserData user_data;
};

struct AseFile
{
    uint16_t width, height;
    uint16_t pixel_ratio_x, pixel_ratio_y; // 1:1, 1:2 or 2:1
    int bytes_per_pixel; // 1 = Indexed, 2 = Grayscale + Alpha, 4 = RGBA

    struct AsePixelData* pixel_data;
    struct AseCel*       cels; // [Frame 0, Layer 0][Frame 0, Layer 1][Frame 1, Layer 0][Frame 1, Layer 1][Frame 2, Layer 0] etc...
    struct AseFrame*     frames;
    struct AseLayer*     layers;
    struct AseTag*       tags;
    struct AsePalette*   palettes; // Each frame can have it's own palette.
    struct AseSlice*     slices;
    int num_pixel_data_blocks;
    int num_cels; // num_frames * num_layers
    int num_frames;
    int num_layers;
    int num_tags;
    int num_palettes;
    int num_slices;
    // All data referenced by AseFile (included the AseFile itself) is stored in one continuous buffer.
    char* data;
    int   data_length;

    struct AseUserData user_data;
};

struct AseFile* ase_read_file(const char* path);
void            ase_free_file(struct AseFile* file);
// For creating texture atlases for GPU rendering.
bool ase_trim_pixels(struct AseFile* file, int pixel_data_index);
bool ase_copy_pixels(struct AseFile* file, int pixel_data_index, uint8_t* dst, int pitch, int offset_x, int offset_y);
// Software rendering.
void ase_draw_cel_partial(struct AseFile* file, int frame, int layer, int src_x, int src_y, int src_w, int src_h, uint8_t* dst, int pitch, int offset_x, int offset_y);
void ase_draw_frame_partial(struct AseFile* file, int frame, int src_x, int src_y, int src_w, int src_h, uint8_t* dst, int pitch, int offset_x, int offset_y);
static inline void ase_draw_cel(struct AseFile* file, int frame, int layer, uint8_t* dst, int pitch, int offset_x, int offset_y){ ase_draw_cel_partial(file, frame, layer, 0, 0, file->width, file->height, dst, pitch, offset_x, offset_y); }
static inline void ase_draw_frame(struct AseFile* file, int frame, uint8_t* dst, int pitch, int offset_x, int offset_y){ ase_draw_frame_partial(file, frame, 0, 0, file->width, file->height, dst, pitch, offset_x, offset_y); }
// Manually blending pixels over a background.
void ase_blend_pixels(int blend_mode, uint8_t opacity, const uint32_t* restrict palette, uint8_t* restrict src, int src_bytes_per_pixel, int src_pitch, int src_x, int src_y, int src_w, int src_h, uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y);

static inline int ase_get_cel_idx(struct AseFile* file, int frame, int layer)
{
    return file->num_layers * frame + layer;
}

static inline int ase_get_frame_from_cel_idx(struct AseFile* file, int cel_idx)
{
    return cel_idx / file->num_layers;
}

static inline int ase_get_layer_from_cel_idx(struct AseFile* file, int cel_idx)
{
    return cel_idx % file->num_layers;
}

/* ======================= Low Level API ======================= */

struct AseParser;
enum AseStructType;

struct AseParser ase_open_parser(const char* path);
void ase_close_parser(struct AseParser* p);
void ase_reset_parser(struct AseParser* p);
enum AseStructType ase_parse_next(struct AseParser* p);
void ase_skip_chunk(struct AseParser* p);
void ase_skip_frame(struct AseParser* p);

/* ====================== Low Level API structs ====================== */

typedef enum {
    CHUNK_LAYER = 0x2004,
    CHUNK_CEL = 0x2005,
    CHUNK_CEL_EXTRA = 0x2006,
    CHUNK_COLOR_PROFILE = 0x2007,
    CHUNK_EXTERNAL_FILES = 0x2008,
    CHUNK_TAGS = 0x2018,
    CHUNK_PALETTE = 0x2019,
    CHUNK_USER_DATA = 0x2020,
    CHUNK_SLICE = 0x2022,
    CHUNK_TILESET = 0x2023
} ase_chunk_type;

enum AseStructType
{
    ASE_END_OF_FILE         = 0,
    ASE_ERROR               = 1,
    ASE_HEADER              = 2,
    ASE_FRAME_HEADER        = 3,
    ASE_CHUNK_HEADER        = 5,
    // 0x0004, 0x0011, 0x2016, 0x2017 are reserved for the deprecated chunk types
    ASE_LAYER               = 0x2004,
    ASE_CEL                 = 0x2005,
    ASE_CEL_EXTRA           = 0x2006,
    ASE_COLOR_PROFILE       = 0x2007,
    ASE_EXTERNAL_FILES      = 0x2008,
    ASE_EXTERNAL_FILE_ENTRY = 0x0108,
    ASE_TAGS                = 0x2018,
    ASE_TAG                 = 0x0118,
    ASE_PALETTE             = 0x2019,
    ASE_PALETTE_ENTRY       = 0x0119,
    ASE_USER_DATA           = 0x2020,
    ASE_SLICE               = 0x2022,
    ASE_SLICE_KEY           = 0x0122,
    ASE_TILESET             = 0x2023
};

typedef enum {
    INCLUDE_CHUNK_HEADER = 1 << 0,
    INCLUDE_FRAME = 1 << 1,
    INCLUDE_LAYER = 1 << 2,
    INCLUDE_CEL = 1 << 3,
    INCLUDE_CEL_EXTRA = 1 << 4,
    INCLUDE_COLOR_PROFILE = 1 << 5,
    INCLUDE_EXTERNAL_FILES = 1 << 6,
    INCLUDE_TAGS = 1 << 7,
    INCLUDE_PALETTE = 1 << 8,
    INCLUDE_USER_DATA = 1 << 9,
    INCLUDE_SLICE = 1 << 10,
    INCLUDE_TILESET = 1 << 11
} ase_include_flag_t;

enum AseError
{
    ASE_NO_ERROR,
    ASE_CANT_OPEN,
    ASE_UNKNOWN_FILE_FORMAT,
    ASE_PARSER_ERROR,
};

struct ase_header_t;
struct ase_slice_t;
struct ase_chunk_header_t;
struct ase_frame_header_t;

struct AseParser
{
    // Return values
    void*              element;
    int                element_size;
    enum AseStructType element_type;


    uint32_t filter;// ase_include_flag_t
    char* data;
    int data_size;

    char* cur;
    int cur_frame;
    int cur_frame_remaining_chunks;
    int cur_chunk_remaining_entries;

    char* next_frame_start;
    char* next_chunk_start;
    int nesting_depth;
    uint32_t current_chunk_type;

    struct ase_header_t* file_header;
    struct ase_slice_t* current_slice;
    struct ase_chunk_header_t* current_chunk_header;
    struct ase_frame_header_t* current_frame_header;

    uint32_t color_palette[256];
    int color_palette_size;
    int current_color_palette_index;
    enum AseError error;
};

/* ====================== File structs ====================== */

#pragma pack(push, 1)
#if defined(__GNUC__) || defined(__clang__)
#pragma scalar_storage_order little-endian
#endif

/* ======================= File-Header ======================= */
typedef struct ase_header_t{
    uint32_t file_size;          // Total file size
    uint16_t magic;              // 0xA5E0
    uint16_t frames;             // Number of frames
    uint16_t width;              // Width in pixels
    uint16_t height;             // Height in pixels
    uint16_t color_depth;        // 32=RGBA, 16=Grayscale, 8=Indexed
    uint32_t flag_layer_opacity:1;              // Bit 1: layer opacity valid, bit2: group blend/opacity, bit4: layers have UUID
    uint32_t flag_group_blend:1;
    uint32_t:1;
    uint32_t flag_layers_have_uuid:1;
    uint32_t:28;
    uint16_t speed;              // Deprecated, use frame duration
    uint32_t reserved[2];
    uint8_t  transparent_index;  // Palette index for transparent color (indexed sprites)
    uint8_t  ignore[3];          // Ignore
    uint16_t num_colors;         // 0 means 256
    uint8_t  pixel_width;        // Pixel ratio width
    uint8_t  pixel_height;       // Pixel ratio height
    int16_t  grid_x;
    int16_t  grid_y;
    uint16_t grid_width;         // 0 if no grid
    uint16_t grid_height;
    uint8_t unused[84];
} ase_header_t;

/* ========================= Frame Header ========================= */
typedef struct ase_frame_header_t {
    uint32_t byte_length;     // Size of this frame (including this header)
    uint16_t magic;              // 0xF1FA
    uint16_t old_chunk_count;    // 0xFFFF means use new field
    uint16_t duration_ms;        // Frame duration in milliseconds
    uint8_t unused[2];          // Reserved
    uint32_t new_chunk_count;    // Used if old_chunk_count == 0xFFFF
} ase_frame_header_t;

/* ========================= Chunk Header ========================= */

typedef struct ase_chunk_header_t{
    uint32_t chunk_size;         // Includes size and type fields (≥6)
    uint16_t chunk_type;         // e.g., 0x2004, 0x2005, etc.
    uint8_t chunk_data[];
} ase_chunk_header_t;

/* ========================= Chunk Type: 0x0004 and 0x0011 (Old Palette) ========================= */
typedef struct {
    uint16_t num_packets;
} ase_palette_old_t;

typedef struct {
    uint8_t skip;
    uint8_t num_colors;
    uint8_t color[];
} ase_palette_old_packet_t;

/* ========================= Chunk Type: 0x2004 (Layer) ========================= */
typedef enum ase_layer_type {
    LAYER_NORMAL   = 0,
    LAYER_GROUP    = 1,
    LAYER_TILEMAP  = 2
} ase_layer_type_t;

typedef enum ase_blend_mode {
    BLEND_NORMAL        = 0,
    BLEND_MULTIPLY      = 1,
    BLEND_SCREEN        = 2,
    BLEND_OVERLAY       = 3,
    BLEND_DARKEN        = 4,
    BLEND_LIGHTEN       = 5,
    BLEND_COLOR_DODGE   = 6,
    BLEND_COLOR_BURN    = 7,
    BLEND_HARD_LIGHT    = 8,
    BLEND_SOFT_LIGHT    = 9,
    BLEND_DIFFERENCE    = 10,
    BLEND_EXCLUSION     = 11,
    BLEND_HUE           = 12,
    BLEND_SATURATION    = 13,
    BLEND_COLOR         = 14,
    BLEND_LUMINOSITY    = 15,
    BLEND_ADDITION      = 16,
    BLEND_SUBTRACT      = 17,
    BLEND_DIVIDE        = 18,
    BLEND_MAX
} ase_blend_mode_t;

typedef struct {
    uint16_t flag_visible : 1;
    uint16_t flag_editable : 1;
    uint16_t flag_lock_movement : 1;
    uint16_t flag_background : 1;
    uint16_t flag_prefer_linked : 1;
    uint16_t flag_collapsed : 1;
    uint16_t flag_reference_layer : 1;
    uint16_t:9; // Unused flags
    uint16_t type;               // ase_layer_type_t
    uint16_t child_level;
    uint16_t default_width;      // ignored
    uint16_t default_height;     // ignored
    uint16_t blend_mode;         // ase_blend_mode_t
    uint8_t opacity;
    uint8_t unused[3];
    uint16_t layer_name_length;
    char     layer_name[];
} ase_layer_chunk_t;

/* ========================= Chunk Type: 0x2005 (Cel) ========================= */
typedef enum ase_cel_type {
    CEL_RAW_IMAGE        = 0,
    CEL_LINKED           = 1,
    CEL_COMPRESSED_IMAGE = 2,
    CEL_COMPRESSED_TILEMAP = 3
} ase_cel_type_t;

typedef struct ase_cel_t {
    uint16_t layer_idx;          // Index of layer (see NOTE.2)
    int16_t x;
    int16_t y;
    uint8_t opacity;
    uint16_t cel_type;           // ase_cel_type_t
    int16_t z_index;
    uint8_t unused[5];

    union
    {
        struct
        {
            uint16_t w; uint16_t h;
            uint8_t pixels[];
        };
        struct
        {
            uint16_t frame;
        } linked;
        struct
        {
            uint16_t tile_w;
            uint16_t tile_h;
            uint16_t bits_per_tile;
            uint32_t mask_id;
            uint32_t mask_xflip;
            uint32_t mask_yflip;
            uint32_t mask_diag;
            uint8_t reserved[10];
            uint8_t tiles[];
        } tilemap;
    };
} ase_cel_t;

/* ========================= Chunk Type: 0x2006 (Cel Extra) ========================= */
typedef struct {
    uint32_t flags;              // bit0: precise bounds set
    uint16_t precise_x;
    uint16_t precise_x_fraction;
    uint16_t precise_y;
    uint16_t precise_y_fraction;
    uint16_t cel_width;
    uint16_t cel_width_fraction;
    uint16_t cel_height;
    uint16_t cel_height_fraction;
    uint8_t unused[16];
} ase_cel_extra_t;

/* ========================= Chunk Type: 0x2007 (Color Profile) ========================= */
typedef struct {
    uint16_t type;               // 0=none, 1=sRGB, 2=ICC
    uint16_t flags;              // bit0: fixed gamma
    uint16_t gamma;
    uint16_t gamma_fraction;              // 1.0 = linear
    uint8_t reserved[8];
    // If type == 2:
    uint32_t icc_len;
    uint8_t icc_data[];
} ase_color_profile_t;

/* ========================= Chunk Type: 0x2008 (External Files) ========================= */
typedef enum {
    EXTERNAL_PALETTE = 0,
    EXTERNAL_TILESET = 1,
    EXTERNAL_EXTENSION = 2,
    EXTERNAL_TILEMGMT = 3
} ase_external_file_type_t;

typedef struct {
    uint32_t num_entries;
    uint8_t reserved[8];
} ase_external_files_t; // Followed by ase_external_file_entry_t

// For each entry in num_entries
typedef struct {
    uint32_t entry_id;
    uint8_t type; // ase_external_file_type_t
    uint8_t reserved[7];
    uint16_t filename_or_id_length;
    char filename_or_id[];
} ase_external_file_entry_t;

/* ========================= Chunk Type: 0x2016 (Mask – deprecated) ========================= */
typedef struct {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t unused[8];
    uint16_t name_length;
    char name[];
} ase_mask_t; // Followed by uint8_t[] bitmap data.

/* ========================= Chunk Type: 0x2018 (Tags) ========================= */
typedef enum  {
    LOOP_FORWARD = 0,
    LOOP_REVERSE = 1,
    LOOP_PING_PONG = 2,
    LOOP_PING_PONG_REVERSE = 3
} ase_loop_dir_t;

typedef struct {
    uint16_t num_tags;
    uint8_t unused[8];
} ase_tags_t;

// For each tag in num_tags
typedef struct {
    uint16_t from, to;
    uint8_t loop_dir; // ase_loop_dir_t
    uint16_t repeat;
    uint8_t unused[6];
    uint8_t tag_color[3]; // Depreceated in Aseprite 1.3.x
    uint8_t extra;
    uint16_t name_length;
    char name[];
} ase_tag_t;

/* ========================= Chunk Type: 0x2019 (Palette) ========================= */
typedef struct {
    uint32_t new_palette_size;
    uint32_t first_index;
    uint32_t last_index;
    uint8_t unused[8];
} ase_palette_t;

// For each entry in [first_index, last_index]:
typedef struct {
    uint16_t flag_has_name:1;
    uint16_t:15; // Unused flags
    uint32_t rgba;
    // if flags&1:
    uint16_t color_name_length;
    char color_name[];
} ase_palette_entry_t;

/* ========================= Chunk Type: 0x2020 (User Data) ========================= */
typedef struct {
    uint32_t flag_has_text:1;
    uint32_t flag_has_color:1;
    uint32_t flag_has_properties:1;
    uint32_t:29; // unused flags
} ase_user_data_t;

// if flag_has_text:
typedef struct {
    uint16_t text_length;
    char text[];
} ase_text_t;

// if flag_has_color:
typedef struct {
    uint32_t rgba;
} ase_color_t;

// if flag_has_properties:
typedef struct {
    uint32_t props_size;
    uint32_t num_maps;
} ase_properties_t;

typedef struct {
    uint32_t extension_id; // 0 = Base Aseprite
    uint32_t num_properties;
} ase_property_map_t;

typedef struct {
    uint16_t name_length;
    char name[];
} ase_property_name_t;

typedef enum {
    TYPE_BOOL, // uint8_t
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_INT32,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FIXED16_16, // 2 uint16_t
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_STRING, // uint16t size + char[]
    TYPE_POINT, // 2 int32_t
    TYPE_SIZE, // 2 uint32_t
    TYPE_RECT, // POINT + SIZE
    TYPE_VECTOR, // It's complicated, see the spec.
    TYPE_PROPERTY_MAP, // Oh no. Recursion.
    TYPE_UUID // uint8_t[16]
} ase_property_type_t;

typedef struct {
    uint16_t type; // ase_property_type_t
    uint8_t data[]; // Cast based on type.
} ase_property_t;

/* ========================= Chunk Type: 0x2022 (Slice) ========================= */
typedef struct ase_slice_t{
    uint32_t num_keys;
    uint32_t flag_has_9patch:1;
    uint32_t flag_has_pivot:1;
    uint32_t:30;
    uint32_t reserved;
    uint16_t name_length;
    char name[];
} ase_slice_t;

// For each key:
typedef struct
{
    uint32_t frame_num;
    int32_t  slice_x, slice_y;
    uint32_t slice_w, slice_h;
    int32_t  center_and_pivot[6];
} ase_slice_key_t;

/* ========================= Chunk Type: 0x2023 (Tileset) ========================= */
typedef struct ase_tileset_t {
    uint32_t tileset_id;
    uint32_t flag_extern:1;
    uint32_t flag_internal:1;
    uint32_t flag_empty_tile_id_0:1;
    uint32_t flag_auto_flip_x:1;
    uint32_t flag_auto_flip_y:1;
    uint32_t flag_auto_flip_diagonal:1;
    uint32_t:26;
    uint32_t num_tiles;
    uint16_t tile_width;
    uint16_t tile_height;
    int16_t base_index;         // UI display offset
    uint8_t reserved[14];
    uint16_t name_length;
    char name[];
    // if flag_extern: uint32_t ext_file_id, uint32_t ext_tileset_id
    // if flag_internal: uint32_t compressed_data_len, PIXEL[] compressed_tileset_image (ZLIB)
} ase_tileset_t;

#pragma pack(pop)
#if defined(__GNUC__) || defined(__clang__)
#pragma scalar_storage_order default
#endif

#ifdef __cplusplus
}
#endif

/*  Copyright (c) 2026 Manuel Riecke

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
*/