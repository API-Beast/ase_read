#include "ase_read.h"
#include <stdio.h>
#include <stdlib.h> 
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

// ------------- High Level API ------------- //

#define PAD8(X) ((X + 7) & ~7)

static int calc_layout(int num_frames, int num_layers, int num_cels, int num_tags, int num_pixel_data_blocks, int num_palettes, int num_slices, int string_length, int length, int max_compressed_size,
                       int* off_frames, int* off_layers, int* off_cels, int* off_tags, 
                       int* off_cel_header, int* off_palettes, int* off_slices, int* off_strings, int* off_data, int* off_temp_buffer)
{
    int offset = 0;
    offset += sizeof(struct AseFile);
    offset = PAD8(offset); /* 8 byte alignment */
    
    *off_frames = offset;
    offset += num_frames * sizeof(struct AseFrame);
    offset = PAD8(offset);
    
    *off_layers = offset;
    offset += num_layers * sizeof(struct AseLayer);
    offset = PAD8(offset);
    
    *off_cels = offset;
    offset += num_cels * sizeof(struct AseCel);
    offset = PAD8(offset);
    
    *off_tags = offset;
    offset += num_tags * sizeof(struct AseTag);
    offset = PAD8(offset);
    
    *off_cel_header = offset;
    offset += num_pixel_data_blocks * sizeof(struct AsePixelData);
    offset = PAD8(offset);
    
    *off_palettes = offset;
    offset += num_palettes * sizeof(struct AsePalette);
    offset = PAD8(offset);
    
    *off_slices = offset;
    offset += num_slices * sizeof(struct AseSlice);
    offset = PAD8(offset);
    
    *off_strings = offset;
    offset += string_length;
    offset = PAD8(offset);

    *off_data = offset;
    offset += length;
    offset = PAD8(offset);
    
    *off_temp_buffer = offset;
    offset += max_compressed_size;
    offset = PAD8(offset);

    return (int)offset;
}

static inline int ase_decompress(struct AseFile* file, uint8_t* source, int source_size, uint8_t* buffer, int buffer_size, void** decompressor);
struct AseFile* ase_read_file(const char* path)
{
    struct AseParser parser = ase_open_parser(path);
    
    if (parser.error != ASE_NO_ERROR) {
        ase_close_parser(&parser);
        return NULL;
    }

    /* First pass: count items */
    int num_frames = 0, num_layers = 0, num_tags = 0;
    int num_pixel_data_blocks = 0;
    int num_palettes = 0;
    int num_slices = 0;
    int total_pixel_data_bytes = 0;
    int max_uncompress_size = 0;
    int max_compressed_size = 0;
    int total_uncompressed_size = 0;
    int string_data_size = 0;
    int bytes_per_pixel = 0;
    
    enum AseStructType type;
    while ((type = ase_parse_next(&parser)) != ASE_END_OF_FILE) {
        switch (type) {
            case ASE_HEADER: {
                ase_header_t* hdr = (ase_header_t*)parser.element;
                num_frames = hdr->frames;
                bytes_per_pixel = hdr->color_depth/8;
                break;
            }
            case ASE_LAYER: {
                ase_layer_chunk_t* layer = (ase_layer_chunk_t*)parser.element;
                num_layers++;
                string_data_size += layer->layer_name_length + 1;
                break;
            }
            case ASE_CEL: {
                ase_cel_t* cel = (ase_cel_t*)parser.element;
                if (cel->cel_type != CEL_LINKED)
                {
                    num_pixel_data_blocks++;
                    if (cel->cel_type == CEL_RAW_IMAGE || cel->cel_type == CEL_COMPRESSED_IMAGE)
                    {
                        int size = ((uint8_t*)parser.next_chunk_start) - &cel->pixels[0];
                        size = PAD8(size);
                        total_pixel_data_bytes += size;
                        if(size > max_compressed_size)
                            max_compressed_size = size;
                    }
                    if(cel->cel_type == CEL_COMPRESSED_IMAGE)
                    {
                        int uncompress_size = cel->w * cel->h * bytes_per_pixel;
                        if(uncompress_size > max_uncompress_size)
                            max_uncompress_size = uncompress_size;
                        total_uncompressed_size += PAD8(uncompress_size);
                    }
                }
                break;
            }
            case ASE_TAG: {
                ase_tag_t* tag = (ase_tag_t*)parser.element;
                string_data_size += tag->name_length + 1;
                num_tags++;
                break;
            }
            case ASE_PALETTE: {
                ase_palette_t* pal = (ase_palette_t*)parser.element;
                num_palettes++;
                break;
            }
            case ASE_SLICE: {
                ase_slice_t* slice = (ase_slice_t*)parser.element;
                num_slices++;
                string_data_size += slice->name_length + 1;
                break;
            }
            case ASE_USER_DATA: {
                ase_user_data_t* data = (ase_user_data_t*)parser.element;
                uint16_t* text_length_ptr = (uint16_t*)((uint8_t*)data + sizeof(ase_user_data_t));
                if(data->flag_has_text) string_data_size += *text_length_ptr + 1;
                break;
            }
            case ASE_ERROR: {
                ase_close_parser(&parser);
                return NULL;
                break;
            }
            default:
                break;
        }
    }
    
    if(total_uncompressed_size > total_pixel_data_bytes)
        total_pixel_data_bytes = total_uncompressed_size;
    int num_cels = num_layers * num_frames;

    int off_frames, off_layers, off_cels, off_tags, off_cel_header, off_palettes, off_slices, off_strings, off_cel_bytes, off_temp_buffer;
    int needed = calc_layout(num_frames, num_layers, num_cels, num_tags, num_pixel_data_blocks, num_palettes, num_slices, string_data_size, total_pixel_data_bytes, max_compressed_size,
                             &off_frames, &off_layers, &off_cels, &off_tags, 
                             &off_cel_header, &off_palettes, &off_slices, &off_strings, &off_cel_bytes, &off_temp_buffer);

    /* Second parsing pass: populate structures */
    ase_reset_parser(&parser);
    struct AseFile* file = malloc(needed);
    uint8_t* memory = (uint8_t*)file;
    memset(file, 0, sizeof(*file));
    
    file->data = (char*)memory;
    file->data_length = needed;
    
    file->frames = (struct AseFrame*)(memory + off_frames);
    file->layers = (struct AseLayer*)(memory + off_layers);
    file->cels = (struct AseCel*)(memory + off_cels);
    file->tags = (struct AseTag*)(memory + off_tags);
    file->pixel_data = (struct AsePixelData*)(memory + off_cel_header);
    file->palettes = (struct AsePalette*)(memory + off_palettes);
    file->slices = (struct AseSlice*)(memory + off_slices);
    
