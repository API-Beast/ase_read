ase_read is a tiny reader for Aseprite files. It is quite fast, on a desktop PC small animations can be loaded in <1ms, large ones in <10ms.

It consists of a low level streaming parser as well as easy to use high level API. The high level API includes a software renderer for composing frames, supporting all 18 blend modes which Aseprite uses.

It uses sinfl for decompression, but you can also use libdeflate for decompression by defining ASE_USE_LIBDEFLATE. libdeflate is approximately 30% faster, but is also a quite large library, while sinfl is a single header. You can also use miniz or other zlib compatible implementations, but those are approximately 300% slower.

# High Level API
```c
struct AseFile* ase_read_file(const char* path);
void            ase_free_file(struct AseFile* file);
// For creating texture atlases for GPU rendering.
bool ase_trim_pixels(struct AseFile* file, int pixel_data_index);
bool ase_copy_pixels(struct AseFile* file, int pixel_data_index, uint8_t* dst, int pitch, int offset_x, int offset_y);
// For compositing on the CPU.
void ase_draw_frame(struct AseFile* file, int frame, uint8_t* dst, int pitch, int offset_x, int offset_y);
void ase_draw_cel(struct AseFile* file, int frame, int layer, uint8_t* dst, int pitch, int offset_x, int offset_y);
```

# Examples

Read file and create texture atlas for GPU rendering.
```c
struct AseFile* file = ase_read_file("sprite.ase");
for(int i = 0; i < file->num_pixel_data_blocks; i++)
{
    ase_trim_pixels(file, i); // (optional) Trim transparent areas to not waste space.
    struct AsePixelData* cel_data = &file->pixel_data[i];
    int dst_x, dst_y;
    int idx = your_find_space_in_texture_atlas_function(cel_data->trimmed_w, cel_data->trimmed_h, &dst_x, &dst_y);
    ase_copy_pixels(file, i, your_texture_atlas_data, your_texture_atlas_width * 4, dst_x, dst_y);
    your_store_texture_atlas_subrect_data_function(idx, cel_data->trimmed_x, cel_data->trimmed_y, cel_data->trimmed_w, cel_data->trimmed_h);
}
```

Read file and create spritesheet grid where each cel is one frame.
```c
struct AseFile* file = ase_read_file("sprite.ase");
int texture_width = file->num_frames * file->width;
int texture_pitch = texture_width * 4;
uint8_t* texture_data = malloc(file->height * texture_pitch);
for(int frame = 0; frame < file->num_frames; frame++)
    ase_draw_frame(file, frame, texture_data, texture_pitch, file->width * frame, 0);
```

All the meta data and user data is stored plainly in struct AseFile. See the struct definitions in the header file for accessing it. To extract slices use the ase_draw_frame_partial function. 

# Limitations

- High Level API:
 * Tilemaps are not supported by the high level API. Cels containing tilemaps are simply treated as if they were empty.
 * Composite group layers are not supported. This is an experimental feature you have to explicitly enable in Aseprite to use.
 * Slice animations are not supported. This was a experimental feature that was removed in newer versions of Aseprite.
 * External files and color profiles are not supported.
- Low Level API:
 * Depreceated block types are not supported, this means that the Aseprite files need to have been created with Asprite version 1.2 or newer. Though in practice many older files will work just fine.
 * Generally, property maps are tricky to parse and the low level API doesn't have anything to make it easier. Property maps are used by Lua extensions to store data in a asperite file.

Merge requests implementing any of these are welcome.