    file->num_frames = num_frames;
    file->num_layers = num_layers;
    file->num_cels = num_cels;
    file->num_tags = num_tags;
    file->num_pixel_data_blocks = num_pixel_data_blocks;
    file->num_palettes = num_palettes;
    file->num_slices = num_slices;

    for(int i = 0; i<num_cels; i++) file->cels[i] = (struct AseCel){.pixel_data_index = -1};

    char* string_data_tail_ptr = (char*)(memory + off_strings);
    uint8_t* cel_data_tail     = (uint8_t*)(memory + off_cel_bytes);
    uint8_t* temp_buffer = (uint8_t*)(memory + off_temp_buffer);
    
    int pixel_data_idx = 0;
    int cel_idx = 0;
    int palette_idx = 0;
    int tag_idx = 0;
    int slice_idx = 0;
    bool slice_key_read = false;
    int current_palette_entry_idx = 0;
    struct AsePalette* current_palette = NULL;

    int current_frame_idx = -1;
    int current_layer_idx = -1;
    struct AseUserData* current_user_data = NULL;
    int user_data_stride = 0;
    struct AseUserData* last_user_data = NULL;

    int transparency_index = -1;
    void* decompressor = NULL;
    while ((type = ase_parse_next(&parser)) != ASE_END_OF_FILE) {
        switch (type) {
            case ASE_HEADER: {
                ase_header_t* hdr = (ase_header_t*)parser.element;
                file->width = hdr->width;
                file->height = hdr->height;
                file->pixel_ratio_x = hdr->pixel_width;
                file->pixel_ratio_y = hdr->pixel_height;
                file->bytes_per_pixel = (hdr->color_depth == 32) ? 4 : 
                                         (hdr->color_depth == 16) ? 2 : 1;
                transparency_index = hdr->transparent_index;
                current_user_data = &file->user_data;
                break;
            }
            
            case ASE_FRAME_HEADER: {
                ase_frame_header_t* fh = (ase_frame_header_t*)parser.element;
                current_frame_idx++;
                if (current_frame_idx < num_frames)
                {
                    file->frames[current_frame_idx].duration = fh->duration_ms / 1000.0f;
                    file->frames[current_frame_idx].palette = current_palette;
                }
                cel_idx = ase_get_cel_idx(file, current_frame_idx, 0);
                current_layer_idx = -1;
                break;
            }
            
            case ASE_LAYER: {
                ase_layer_chunk_t* layer_data = (ase_layer_chunk_t*)parser.element;
                current_layer_idx++;

                struct AseLayer* layer = &file->layers[current_layer_idx];
                layer->blend_mode = layer_data->blend_mode;
                layer->opacity    = layer_data->opacity;
                layer->is_group   = (layer_data->type == LAYER_GROUP);
                layer->depth      = layer_data->child_level;
                layer->visible    = layer_data->flag_visible;

                layer->parent = -1;
                for(int i = current_layer_idx-1; i>=0; i--)
                if(file->layers[i].depth < layer->depth)
                {
                    layer->parent = i;
                    break;
                }
                /* Copy name */
                int name_len = layer_data->layer_name_length;
                memcpy(string_data_tail_ptr, layer_data->layer_name, name_len);
                string_data_tail_ptr[name_len] = '\0';
                layer->name = string_data_tail_ptr;
                string_data_tail_ptr += name_len + 1;
                current_user_data = &layer->user_data;
                break;
            }
            
            case ASE_CEL: {
                ase_cel_t* cel_elem = (ase_cel_t*)parser.element;
                cel_idx = current_frame_idx * num_layers + cel_elem->layer_idx;
                if (cel_idx < num_cels)
                {
                    struct AseCel* cel = &file->cels[cel_idx];
                    
                    cel->x = cel_elem->x;
                    cel->y = cel_elem->y;
                    cel->opacity = cel_elem->opacity;
                    cel->z_index = cel_elem->z_index;
                    cel->draw_order = ((int32_t)(cel_elem->layer_idx + cel->z_index) << 16) | (uint16_t)cel->z_index;
                    
                    if(cel_elem->cel_type == CEL_LINKED)
                    {
                        //printf("Linked Cell from %d to %d", cel_elem->linked.frame, current_frame_idx);
                        cel->pixel_data_index = file->cels[ase_get_cel_idx(file, cel_elem->linked.frame, cel_elem->layer_idx)].pixel_data_index;
                    }
                    else if(cel_elem->cel_type == CEL_COMPRESSED_IMAGE || cel_elem->cel_type == CEL_RAW_IMAGE)
                    {
                        cel->pixel_data_index = pixel_data_idx;
                        struct AsePixelData* pixels = &file->pixel_data[pixel_data_idx];
                        pixels->origin_cel = cel_idx;
                            
                        pixels->w = cel_elem->w;
                        pixels->h = cel_elem->h;
                        pixels->data = cel_data_tail;
                        
                        if(cel_elem->cel_type == CEL_COMPRESSED_IMAGE)
                        {
                            int src_length = ((uint8_t*)parser.next_chunk_start) - &cel_elem->pixels[0];
                            int dst_length = pixels->w * pixels->h * bytes_per_pixel;
                            // sinfl needs 8 byte aligned input
                            memcpy(temp_buffer, cel_elem->pixels, src_length);
                            pixels->length = ase_decompress(file, temp_buffer, src_length, pixels->data, PAD8(dst_length), &decompressor);
                            if(pixels->length == -1)
                            {
                                fprintf(stderr, "ase_read: corrupted data block encountered during decompression");
                                ase_free_file(file);
                                return NULL;
                            }
                            cel_data_tail += PAD8(pixels->length);
                        }
                        else if(cel_elem->cel_type == CEL_RAW_IMAGE)
                        {
                            int length = ((uint8_t*)parser.next_chunk_start) - &cel_elem->pixels[0];
                            memcpy(cel_data_tail, cel_elem->pixels, length);
                            pixels->length = length;
                            cel_data_tail += PAD8(length);
                        }
                        
                        pixel_data_idx++;
                    }
                    
                    current_user_data = &cel->user_data;
                }
                cel_idx++;
                break;
            }
            case ASE_TAGS: {
                ase_tags_t* tags_data = (ase_tags_t*)parser.element;
                current_user_data = &file->tags[tag_idx].user_data;
                last_user_data    = &file->tags[tag_idx+tags_data->num_tags-1].user_data;
                user_data_stride  = (uint8_t*)(&file->tags[tag_idx+1].user_data) - (uint8_t*)(&file->tags[tag_idx].user_data);
                break;
            }
            case ASE_TAG: {
                ase_tag_t* tag_data = (ase_tag_t*)parser.element;
                if (tag_idx < num_tags) {
                    struct AseTag* tag = &file->tags[tag_idx];
                    
                    tag->start_frame = tag_data->from;
                    tag->end_frame = tag_data->to;
                    
                    int name_len = tag_data->name_length;
                    memcpy(string_data_tail_ptr, tag_data->name, name_len);
                    string_data_tail_ptr[name_len] = '\0';
                    tag->name = string_data_tail_ptr;
                    string_data_tail_ptr += name_len + 1;
                    tag_idx++;
                }
                break;
            }
            
            case ASE_PALETTE: {
                ase_palette_t* pal_data = (ase_palette_t*)parser.element;
                struct AsePalette* new_palette = &file->palettes[palette_idx];
                if(current_palette == NULL)
                    memset(new_palette, 0x000000FF, sizeof(struct AsePalette));
                else
                    memcpy(new_palette, current_palette, sizeof(struct AsePalette));
                current_palette = new_palette;
                if (current_frame_idx >= 0 && current_frame_idx < num_frames)
                    file->frames[current_frame_idx].palette = current_palette;
                
                current_palette_entry_idx = pal_data->first_index;
                palette_idx++;
                break;
            }
            
            case ASE_PALETTE_ENTRY: {
                ase_palette_entry_t* entry = (ase_palette_entry_t*)parser.element;
                if (current_palette && current_palette_entry_idx < 256)
                {
                    current_palette->colors[current_palette_entry_idx] = entry->rgba;
                    if(current_palette_entry_idx == transparency_index)
                        current_palette->colors[current_palette_entry_idx] &= 0xFFFFFF00; // Set alpha to 0.
                    current_palette_entry_idx++;
                }
                break;
            }
            
            case ASE_USER_DATA: {
                ase_user_data_t* ud = (ase_user_data_t*)parser.element;
                if (current_user_data != NULL)
                {
                    uint8_t* ptr = (uint8_t*)ud + sizeof(ase_user_data_t);
                    
                    if (ud->flag_has_text)
                    {
                        ase_text_t* text = (ase_text_t*)ptr;
                        memcpy(string_data_tail_ptr, text->text, text->text_length);
                        string_data_tail_ptr[text->text_length] = '\0';
                        current_user_data->text = string_data_tail_ptr;
                        string_data_tail_ptr += text->text_length + 1;
                        ptr += sizeof(ase_text_t) + text->text_length;
                    }
                    
                    if (ud->flag_has_color)
                    {
                        ase_color_t* color = (ase_color_t*)ptr;
                        current_user_data->color = color->rgba;
                    }

                    current_user_data = (struct AseUserData*)((uint8_t*)current_user_data + user_data_stride);
                    if(current_user_data > last_user_data) current_user_data = NULL;
                }
                break;
            }
            
            case ASE_SLICE: {
                ase_slice_t* slice_data = (ase_slice_t*)parser.element;
                slice_key_read = false;
                struct AseSlice* slice = &file->slices[slice_idx];
                int name_len = slice_data->name_length;
                memcpy(string_data_tail_ptr, slice_data->name, name_len);
                string_data_tail_ptr[name_len] = '\0';
                slice->name = string_data_tail_ptr;
                current_user_data = &slice->user_data;
                last_user_data    = NULL;
                string_data_tail_ptr += name_len + 1;
                slice_idx++;
                break;
            }

            case ASE_SLICE_KEY: {
                if (!slice_key_read && slice_idx > 0) {
                    ase_slice_key_t* key = (ase_slice_key_t*)parser.element;
                    struct AseSlice* slice = &file->slices[slice_idx - 1];
                    slice->slice_x = key->slice_x;
                    slice->slice_y = key->slice_y;
                    slice->slice_w = key->slice_w;
                    slice->slice_h = key->slice_h;
                    ase_slice_t* slice_data = parser.current_slice;
                    if (slice_data->flag_has_9patch) {
                        slice->is_9patch = true;
                        slice->patch_center_x = key->center_and_pivot[0];
                        slice->patch_center_y = key->center_and_pivot[1];
                        slice->patch_center_w = key->center_and_pivot[2];
                        slice->patch_center_h = key->center_and_pivot[3];
                    }
                    if (slice_data->flag_has_pivot) {
                        slice->has_pivot = true;
                        slice->pivot_x = key->center_and_pivot[4];
                        slice->pivot_y = key->center_and_pivot[5];
                    }
                    slice_key_read = true;
                }
                break;
            }

            default:
                break;
        }
    }
    
    ase_close_parser(&parser);
    #ifdef ASE_USE_LIBDEFLATE
    libdeflate_free_decompressor(decompressor);
    #endif

    return file;
}

#if defined(ASE_USE_LIBDEFLATE)
#include <libdeflate.h>
#elif defined(ASE_USE_MINIZ)
#include "miniz.h"
#else
#define SINFL_IMPLEMENTATION
#include "sinfl.h"
#endif

static inline int ase_decompress(struct AseFile* file, uint8_t* source, int source_size, uint8_t* buffer, int buffer_size, void** decompressor)
{
    #if defined(ASE_USE_LIBDEFLATE)
    buffer = buffer;
    struct libdeflate_decompressor* decomp = *decompressor;
    if(*decompressor == NULL)
    {
        *decompressor = libdeflate_alloc_decompressor();
        decomp = *decompressor;
    }
    size_t actual_out_nbytes = 0;
    if (libdeflate_zlib_decompress(decomp, source, (size_t)source_size, buffer, buffer_size, &actual_out_nbytes) != LIBDEFLATE_SUCCESS)
        return -1;
    return actual_out_nbytes;
    #elif defined(ASE_USE_MINIZ)
    uLongf size = buffer_size;
    if(uncompress(buffer, &size, source, (unsigned long)source_size) != Z_OK)
        return -1;
    return size;
    #else
    return zsinflate(buffer, buffer_size, source, source_size);
    #endif
}

void ase_free_file(struct AseFile* file)
{
    free(file);
}

static inline void ase_copy_pixels_indexed(
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y,
    const uint32_t* restrict palette)
{
    for (int y = 0; y < src_h; y++)
    {
        uint8_t* from = src + (src_y + y) * src_pitch + src_x;
        uint32_t* to = (uint32_t*)(dst + (dst_y + y) * dst_pitch + dst_x * 4);
        for (int x = 0; x < src_w; x++)
            *to++ = palette[*from++];
    }
}

static inline void ase_copy_pixels_grayscale(
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    {
        uint8_t* from = src + (src_y + y) * src_pitch + src_x * 2;
        uint32_t* to = (uint32_t*)(dst + (dst_y + y) * dst_pitch + dst_x * 4);
        for (int x = 0; x < src_w; x++)
        {
            *to++ = (from[0]) | (from[0] << 8) | (from[0] << 16) | (from[1] << 24);
            from += 2;
        }
    }
}

static inline void ase_copy_pixels_rgba(
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    {
        uint32_t* from = (uint32_t*)(src + (src_y + y) * src_pitch + src_x * 4);
        uint32_t* to = (uint32_t*)(dst + (dst_y + y) * dst_pitch + dst_x * 4);
        memcpy(to, from, src_w * 4);
    }
}


bool ase_copy_pixels(struct AseFile* file, int pixel_data_index, uint8_t* dst, int pitch, int dst_x, int dst_y)
{
    if(file == NULL || dst == NULL ||pixel_data_index < 0 || pixel_data_index >= file->num_pixel_data_blocks)
        return false;
    if(file->bytes_per_pixel != 1 && file->bytes_per_pixel != 2 && file->bytes_per_pixel != 4)
        return false;
    struct AsePixelData* pixels = &file->pixel_data[pixel_data_index];
    if(pixels->data == NULL || pixels->length <= 0)
        return false;

    uint8_t* src = pixels->data;
    int src_pitch = pixels->w * file->bytes_per_pixel;
    int frame = ase_get_frame_from_cel_idx(file, pixels->origin_cel);

    int x, y, w, h;
    x = pixels->is_trimmed ? pixels->trimmed_x :         0;
    y = pixels->is_trimmed ? pixels->trimmed_y :         0;
    w = pixels->is_trimmed ? pixels->trimmed_w : pixels->w;
    h = pixels->is_trimmed ? pixels->trimmed_h : pixels->h;
    if(w < 1 || h < 1) return true; // Blitting is a NOOP.

    if(file->bytes_per_pixel == 1)
        ase_copy_pixels_indexed(src, src_pitch, x, y, w, h, dst, pitch, dst_x, dst_y, file->frames[frame].palette->colors);
    else if(file->bytes_per_pixel == 2)
        ase_copy_pixels_grayscale(src, src_pitch, x, y, w, h, dst, pitch, dst_x, dst_y);
    else if(file->bytes_per_pixel == 4)
        ase_copy_pixels_rgba(src, src_pitch, x, y, w, h, dst, pitch, dst_x, dst_y);
    return true;
}

bool ase_trim_pixels(struct AseFile* file, int pixel_data_index)
{
    if(file == NULL || pixel_data_index < 0 || pixel_data_index >= file->num_pixel_data_blocks)
        return false;
    if(file->bytes_per_pixel != 1 && file->bytes_per_pixel != 2 && file->bytes_per_pixel != 4)
        return false;
    struct AsePixelData* pixels = &file->pixel_data[pixel_data_index];
    if(pixels->data == NULL || pixels->length <= 0)
        return false;

    uint8_t* src = pixels->data;
    int src_pitch = pixels->w * file->bytes_per_pixel;
    int frame = ase_get_frame_from_cel_idx(file, pixels->origin_cel);

    int min_x = pixels->w, min_y = pixels->h;
    int max_x = -1,        max_y = -1;
    #define UPDATE_BOUNDS(x, y) { if (x < min_x) min_x = x;\
                    if (x > max_x) max_x = x;\
                    if (y < min_y) min_y = y;\
                    if (y > max_y) max_y = y; }
    if(file->bytes_per_pixel == 1) // indexed
    {
        int frame = ase_get_frame_from_cel_idx(file, pixels->origin_cel);
        struct AsePalette* pal = file->frames[frame].palette;
        if(!pal) return false;
        for(int y = 0; y < pixels->h; y++)
        {
            uint8_t* scanline = src + y * src_pitch;
            for(int x = 0; x < pixels->w; x++)
            if(((pal->colors[scanline[x]] >> 24) & 0xFF) != 0) UPDATE_BOUNDS(x, y);
        }
    }
    else if(file->bytes_per_pixel == 2) // grayscale + alpha
    {
        for(int y = 0; y < pixels->h; y++)
        {
            uint8_t* scanline = src + y * src_pitch;
            for(int x = 0; x < pixels->w; x++)
            if(scanline[x * 2 + 1] != 0) UPDATE_BOUNDS(x, y);
        }
    }
    else if(file->bytes_per_pixel == 4)
    {
        for(int y = 0; y < pixels->h; y++)
        {
            uint8_t* scanline = src + y * src_pitch;
            for (int x = 0; x < pixels->w; x++)
            if(scanline[x * 4 + 3] != 0) UPDATE_BOUNDS(x, y);
        }
    }
    pixels->trimmed_x = (uint16_t)min_x;
    pixels->trimmed_y = (uint16_t)min_y;
    pixels->trimmed_w = max_x < min_x ? 0 : (uint16_t)(max_x - min_x + 1);
    pixels->trimmed_h = max_y < min_y ? 0 : (uint16_t)(max_y - min_y + 1);
    pixels->is_trimmed = true;
    return true;
}

// --------------- Software Rendering ------------- //
void ase_draw_cel_partial(struct AseFile* file, int frame, int layer, int src_x, int src_y, int src_w, int src_h, uint8_t* dst, int pitch, int offset_x, int offset_y)
{
    int cel_idx = ase_get_cel_idx(file, frame, layer);
    struct AseCel*   cel = &file->cels[cel_idx];
    struct AseLayer* lay = &file->layers[layer];

    if (cel->pixel_data_index < 0) return;

    struct AsePixelData* pd      = &file->pixel_data[cel->pixel_data_index];
    struct AseFrame*     fr      = &file->frames[frame];
    struct AsePalette*   palette = fr->palette;

    // clip
    if (src_x < 0) { src_w += src_x; offset_x -= src_x; src_x = 0; }
    if (src_y < 0) { src_h += src_y; offset_y -= src_y; src_y = 0; }
    if (src_x + src_w > (int)pd->w) src_w = pd->w - src_x;
    if (src_y + src_h > (int)pd->h) src_h = pd->h - src_y;
    if (src_w <= 0 || src_h <= 0) return;

    uint8_t opacity = (uint8_t)((cel->opacity * lay->opacity) / 255);
    ase_blend_pixels(lay->blend_mode, opacity, palette ? palette->colors : NULL, pd->data, file->bytes_per_pixel, pd->w * file->bytes_per_pixel, src_x, src_y, src_w, src_h, dst, pitch, offset_x + cel->x, offset_y + cel->y);
}

void ase_draw_frame_partial(struct AseFile* file, int frame, int src_x, int src_y, int src_w, int src_h, uint8_t* dst, int pitch, int offset_x, int offset_y)
{
    // sort
    int order[256];
    int count = file->num_layers;
    assert(count < 256);
    for (int i = 0; i < count; i++) order[i] = i;
    for (int i = 1; i < count; i++)
    {
        int layer = order[i];
        int32_t key_order = file->cels[ase_get_cel_idx(file, frame, layer)].draw_order;
        int j = i - 1;
        while(j >= 0 && file->cels[ase_get_cel_idx(file, frame, order[j])].draw_order > key_order)
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = layer;
    }
    // draw
    for(int i = 0; i < count; i++)
    if(file->layers[order[i]].visible)
        ase_draw_cel_partial(file, frame, order[i], src_x, src_y, src_w, src_h, dst, pitch, offset_x, offset_y);
}

// --------- Software Rendering Blending ---------- //
static inline int hsl_lum(int r, int g, int b) {
    return (r*299 + g*587 + b*114) / 1000; // weighted luminance (Rec. 601)
}

static inline int hsl_sat(int r, int g, int b) {
    int cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return cmax - cmin;
}

static inline void hsl_clip_color(int* r, int* g, int* b) {
    int l = hsl_lum(*r, *g, *b);
    int cmin = *r < *g ? (*r < *b ? *r : *b) : (*g < *b ? *g : *b);
    int cmax = *r > *g ? (*r > *b ? *r : *b) : (*g > *b ? *g : *b);
    if(cmin < 0 && (l - cmin) != 0) {
        *r = l + (*r - l) * l / (l - cmin);
        *g = l + (*g - l) * l / (l - cmin);
        *b = l + (*b - l) * l / (l - cmin);
    }
    if (cmax > 255 && (cmax - l) != 0) {
        *r = l + (*r - l) * (255 - l) / (cmax - l);
        *g = l + (*g - l) * (255 - l) / (cmax - l);
        *b = l + (*b - l) * (255 - l) / (cmax - l);
    }
}

static inline void hsl_set_lum(int* r, int* g, int* b, int l) {
    int d = l - hsl_lum(*r, *g, *b);
    *r += d; *g += d; *b += d;
    hsl_clip_color(r, g, b);
}

static inline void hsl_set_sat(int* r, int* g, int* b, int s) {
    // sort pointers: cmin <= cmid <= cmax
    int *cmin, *cmid, *cmax;
    if (*r <= *g && *r <= *b) { cmin = r; cmax = *g >= *b ? g : b; cmid = *g >= *b ? b : g; }
    else if (*g <= *r && *g <= *b) { cmin = g; cmax = *r >= *b ? r : b; cmid = *r >= *b ? b : r; }
    else { cmin = b; cmax = *r >= *g ? r : g; cmid = *r >= *g ? g : r; }

    if (*cmax > *cmin) {
        *cmid = (*cmid - *cmin) * s / (*cmax - *cmin);
        *cmax = s;
    } else {
        *cmid = *cmax = 0;
    }
    *cmin = 0;
}

static inline uint8_t sqrt_u8(uint8_t x) {
    return (uint8_t)(sqrtf(x / 255.0f) * 255.0f + 0.5f);
}

// These do exactly the same thing as the generic version, however GCC gets confused by the noops and doesn't vectorize because of them, hence why we needed to hoist these out.
// Generally, clang on -O3 vectorizes all possible versions, GCC on -O3 only vectorizes the direct_4 and direct_2 versions.
static inline void ase_blend_pixels_direct_4(
    uint8_t opacity,
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    for (int x = 0; x < src_w; x++)
    {
        uint8_t* from = src + (src_y+y)*src_pitch + (src_x+x)*4;
        uint8_t*   to = dst + (dst_y+y)*dst_pitch + (dst_x+x)*4;
        uint8_t  rgba[4] = {from[0], from[1], from[2], from[3]};
        rgba[3] = rgba[3] * opacity / 255;
        to[3] = rgba[3] + to[3] * (255 - rgba[3]) / 255;
        for(int i = 0; i < 3; i++)
            to[i] = (rgba[i]*rgba[3] + to[i] * (255 - rgba[3]))/255;
    }
}

static inline void ase_blend_pixels_direct_2(
    uint8_t opacity,
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    for (int x = 0; x < src_w; x++)
    {
        uint8_t* from = src + (src_y+y)*src_pitch + (src_x+x)*2;
        uint8_t*   to = dst + (dst_y+y)*dst_pitch + (dst_x+x)*4;
        uint8_t  rgba[4] = {from[0], from[0], from[0], from[1]};
        rgba[3] = rgba[3] * opacity / 255;
        to[3] = rgba[3] + to[3] * (255 - rgba[3]) / 255;
        for(int i = 0; i < 3; i++)
            to[i] = (rgba[i]*rgba[3] + to[i] * (255 - rgba[3]))/255;
    }
}

static inline void ase_blend_pixels_direct_1(
    uint8_t opacity,
    const uint32_t* restrict palette,
    uint8_t* restrict src, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    for (int x = 0; x < src_w; x++)
    {
        uint8_t* from = src + (src_y+y)*src_pitch + (src_x+x)*1;
        uint8_t*   to = dst + (dst_y+y)*dst_pitch + (dst_x+x)*4;
        uint32_t color = palette[from[0]];
        uint8_t*  rgba = (uint8_t*)&color;
        rgba[3] = rgba[3] * opacity / 255;
        to[3] = rgba[3] + to[3] * (255 - rgba[3]) / 255;
        for(int i = 0; i < 3; i++)
            to[i] = (rgba[i]*rgba[3] + to[i] * (255 - rgba[3]))/255;
    }
}

static inline void ase_blend_pixels_generic(
    uint32_t (*fetch_fn)(const uint8_t* from, const uint32_t* palette),
    uint8_t (*blend_fn)(uint8_t a, uint8_t b),
    void (*transform_fn)(uint8_t* rgba, const uint8_t* background),
    uint8_t opacity,  const uint32_t* restrict palette,
    uint8_t* restrict src, int src_bytes_per_pixel, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    for (int y = 0; y < src_h; y++)
    for (int x = 0; x < src_w; x++)
    {
        const uint8_t* from = src + (src_y+y)*src_pitch + (src_x+x)*src_bytes_per_pixel;
        uint8_t*       to   = dst + (dst_y+y)*dst_pitch + (dst_x+x)*4;

        uint32_t color = fetch_fn(from, palette);
        uint8_t rgba[4];
        memcpy(rgba, &color, 4);
        rgba[3] = rgba[3] * opacity / 255;
        uint8_t old_alpha = to[3];
        to[3] = rgba[3] + to[3] * (255 - rgba[3]) / 255;

        if(blend_fn)
        for (int i = 0; i < 3; i++)
            rgba[i] = blend_fn(to[i], rgba[i]);
        if(transform_fn)
            transform_fn(rgba, to);
        for(int i = 0; i < 3; i++)
            to[i] = (rgba[i]*rgba[3] + to[i] * (255 - rgba[3]))/255;
    }
}

static inline uint32_t fetch_1(const uint8_t* p, const uint32_t* pal){ return pal[*p]; }
static inline uint32_t fetch_2(const uint8_t* p, const uint32_t* pal){ uint8_t rgba[4] = {p[0], p[0], p[0], p[1]}; return *(uint32_t*)rgba; }
static inline uint32_t fetch_4(const uint8_t* p, const uint32_t* pal){ return *(uint32_t*)p; }

static inline uint8_t b_normal     (uint8_t a, uint8_t b) { return b; }
static inline uint8_t b_multiply   (uint8_t a, uint8_t b) { return (uint16_t)(a*b)/255; }
static inline uint8_t b_screen     (uint8_t a, uint8_t b) { return a + b - (uint16_t)(a*b)/255; }
static inline uint8_t b_darken     (uint8_t a, uint8_t b) { return a < b ? a : b; }
static inline uint8_t b_lighten    (uint8_t a, uint8_t b) { return a > b ? a : b; }
static inline uint8_t b_difference (uint8_t a, uint8_t b) { return a > b ? a - b : b - a; }
static inline uint8_t b_addition   (uint8_t a, uint8_t b) { return (uint8_t)(a + b) < a ? 255 : a + b; }
static inline uint8_t b_subtract   (uint8_t a, uint8_t b) { return (uint8_t)(a - b) > a ? 0   : a - b; }
static inline uint8_t b_divide     (uint8_t a, uint8_t b) { return b == 0 ? 255 : (uint16_t)a*255/b > 255 ? 255 : (uint16_t)a*255/b; }
static inline uint8_t b_exclusion  (uint8_t a, uint8_t b) { return a + b - (uint16_t)(2*a*b)/255; }
static inline uint8_t b_overlay    (uint8_t a, uint8_t b) { return a < 128 ? 2*a*b/255 : 255 - 2*(255-a)*(255-b)/255; }
static inline uint8_t b_hard_light (uint8_t a, uint8_t b) { return b < 128 ? 2*a*b/255 : 255 - 2*(255-a)*(255-b)/255; }
static inline uint8_t b_soft_light (uint8_t a, uint8_t b) { return b < 128 ? a - (uint16_t)(255-2*b)*a/255*(255-a)/255 : a + (uint16_t)(2*b-255)*(sqrt_u8(a)-a)/255; }
static inline uint8_t b_color_dodge(uint8_t a, uint8_t b) { return b == 255 ? 255 : (uint16_t)a*255/(255-b) > 255 ? 255 : (uint16_t)a*255/(255-b); }
static inline uint8_t b_color_burn (uint8_t a, uint8_t b) { return b == 0   ? 0   : (int16_t)(255 - (uint16_t)(255-a)*255/b) < 0 ? 0 : 255 - (uint16_t)(255-a)*255/b; }

static inline void t_hue(uint8_t* rgba, const uint8_t* bg) {
    int r = rgba[0], g = rgba[1], b = rgba[2];
    hsl_set_sat(&r, &g, &b, hsl_sat(bg[0], bg[1], bg[2]));
    hsl_set_lum(&r, &g, &b, hsl_lum(bg[0], bg[1], bg[2]));
    rgba[0] = r; rgba[1] = g; rgba[2] = b;
}

static inline void t_sat(uint8_t* rgba, const uint8_t* bg) {
    int r = bg[0], g = bg[1], b = bg[2];
    hsl_set_sat(&r, &g, &b, hsl_sat(rgba[0], rgba[1], rgba[2]));
    hsl_set_lum(&r, &g, &b, hsl_lum(bg[0], bg[1], bg[2]));
    rgba[0] = r; rgba[1] = g; rgba[2] = b;
}

static inline void t_lum(uint8_t* rgba, const uint8_t* bg) {
    int r = bg[0], g = bg[1], b = bg[2];
    hsl_set_lum(&r, &g, &b, hsl_lum(rgba[0], rgba[1], rgba[2]));
    rgba[0] = r; rgba[1] = g; rgba[2] = b;
}

static inline void t_color(uint8_t* rgba, const uint8_t* bg) {
    int r = rgba[0], g = rgba[1], b = rgba[2];
    hsl_set_lum(&r, &g, &b, hsl_lum(bg[0], bg[1], bg[2]));
    rgba[0] = r; rgba[1] = g; rgba[2] = b;
}

#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

void ase_blend_pixels(
    int blend_mode, uint8_t opacity, const uint32_t* restrict palette,
    uint8_t* restrict src, int src_bytes_per_pixel, int src_pitch, int src_x, int src_y, int src_w, int src_h,
    uint8_t* restrict dst, int dst_pitch, int dst_x, int dst_y)
{
    if(blend_mode < 0 || blend_mode >= BLEND_MAX) return;
    if(src_bytes_per_pixel != 1 && src_bytes_per_pixel != 2 && src_bytes_per_pixel != 4) return;
    #define DISPATCH(BLEND_FN, POST_FN)\
             if(src_bytes_per_pixel == 1) ase_blend_pixels_generic(fetch_1, BLEND_FN, POST_FN, opacity, palette, src, src_bytes_per_pixel, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);\
        else if(src_bytes_per_pixel == 2) ase_blend_pixels_generic(fetch_2, BLEND_FN, POST_FN, opacity, palette, src, src_bytes_per_pixel, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);\
        else if(src_bytes_per_pixel == 4) ase_blend_pixels_generic(fetch_4, BLEND_FN, POST_FN, opacity, palette, src, src_bytes_per_pixel, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);
    if(LIKELY(blend_mode == BLEND_NORMAL))
    {
        if(src_bytes_per_pixel == 4) ase_blend_pixels_direct_4(opacity, src, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);
        if(src_bytes_per_pixel == 2) ase_blend_pixels_direct_2(opacity, src, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);
        if(src_bytes_per_pixel == 1) ase_blend_pixels_direct_1(opacity, palette, src, src_pitch, src_x, src_y, src_w, src_h, dst, dst_pitch, dst_x, dst_y);
    }
    else
    {
        switch(blend_mode)
        {
            case BLEND_MULTIPLY:    DISPATCH(b_multiply,    NULL); break;
            case BLEND_SCREEN:      DISPATCH(b_screen,      NULL); break;
            case BLEND_DARKEN:      DISPATCH(b_darken,      NULL); break;
            case BLEND_LIGHTEN:     DISPATCH(b_lighten,     NULL); break;
            case BLEND_DIFFERENCE:  DISPATCH(b_difference,  NULL); break;
            case BLEND_ADDITION:    DISPATCH(b_addition,    NULL); break;
            case BLEND_SUBTRACT:    DISPATCH(b_subtract,    NULL); break;
            case BLEND_DIVIDE:      DISPATCH(b_divide,      NULL); break;
            case BLEND_EXCLUSION:   DISPATCH(b_exclusion,   NULL); break;
            case BLEND_OVERLAY:     DISPATCH(b_overlay,     NULL); break;
            case BLEND_HARD_LIGHT:  DISPATCH(b_hard_light,  NULL); break;
            case BLEND_SOFT_LIGHT:  DISPATCH(b_soft_light,  NULL); break;
            case BLEND_COLOR_DODGE: DISPATCH(b_color_dodge, NULL); break;
            case BLEND_COLOR_BURN:  DISPATCH(b_color_burn,  NULL); break;
            case BLEND_COLOR:       DISPATCH(NULL, t_color); break;
            case BLEND_HUE:         DISPATCH(NULL, t_hue); break;
            case BLEND_SATURATION:  DISPATCH(NULL, t_sat); break;
            case BLEND_LUMINOSITY:  DISPATCH(NULL, t_lum); break;
        }
    }
}
// ------------- Low Level API -------------- //
struct AseParser ase_open_parser(const char* path)
{
    struct AseParser p = { .cur_frame = -1 };
    FILE* f = fopen(path, "rb");
    if (!f) { p.error = ASE_CANT_OPEN; return p; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { p.error = ASE_CANT_OPEN; fclose(f); return p; }

    p.data_size = (int)sz;
    p.data = (char*)malloc(p.data_size);
    assert(p.data != NULL);
    int read_size = (int)fread(p.data, 1, p.data_size, f);
    assert(read_size == p.data_size);
    if(((ase_header_t*)p.data)->magic != 0xA5E0)
        p.error = ASE_UNKNOWN_FILE_FORMAT;
    p.cur = p.data;
    p.file_header = (ase_header_t*)p.cur;
    p.filter = INCLUDE_FRAME | INCLUDE_LAYER | INCLUDE_CEL | INCLUDE_TAGS | INCLUDE_USER_DATA | INCLUDE_PALETTE | INCLUDE_SLICE;
    fclose(f);
    return p;
}

void ase_reset_parser(struct AseParser* p)
{
    char* data_ptr = p->data;
    int data_size = p->data_size;
    uint32_t filter = p->filter;
    memset(p, 0, sizeof(struct AseParser));
    p->data = data_ptr;
    p->data_size = data_size;
    p->filter = filter;
    p->cur = data_ptr;
    p->file_header = (ase_header_t*)data_ptr;
    p->cur_frame = -1;
    if(((ase_header_t*)p->data)->magic != 0xA5E0)
        p->error = ASE_UNKNOWN_FILE_FORMAT;
}

void ase_close_parser(struct AseParser* p)
{
    free(p->data);
    p->data = NULL;
}

static const char* ase_struct_type_name(enum AseStructType type)
{
    switch (type)
    {
        case ASE_END_OF_FILE:         return "END_OF_FILE";
        case ASE_HEADER:              return "HEADER";
        case ASE_FRAME_HEADER:        return "FRAME_HEADER";
        case ASE_CHUNK_HEADER:        return "CHUNK_HEADER";
        case ASE_LAYER:               return "LAYER";
        case ASE_CEL:                 return "CEL";
        case ASE_CEL_EXTRA:           return "CEL_EXTRA";
        case ASE_COLOR_PROFILE:       return "COLOR_PROFILE";
        case ASE_EXTERNAL_FILES:      return "EXTERNAL_FILES";
        case ASE_EXTERNAL_FILE_ENTRY: return "EXTERNAL_FILE_ENTRY";
        case ASE_TAGS:                return "TAGS";
        case ASE_TAG:                 return "TAG";
        case ASE_PALETTE:             return "PALETTE";
        case ASE_PALETTE_ENTRY:       return "PALETTE_ENTRY";
        case ASE_USER_DATA:           return "USER_DATA";
        case ASE_SLICE:               return "SLICE";
        case ASE_SLICE_KEY:           return "SLICE_KEY";
        case ASE_TILESET:             return "TILESET";
        default:                      return "UNKNOWN";
    }
}

void ase_skip_chunk(struct AseParser* p)
{
    // Multiple Skips in a row, make sure to parse the skipped chunks and frames so that parser state remains well-defined.
    if(p->cur == p->next_chunk_start)
        ase_parse_next(p);
    p->cur = p->next_chunk_start;
}

void ase_skip_frame(struct AseParser* p)
{
    //printf("-> SKIPPED FRAME\n");
    // Multiple Skips in a row, make sure to parse the skipped chunks and frames so that parser state remains well-defined.
    if(p->cur == p->next_frame_start)
        ase_parse_next(p);
    p->cur = p->next_frame_start;
}

static inline void* advance(struct AseParser* p, int size, enum AseStructType type)
{
    char* old_parser_cur = p->cur;
    p->element = old_parser_cur;
    p->element_size = size;
    p->element_type = type;
    p->cur += size;
    assert(size != 0);
    /*printf("[%16.16s (%04X): Offset % 4d Size %d]", ase_struct_type_name(p->element_type), p->element_type, ((char*)p->element - p->data), p->element_size);
    if(p->cur_chunk_remaining_entries > 0)
        printf(" +%d", p->cur_chunk_remaining_entries);
    printf("\n");*/
    return old_parser_cur;
}

enum AseStructType ase_parse_next(struct AseParser* p)
{
    if((p->cur - p->data) >= p->data_size || p->error != ASE_NO_ERROR) return ASE_END_OF_FILE;
    if(p->cur == p->data) // Header
    {
        p->nesting_depth = -1;
        advance(p, sizeof(ase_header_t), ASE_HEADER);
        return ASE_HEADER;
    }
    else // Frame Data`
    {
        if(p->cur_frame_remaining_chunks == 0  || p->cur == p->next_frame_start)
        {
            ase_frame_header_t* header = (ase_frame_header_t*)advance(p, sizeof(ase_frame_header_t), ASE_FRAME_HEADER);
            if(header->magic != 0xF1FA){ fprintf(stderr, "ase-read: encountered wrong magic bytes\n"); p->error = ASE_PARSER_ERROR; return ASE_ERROR; }
            p->cur_frame_remaining_chunks = (header->new_chunk_count != 0 ? header->new_chunk_count : header->old_chunk_count) + 1;
            p->next_frame_start = (char*)header + header->byte_length;
            p->next_chunk_start = (char*)header + sizeof(ase_frame_header_t);
            //printf("-- Next Frame %d Next chunk %d\n", p->next_frame_start - p->data, p->next_chunk_start - p->data);
            p->cur_frame++;
            p->nesting_depth = 0;
            p->current_frame_header = header;
            if(p->filter & INCLUDE_FRAME)
                return ASE_FRAME_HEADER;
            else
                return ase_parse_next(p);
        }
        else
        {
            if(p->nesting_depth == 2 && p->cur_chunk_remaining_entries == 0)
                p->nesting_depth = 0;
            
            if(p->nesting_depth == 0 || p->cur == p->next_chunk_start)
            {
                ase_chunk_header_t* header = (ase_chunk_header_t*)advance(p, sizeof(ase_chunk_header_t), ASE_CHUNK_HEADER);
                if(header->chunk_size >= 0x10000000){ fprintf(stderr, "ase-read: chunk size over 256MB, likely corrupted\n"); p->error = ASE_PARSER_ERROR; return ASE_ERROR; }
                p->current_chunk_header = header;
                p->next_chunk_start = (char*)header + header->chunk_size;
                p->cur_frame_remaining_chunks--;
                p->cur_chunk_remaining_entries = 0;
                p->nesting_depth = p->cur == p->next_chunk_start ? 0 : 1;
                p->current_chunk_type = header->chunk_type;
                //printf("-- Chunk Type: %X; Chunk Size: %d; Next Chunk: %d\n", header->chunk_type, header->chunk_size, p->next_chunk_start - p->data);
                bool include = true;
                switch(header->chunk_type)
                {
                    case CHUNK_CEL:
                        include = include && (p->filter & INCLUDE_CEL);
                        break;
                    case CHUNK_CEL_EXTRA:
                        include = include && (p->filter & INCLUDE_CEL_EXTRA);
                        break;
                    case CHUNK_COLOR_PROFILE:
                        include = include && (p->filter & INCLUDE_COLOR_PROFILE);
                        break;
                    case CHUNK_EXTERNAL_FILES:
                        include = include && (p->filter & INCLUDE_EXTERNAL_FILES);
                        break;
                    case CHUNK_LAYER:
                        include = include && (p->filter & INCLUDE_LAYER);
                        break;
                    case CHUNK_PALETTE:
                        include = include && (p->filter & INCLUDE_PALETTE);
                        break;
                    case CHUNK_SLICE:
                        include = include && (p->filter & INCLUDE_SLICE);
                        break;
                    case CHUNK_TAGS:
                        include = include && (p->filter & INCLUDE_TAGS);
                        break;
                    case CHUNK_USER_DATA:
                        include = include && (p->filter & INCLUDE_USER_DATA);
                        break;
                    case CHUNK_TILESET:
                        include = include && (p->filter & INCLUDE_TILESET);
                        break;
                    default:
                        // Unknown chunk
                        include = false;
                        break;
                }
                if(!include && !(p->file_header->color_depth == 8 && header->chunk_type == CHUNK_PALETTE))
                {
                    //printf("-> SKIPPED %s (%X)\n", ase_struct_type_name(header->chunk_type), header->chunk_type);
                    ase_skip_chunk(p); 
                }
                if(p->filter & INCLUDE_CHUNK_HEADER)
                    return ASE_CHUNK_HEADER;
                else return ase_parse_next(p);
            }
            else if(p->current_chunk_type == CHUNK_LAYER)
            {
                int size = sizeof(ase_layer_chunk_t);
                ase_layer_chunk_t* header = (ase_layer_chunk_t*)p->cur;
                size += header->layer_name_length;
                if(header->type == 2) size += 4;
                if(p->file_header->flag_layers_have_uuid == 1) size += 16;
                
                advance(p, size, ASE_LAYER);
                return ASE_LAYER;
            }
            else if(p->current_chunk_type == CHUNK_PALETTE && p->nesting_depth == 1)
            {
                ase_palette_t* header = (ase_palette_t*)advance(p, sizeof(ase_palette_t), ASE_PALETTE);
                p->current_color_palette_index = header->first_index;
                p->cur_chunk_remaining_entries = header->last_index - header->first_index + 1;
                p->color_palette_size = header->new_palette_size;
                p->nesting_depth = 2;
                if(p->filter & INCLUDE_PALETTE) return ASE_PALETTE;
                else return ase_parse_next(p);
            }
            else if(p->current_chunk_type == CHUNK_PALETTE && p->nesting_depth == 2)
            {
                int size = sizeof(ase_palette_entry_t) - 2;
                ase_palette_entry_t* entry = (ase_palette_entry_t*)p->cur;
                p->color_palette[p->current_color_palette_index++] = entry->rgba;
                p->cur_chunk_remaining_entries--;
                if(entry->flag_has_name)
                    size += 2 + entry->color_name_length;
                advance(p, size, ASE_PALETTE_ENTRY);
                if(p->filter & INCLUDE_PALETTE) return ASE_PALETTE_ENTRY;
                else return ase_parse_next(p);
            }
            else if(p->current_chunk_type == CHUNK_EXTERNAL_FILES && p->nesting_depth == 1)
            {
                ase_external_files_t* header = (ase_external_files_t*)advance(p, sizeof(ase_external_files_t), ASE_EXTERNAL_FILES);
                p->cur_chunk_remaining_entries = header->num_entries;
                p->nesting_depth = 2;
                return ASE_EXTERNAL_FILES;
            }
            else if(p->current_chunk_type == CHUNK_EXTERNAL_FILES && p->nesting_depth == 2)
            {
                int size = sizeof(ase_external_file_entry_t);
                ase_external_file_entry_t* entry = (ase_external_file_entry_t*)p->cur;
                p->cur_chunk_remaining_entries--;
                size += entry->filename_or_id_length;
                advance(p, size, ASE_EXTERNAL_FILE_ENTRY);
                return ASE_EXTERNAL_FILE_ENTRY;
            }
            else if(p->current_chunk_type == CHUNK_SLICE && p->nesting_depth == 1)
            {
                int size = sizeof(ase_slice_t);
                ase_slice_t* slice = (ase_slice_t*)p->cur;
                p->current_slice = slice;
                p->cur_chunk_remaining_entries = slice->num_keys;
                p->nesting_depth = 2;
                size += slice->name_length;
                advance(p, size, ASE_SLICE);
                return ASE_SLICE;
            }
            else if(p->current_chunk_type == CHUNK_SLICE && p->nesting_depth == 2)
            {
                p->cur_chunk_remaining_entries--;
                int size = sizeof(ase_slice_key_t) - 24;
                if(p->current_slice->flag_has_9patch)
                    size += 16;
                if(p->current_slice->flag_has_pivot)
                    size += 8;
                advance(p, size, ASE_SLICE_KEY);
                return ASE_SLICE_KEY;
            }
            else if(p->current_chunk_type == CHUNK_TAGS && p->nesting_depth == 1)
            {
                int size = sizeof(ase_tags_t);
                ase_tags_t* tags = (ase_tags_t*)p->cur;
                p->cur_chunk_remaining_entries = tags->num_tags;
                p->nesting_depth = 2;
                advance(p, size, ASE_TAGS);
                return ASE_TAGS;
            }
            else if(p->current_chunk_type == CHUNK_TAGS && p->nesting_depth == 2)
            {
                p->cur_chunk_remaining_entries--;
                int size = sizeof(ase_tag_t);
                ase_tag_t* tag = (ase_tag_t*)p->cur;
                size += tag->name_length;
                advance(p, size, ASE_TAG);
                return ASE_TAG;
            }
            else
            {
                advance(p, p->next_chunk_start - p->cur, p->current_chunk_type);
                return p->current_chunk_type;
            }
        }
        
    }
}