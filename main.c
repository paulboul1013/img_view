#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <zlib.h>

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600
#define ZOOM_STEP 1.1
#define MIN_ZOOM  0.1
#define MAX_ZOOM  10.0

typedef enum {
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_PPM_P6,
    IMAGE_FORMAT_PNG
} ImageFormat;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t compression_method;
    uint8_t filter_method;
    uint8_t interlace_method;
} PNGHeader;

typedef struct {
    unsigned char *data;
    size_t size;
} ByteBuffer;

static double get_base_scale(int image_width, int image_height)
{
    double scale_x = (double)WINDOW_WIDTH / (double)image_width;
    double scale_y = (double)WINDOW_HEIGHT / (double)image_height;
    return scale_x < scale_y ? scale_x : scale_y;
}

static void clamp_image_position(
    double *image_x,
    double *image_y,
    int image_width,
    int image_height,
    double base_scale,
    double zoom)
{
    double scaled_width = image_width * base_scale * zoom;
    double scaled_height = image_height * base_scale * zoom;

    if (scaled_width <= WINDOW_WIDTH) {
        *image_x = (WINDOW_WIDTH - scaled_width) / 2.0;
    } else {
        double min_x = WINDOW_WIDTH - scaled_width;
        if (*image_x > 0.0) *image_x = 0.0;
        if (*image_x < min_x) *image_x = min_x;
    }

    if (scaled_height <= WINDOW_HEIGHT) {
        *image_y = (WINDOW_HEIGHT - scaled_height) / 2.0;
    } else {
        double min_y = WINDOW_HEIGHT - scaled_height;
        if (*image_y > 0.0) *image_y = 0.0;
        if (*image_y < min_y) *image_y = min_y;
    }
}

static void render_image(
    SDL_Window *window,
    SDL_Surface *window_surface,
    SDL_Surface *image_surface,
    int image_width,
    int image_height,
    double zoom,
    double image_x,
    double image_y)
{
    double base_scale = get_base_scale(image_width, image_height);
    double final_scale = base_scale * zoom;

    int scaled_width = (int)(image_width * final_scale);
    int scaled_height = (int)(image_height * final_scale);

    SDL_Rect dst_rect;
    dst_rect.x = (int)image_x;
    dst_rect.y = (int)image_y;
    dst_rect.w = scaled_width;
    dst_rect.h = scaled_height;

    Uint32 background = SDL_MapRGB(window_surface->format, 0, 0, 0);
    SDL_FillRect(window_surface, NULL, background);

    if (SDL_BlitScaled(image_surface, NULL, window_surface, &dst_rect) != 0) {
        fprintf(stderr, "SDL_BlitScaled: %s\n", SDL_GetError());
    }

    SDL_UpdateWindowSurface(window);
}

static SDL_Surface *rotate_surface_90_clockwise(SDL_Surface *src)
{
    int src_width = src->w;
    int src_height = src->h;

    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(
        0,
        src_height,
        src_width,
        32,
        src->format->format
    );

    if (!dst) {
        return NULL;
    }

    if (SDL_MUSTLOCK(src) && SDL_LockSurface(src) != 0) {
        SDL_FreeSurface(dst);
        return NULL;
    }

    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) != 0) {
        if (SDL_MUSTLOCK(src)) SDL_UnlockSurface(src);
        SDL_FreeSurface(dst);
        return NULL;
    }

    for (int y = 0; y < src_height; ++y) {
        Uint32 *src_row = (Uint32 *)((Uint8 *)src->pixels + y * src->pitch);
        for (int x = 0; x < src_width; ++x) {
            int dst_x = src_height - 1 - y;
            int dst_y = x;
            Uint32 *dst_row = (Uint32 *)((Uint8 *)dst->pixels + dst_y * dst->pitch);
            dst_row[dst_x] = src_row[x];
        }
    }

    if (SDL_MUSTLOCK(dst)) SDL_UnlockSurface(dst);
    if (SDL_MUSTLOCK(src)) SDL_UnlockSurface(src);

    return dst;
}

static int detect_image_format(FILE *input, ImageFormat *format)
{
    static const unsigned char png_signature[8] = {
        0x89, 0x50, 0x4E, 0x47,
        0x0D, 0x0A, 0x1A, 0x0A
    };

    int b0 = fgetc(input);
    int b1 = fgetc(input);

    if (b0 == EOF || b1 == EOF) {
        return 0;
    }

    if (b0 == 'P' && b1 == '6') {
        *format = IMAGE_FORMAT_PPM_P6;
        return 1;
    }

    if ((unsigned char)b0 == 0x89 && (unsigned char)b1 == 0x50) {
        unsigned char signature[8];
        signature[0] = (unsigned char)b0;
        signature[1] = (unsigned char)b1;

        if (fread(signature + 2, 1, 6, input) != 6) {
            return 0;
        }

        if (memcmp(signature, png_signature, sizeof(png_signature)) == 0) {
            *format = IMAGE_FORMAT_PNG;
            return 1;
        }
    }

    *format = IMAGE_FORMAT_UNKNOWN;
    return 1;
}

static int read_u32_be(FILE *input, uint32_t *value)
{
    unsigned char b[4];
    if (fread(b, 1, 4, input) != 4) {
        return 0;
    }

    *value =
        ((uint32_t)b[0] << 24) |
        ((uint32_t)b[1] << 16) |
        ((uint32_t)b[2] << 8)  |
        ((uint32_t)b[3]);

    return 1;
}

static uint32_t u32_be_from_bytes(const unsigned char *b)
{
    return
        ((uint32_t)b[0] << 24) |
        ((uint32_t)b[1] << 16) |
        ((uint32_t)b[2] << 8)  |
        ((uint32_t)b[3]);
}

static int skip_stream_bytes(FILE *input, uint32_t count)
{
    unsigned char buffer[4096];

    while (count > 0) {
        size_t amount = count < sizeof(buffer) ? (size_t)count : sizeof(buffer);
        if (fread(buffer, 1, amount, input) != amount) {
            return 0;
        }
        count -= (uint32_t)amount;
    }

    return 1;
}

static int append_bytes(ByteBuffer *buffer, const unsigned char *data, size_t size)
{
    if (size == 0) {
        return 1;
    }

    if (size > SIZE_MAX - buffer->size) {
        return 0;
    }

    size_t new_size = buffer->size + size;
    unsigned char *new_data = realloc(buffer->data, new_size);
    if (!new_data) {
        return 0;
    }

    memcpy(new_data + buffer->size, data, size);
    buffer->data = new_data;
    buffer->size = new_size;
    return 1;
}

static const char *png_color_type_name(uint8_t color_type)
{
    switch (color_type) {
        case 0: return "Grayscale";
        case 2: return "Truecolor RGB";
        case 3: return "Indexed-color / Palette";
        case 4: return "Grayscale + Alpha";
        case 6: return "Truecolor RGBA";
        default: return "Unknown";
    }
}

static int parse_png_ihdr(const unsigned char *data, uint32_t length, PNGHeader *header)
{
    if (length != 13) {
        fprintf(stderr, "PNG: IHDR length must be 13, got %u\n", (unsigned)length);
        return 0;
    }

    header->width = u32_be_from_bytes(data + 0);
    header->height = u32_be_from_bytes(data + 4);
    header->bit_depth = data[8];
    header->color_type = data[9];
    header->compression_method = data[10];
    header->filter_method = data[11];
    header->interlace_method = data[12];

    if (header->width == 0 || header->height == 0) {
        fprintf(stderr, "PNG: width and height must be non-zero\n");
        return 0;
    }

    return 1;
}

static void print_png_ihdr(const PNGHeader *header)
{
    const char *interlace_desc =
        header->interlace_method == 0 ? "none" :
        header->interlace_method == 1 ? "Adam7" :
        "unknown";

    printf("\nIHDR metadata\n");
    printf("--------------------------------\n");
    printf("Width       : %u\n", (unsigned)header->width);
    printf("Height      : %u\n", (unsigned)header->height);
    printf("Bit depth   : %u\n", (unsigned)header->bit_depth);
    printf("Color type  : %u (%s)\n",
           (unsigned)header->color_type,
           png_color_type_name(header->color_type));
    printf("Compression : %u\n", (unsigned)header->compression_method);
    printf("Filter      : %u\n", (unsigned)header->filter_method);
    printf("Interlace   : %u (%s)\n",
           (unsigned)header->interlace_method,
           interlace_desc);
    printf("--------------------------------\n\n");
}

static int validate_stage5_supported_png(const PNGHeader *header)
{
    if (header->bit_depth != 8) {
        fprintf(stderr, "Stage 5 only supports 8-bit PNG for now\n");
        return 0;
    }

    if (header->color_type != 2 && header->color_type != 6) {
        fprintf(stderr, "Stage 5 only supports color type 2 (RGB) or 6 (RGBA)\n");
        return 0;
    }

    if (header->compression_method != 0) {
        fprintf(stderr, "Unsupported PNG compression method: %u\n",
                (unsigned)header->compression_method);
        return 0;
    }

    if (header->filter_method != 0) {
        fprintf(stderr, "Unsupported PNG filter method: %u\n",
                (unsigned)header->filter_method);
        return 0;
    }

    if (header->interlace_method != 0) {
        fprintf(stderr, "Stage 5 does not support interlaced PNG yet\n");
        return 0;
    }

    return 1;
}

static int inflate_idat_stream(
    const PNGHeader *header,
    const ByteBuffer *idat,
    ByteBuffer *filtered_out,
    int *channels_out)
{
    int channels;
    size_t row_bytes;
    size_t filtered_size;
    unsigned char *filtered = NULL;
    uLongf output_size;
    int zret;

    if (!validate_stage5_supported_png(header)) {
        return 0;
    }

    channels = (header->color_type == 2) ? 3 : 4;
    row_bytes = (size_t)header->width * (size_t)channels;

    if (row_bytes > SIZE_MAX - 1) {
        fprintf(stderr, "PNG row size overflow\n");
        return 0;
    }

    if ((row_bytes + 1) > SIZE_MAX / (size_t)header->height) {
        fprintf(stderr, "PNG filtered size overflow\n");
        return 0;
    }

    filtered_size = (row_bytes + 1) * (size_t)header->height;

    filtered = (unsigned char *)malloc(filtered_size);
    if (!filtered) {
        fprintf(stderr, "Failed to allocate filtered output buffer\n");
        return 0;
    }

    output_size = (uLongf)filtered_size;
    zret = uncompress(filtered,
                      &output_size,
                      idat->data,
                      (uLong)idat->size);

    if (zret != Z_OK) {
        fprintf(stderr, "zlib uncompress failed: %d\n", zret);
        free(filtered);
        return 0;
    }

    if ((size_t)output_size != filtered_size) {
        fprintf(stderr,
                "Unexpected inflated size: expected %zu, got %lu\n",
                filtered_size,
                (unsigned long)output_size);
        free(filtered);
        return 0;
    }

    filtered_out->data = filtered;
    filtered_out->size = filtered_size;
    *channels_out = channels;
    return 1;
}


/*
 * Debug helper: 將 Stage 5 inflate 後的 scanline 原始資料
 * 輸出成十六進位文字檔。
 *
 * 每一列在 filtered buffer 中的格式：
 *
 *     [1-byte filter type][row_bytes filtered data]
 *
 * 注意：這裡輸出的 row data 還沒有做 PNG unfilter，
 * 因此 filter type 1~4 時，這些 bytes 不是最終 RGB/RGBA pixel 值。
 */
static const char *png_filter_type_name(uint8_t filter_type)
{
    switch (filter_type) {
        case 0: return "None";
        case 1: return "Sub";
        case 2: return "Up";
        case 3: return "Average";
        case 4: return "Paeth";
        default: return "Invalid/Unknown";
    }
}

static int dump_filtered_scanlines_hex(
    const PNGHeader *header,
    const ByteBuffer *filtered,
    int channels,
    const char *output_path)
{
    const size_t bytes_per_hex_line = 32;
    size_t row_bytes;
    size_t scanline_stride;
    size_t expected_size;

    if (!header || !filtered || !filtered->data || !output_path) {
        return 0;
    }

    row_bytes = (size_t)header->width * (size_t)channels;
    scanline_stride = row_bytes + 1;
    expected_size = scanline_stride * (size_t)header->height;

    if (filtered->size != expected_size) {
        fprintf(stderr,
                "Cannot dump scanlines: expected %zu bytes, got %zu\n",
                expected_size,
                filtered->size);
        return 0;
    }

    FILE *out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr,
                "Failed to create scanline dump: %s\n",
                output_path);
        return 0;
    }

    fprintf(out, "PNG Stage 5 filtered scanline dump\n");
    fprintf(out, "========================================\n");
    fprintf(out, "Width             : %u\n", (unsigned)header->width);
    fprintf(out, "Height            : %u\n", (unsigned)header->height);
    fprintf(out, "Channels          : %d\n", channels);
    fprintf(out, "Bytes per pixel   : %d\n", channels);
    fprintf(out, "Row data bytes    : %zu\n", row_bytes);
    fprintf(out, "Scanline stride   : %zu (1 filter byte + %zu data bytes)\n",
            scanline_stride,
            row_bytes);
    fprintf(out, "Total bytes       : %zu\n", filtered->size);
    fprintf(out, "========================================\n\n");

    for (uint32_t y = 0; y < header->height; ++y) {
        const unsigned char *scanline =
            filtered->data + (size_t)y * scanline_stride;

        uint8_t filter_type = scanline[0];
        const unsigned char *row_data = scanline + 1;

        fprintf(out, "Scanline %u\n", (unsigned)y);
        fprintf(out, "Filter type : %u (%s)\n",
                (unsigned)filter_type,
                png_filter_type_name(filter_type));
        fprintf(out, "Filter byte : %02X\n", (unsigned)filter_type);
        fprintf(out, "Data bytes  : %zu\n", row_bytes);
        fprintf(out, "Hex data:\n");

        for (size_t i = 0; i < row_bytes; ++i) {
            if (i % bytes_per_hex_line == 0) {
                fprintf(out, "%06zX : ", i);
            }

            fprintf(out, "%02X", (unsigned)row_data[i]);

            if ((i + 1) % bytes_per_hex_line == 0 || i + 1 == row_bytes) {
                fputc('\n', out);
            } else {
                fputc(' ', out);
            }
        }

        fputc('\n', out);
    }

    if (fclose(out) != 0) {
        fprintf(stderr,
                "Failed to finish writing scanline dump: %s\n",
                output_path);
        return 0;
    }

    return 1;
}



/*
 * Stage 6: 統計每個 scanline 使用的 PNG filter type。
 *
 * filtered buffer 每一列：
 *     [filter byte][row_bytes filtered data]
 */
typedef struct {
    size_t count[5];
    size_t invalid_count;
} PNGFilterStats;

static int analyze_png_filter_types(
    const PNGHeader *header,
    const ByteBuffer *filtered,
    int channels,
    PNGFilterStats *stats)
{
    size_t row_bytes;
    size_t stride;
    size_t expected_size;

    if (!header || !filtered || !filtered->data || !stats) {
        return 0;
    }

    memset(stats, 0, sizeof(*stats));

    row_bytes = (size_t)header->width * (size_t)channels;
    stride = row_bytes + 1;
    expected_size = stride * (size_t)header->height;

    if (filtered->size != expected_size) {
        fprintf(stderr,
                "Stage 6: filtered buffer size mismatch: expected %zu, got %zu\n",
                expected_size,
                filtered->size);
        return 0;
    }

    for (uint32_t y = 0; y < header->height; ++y) {
        const unsigned char *scanline =
            filtered->data + (size_t)y * stride;

        uint8_t filter_type = scanline[0];

        if (filter_type <= 4) {
            ++stats->count[filter_type];
        } else {
            ++stats->invalid_count;
        }
    }

    return 1;
}

static void print_png_filter_stats(
    const PNGFilterStats *stats)
{
    printf("\nStage 6 filter summary\n");
    printf("--------------------------------\n");
    printf("Filter 0 None    : %zu rows\n", stats->count[0]);
    printf("Filter 1 Sub     : %zu rows\n", stats->count[1]);
    printf("Filter 2 Up      : %zu rows\n", stats->count[2]);
    printf("Filter 3 Average : %zu rows\n", stats->count[3]);
    printf("Filter 4 Paeth   : %zu rows\n", stats->count[4]);
    printf("Invalid filter   : %zu rows\n", stats->invalid_count);
    printf("--------------------------------\n");
}

/*
 * Stage 6: 只實作 Filter Type 0 (None)。
 *
 * Filter 0 的定義：filtered byte 就等於原始 byte，
 * 因此每列只需要跳過最前面的 filter byte，接著 memcpy。
 *
 *     filtered:
 *       [00][R G B A R G B A ...]
 *             |__________________|
 *                       |
 *                    memcpy
 *                       v
 *     raw pixels:
 *           [R G B A R G B A ...]
 *
 * 回傳值：
 *     1  = 全部 scanline 都是 Filter 0，完整還原成功
 *     2  = 遇到合法但 Stage 6 尚未支援的 Filter 1~4
 *     0  = 資料格式或記憶體錯誤
 */
static int unfilter_none_only(
    const PNGHeader *header,
    const ByteBuffer *filtered,
    int channels,
    ByteBuffer *raw_pixels,
    uint32_t *unsupported_row,
    uint8_t *unsupported_filter)
{
    size_t row_bytes;
    size_t stride;
    size_t raw_size;
    unsigned char *raw;

    if (!header || !filtered || !filtered->data || !raw_pixels) {
        return 0;
    }

    row_bytes = (size_t)header->width * (size_t)channels;
    stride = row_bytes + 1;

    if (row_bytes != 0 &&
        (size_t)header->height > SIZE_MAX / row_bytes) {
        fprintf(stderr, "Stage 6: raw pixel size overflow\n");
        return 0;
    }

    raw_size = row_bytes * (size_t)header->height;

    if (filtered->size != stride * (size_t)header->height) {
        fprintf(stderr, "Stage 6: invalid filtered buffer size\n");
        return 0;
    }

    raw = (unsigned char *)malloc(raw_size);
    if (!raw) {
        fprintf(stderr, "Stage 6: failed to allocate raw pixel buffer\n");
        return 0;
    }

    for (uint32_t y = 0; y < header->height; ++y) {
        const unsigned char *scanline =
            filtered->data + (size_t)y * stride;

        uint8_t filter_type = scanline[0];
        const unsigned char *filtered_row = scanline + 1;
        unsigned char *raw_row = raw + (size_t)y * row_bytes;

        if (filter_type != 0) {
            if (filter_type > 4) {
                fprintf(stderr,
                        "Stage 6: invalid filter type %u at scanline %u\n",
                        (unsigned)filter_type,
                        (unsigned)y);
                free(raw);
                return 0;
            }

            if (unsupported_row) {
                *unsupported_row = y;
            }

            if (unsupported_filter) {
                *unsupported_filter = filter_type;
            }

            free(raw);
            return 2;
        }

        /*
         * Filter 0 = None：沒有 predictor，也沒有加減運算。
         * 解壓後的 row bytes 本身就是最終 raw pixel bytes。
         */
        memcpy(raw_row, filtered_row, row_bytes);
    }

    raw_pixels->data = raw;
    raw_pixels->size = raw_size;
    return 1;
}

static int inspect_png_and_inflate(
    FILE *input,
    PNGHeader *header,
    ByteBuffer *idat,
    unsigned int *idat_chunk_count,
    ByteBuffer *filtered,
    int *channels_out)
{
    unsigned int chunk_index = 0;
    int got_ihdr = 0;

    while (1) {
        uint32_t length;
        uint32_t crc;
        char type[5];

        if (!read_u32_be(input, &length)) {
            return 0;
        }

        if (fread(type, 1, 4, input) != 4) {
            return 0;
        }
        type[4] = '\0';

        printf("Chunk %u: type=%.4s, length=%u",
               chunk_index,
               type,
               (unsigned)length);

        if (memcmp(type, "IHDR", 4) == 0) {
            unsigned char ihdr_data[13];

            if (chunk_index != 0) {
                fprintf(stderr, "\nPNG: IHDR must be the first chunk\n");
                return 0;
            }

            if (got_ihdr) {
                fprintf(stderr, "\nPNG: duplicate IHDR chunk\n");
                return 0;
            }

            if (length != sizeof(ihdr_data)) {
                fprintf(stderr, "\nPNG: IHDR length must be 13\n");
                return 0;
            }

            if (fread(ihdr_data, 1, sizeof(ihdr_data), input) != sizeof(ihdr_data)) {
                fprintf(stderr, "\nPNG: truncated IHDR data\n");
                return 0;
            }

            if (!parse_png_ihdr(ihdr_data, length, header)) {
                return 0;
            }

            got_ihdr = 1;
        }
        else if (memcmp(type, "IDAT", 4) == 0) {
            unsigned char *chunk_data = NULL;

            if (!got_ihdr) {
                fprintf(stderr, "\nPNG: encountered IDAT before IHDR\n");
                return 0;
            }

            if (length > 0) {
                chunk_data = (unsigned char *)malloc((size_t)length);
                if (!chunk_data) {
                    fprintf(stderr, "\nFailed to allocate IDAT chunk\n");
                    return 0;
                }

                if (fread(chunk_data, 1, (size_t)length, input) != (size_t)length) {
                    fprintf(stderr, "\nPNG: truncated IDAT data\n");
                    free(chunk_data);
                    return 0;
                }
            }

            if (!append_bytes(idat, chunk_data, (size_t)length)) {
                fprintf(stderr, "\nFailed to append IDAT data\n");
                free(chunk_data);
                return 0;
            }

            free(chunk_data);
            ++(*idat_chunk_count);
        }
        else {
            if (!skip_stream_bytes(input, length)) {
                fprintf(stderr, "\nFailed while skipping chunk data\n");
                return 0;
            }
        }

        if (!read_u32_be(input, &crc)) {
            return 0;
        }

        printf(", crc=0x%08X\n", (unsigned)crc);

        if (memcmp(type, "IHDR", 4) == 0) {
            print_png_ihdr(header);
        }

        ++chunk_index;

        if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    if (!got_ihdr) {
        fprintf(stderr, "PNG: missing IHDR chunk\n");
        return 0;
    }

    if (*idat_chunk_count == 0) {
        fprintf(stderr, "PNG: missing IDAT chunk(s)\n");
        return 0;
    }

    return inflate_idat_stream(header, idat, filtered, channels_out);
}

int main(int argc, char *argv[])
{
    FILE *input = stdin;
    int should_close_input = 0;
    ImageFormat format = IMAGE_FORMAT_UNKNOWN;

    if (argc == 2) {
        input = fopen(argv[1], "rb");
        if (!input) {
            fprintf(stderr, "Failed to open file: %s\n", argv[1]);
            return 1;
        }
        should_close_input = 1;
    } else if (argc > 2) {
        fprintf(stderr,
                "Usage:\n"
                "  %s < image.ppm\n"
                "  %s image.ppm\n"
                "  %s < image.png\n"
                "  %s image.png\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (!detect_image_format(input, &format)) {
        fprintf(stderr, "Failed to read image signature\n");
        goto fail_input;
    }

    if (format == IMAGE_FORMAT_PNG) {
        PNGHeader header = {0};
        ByteBuffer idat = {0};
        ByteBuffer filtered = {0};
        unsigned int idat_chunk_count = 0;
        int channels = 0;
        size_t row_bytes;
        size_t expected_filtered_size;

        printf("PNG detected\n");
        printf("Stage 6: Stage 5 inflate + Filter 0 (None) reconstruction...\n\n");

        if (!inspect_png_and_inflate(
                input,
                &header,
                &idat,
                &idat_chunk_count,
                &filtered,
                &channels)) {
            free(idat.data);
            free(filtered.data);
            goto fail_input;
        }

        row_bytes = (size_t)header.width * (size_t)channels;
        expected_filtered_size = ((size_t)row_bytes + 1) * (size_t)header.height;

        printf("\nStage 4 IDAT summary\n");
        printf("--------------------------------\n");
        printf("IDAT chunks collected : %u\n", idat_chunk_count);
        printf("Compressed size       : %zu bytes\n", idat.size);
        printf("--------------------------------\n");

        printf("\nStage 5 inflate summary\n");
        printf("--------------------------------\n");
        printf("Channels             : %d\n", channels);
        printf("Bytes per pixel      : %d\n", channels);
        printf("Bytes per row        : %zu\n", row_bytes);
        printf("Filter byte per row  : 1\n");
        printf("Rows                 : %u\n", (unsigned)header.height);
        printf("Expected output size : %zu bytes\n", expected_filtered_size);
        printf("Inflated output size : %zu bytes\n", filtered.size);
        printf("--------------------------------\n\n");

        printf("Stage 5 complete: the zlib datastream was inflated successfully.\n");
        printf("The output is still filtered scanlines, not final pixels yet.\n");

        /*
         * Debug dump:
         * 把每一個 scanline 的 filter type + 全部 filtered bytes
         * 輸出成真正的 hexadecimal text data。
         */
        if (!dump_filtered_scanlines_hex(
                &header,
                &filtered,
                channels,
                "scanlines_hex.txt")) {

            fprintf(stderr,
                    "Failed to write scanlines_hex.txt\n");

            free(idat.data);
            free(filtered.data);
            goto fail_input;
        }

        printf("Scanline hex dump written to: scanlines_hex.txt\n");

        /*
         * Stage 6.1: 先統計整張 PNG 實際使用哪些 filter。
         */
        PNGFilterStats filter_stats;

        if (!analyze_png_filter_types(
                &header,
                &filtered,
                channels,
                &filter_stats)) {

            free(idat.data);
            free(filtered.data);
            goto fail_input;
        }

        print_png_filter_stats(&filter_stats);

        /*
         * Stage 6.2: 只實作 Filter 0 (None)。
         */
        ByteBuffer raw_pixels = {0};
        uint32_t unsupported_row = 0;
        uint8_t unsupported_filter = 0;

        int stage6_result =
            unfilter_none_only(
                &header,
                &filtered,
                channels,
                &raw_pixels,
                &unsupported_row,
                &unsupported_filter);

        if (stage6_result == 1) {
            size_t expected_raw_size =
                (size_t)header.width *
                (size_t)header.height *
                (size_t)channels;

            printf("\nStage 6 reconstruction summary\n");
            printf("--------------------------------\n");
            printf("Supported filter       : 0 (None)\n");
            printf("Expected raw size      : %zu bytes\n", expected_raw_size);
            printf("Reconstructed raw size : %zu bytes\n", raw_pixels.size);
            printf("--------------------------------\n\n");

            printf("Stage 6 complete: every scanline used Filter 0.\n");
            printf("Raw RGB/RGBA pixel bytes are now reconstructed.\n");
            printf("Next Stage 7 will add Filter 1 (Sub).\n");
        }
        else if (stage6_result == 2) {
            printf("\nStage 6 stopped at scanline %u.\n",
                   (unsigned)unsupported_row);
            printf("That scanline uses Filter %u (%s).\n",
                   (unsigned)unsupported_filter,
                   png_filter_type_name(unsupported_filter));
            printf("Stage 6 currently implements only Filter 0 (None).\n");
            printf("This is expected for normal PNG files that mix filter types.\n");
            printf("Next Stage 7 will implement Filter 1 (Sub).\n");
        }
        else {
            fprintf(stderr, "Stage 6 reconstruction failed\n");
            free(idat.data);
            free(filtered.data);
            free(raw_pixels.data);
            goto fail_input;
        }

        free(raw_pixels.data);
        free(idat.data);
        free(filtered.data);

        if (should_close_input && input) {
            fclose(input);
        }
        return 0;
    }

    if (format != IMAGE_FORMAT_PPM_P6) {
        fprintf(stderr, "Unsupported image format\n");
        goto fail_input;
    }

    /* Original PPM viewer path remains available. */
    printf("PPM P6 detected\n");

    int width;
    int height;
    int maxval;

    if (fscanf(input, "%d %d", &width, &height) != 2) {
        fprintf(stderr, "Failed to read image size\n");
        goto fail_input;
    }

    if (fscanf(input, "%d", &maxval) != 1 || maxval != 255) {
        fprintf(stderr, "Only maxval=255 is supported\n");
        goto fail_input;
    }

    fgetc(input);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        goto fail_input;
    }

    SDL_Window *window = SDL_CreateWindow(
        "PPM Viewer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        0
    );

    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        goto fail_input;
    }

    SDL_Surface *image_surface = SDL_CreateRGBSurfaceWithFormat(
        0,
        width,
        height,
        32,
        SDL_PIXELFORMAT_ARGB8888
    );

    if (!image_surface) {
        fprintf(stderr, "Create image surface: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        goto fail_input;
    }

    if (SDL_MUSTLOCK(image_surface)) {
        if (SDL_LockSurface(image_surface) != 0) {
            fprintf(stderr, "SDL_LockSurface: %s\n", SDL_GetError());
            SDL_FreeSurface(image_surface);
            SDL_DestroyWindow(window);
            SDL_Quit();
            goto fail_input;
        }
    }

    for (int y = 0; y < height; ++y) {
        Uint8 *row = (Uint8 *)image_surface->pixels + y * image_surface->pitch;
        for (int x = 0; x < width; ++x) {
            int r = fgetc(input);
            int g = fgetc(input);
            int b = fgetc(input);

            if (r == EOF || g == EOF || b == EOF) {
                fprintf(stderr, "Unexpected end of image\n");
                if (SDL_MUSTLOCK(image_surface)) SDL_UnlockSurface(image_surface);
                SDL_FreeSurface(image_surface);
                SDL_DestroyWindow(window);
                SDL_Quit();
                goto fail_input;
            }

            Uint32 color = SDL_MapRGB(image_surface->format, (Uint8)r, (Uint8)g, (Uint8)b);
            ((Uint32 *)row)[x] = color;
        }
    }

    if (SDL_MUSTLOCK(image_surface)) {
        SDL_UnlockSurface(image_surface);
    }

    if (should_close_input) {
        fclose(input);
        input = NULL;
        should_close_input = 0;
    }

    SDL_Surface *window_surface = SDL_GetWindowSurface(window);
    if (!window_surface) {
        fprintf(stderr, "SDL_GetWindowSurface: %s\n", SDL_GetError());
        SDL_FreeSurface(image_surface);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    double zoom = 1.0;
    double base_scale = get_base_scale(width, height);
    double display_width = width * base_scale;
    double display_height = height * base_scale;
    double image_x = (WINDOW_WIDTH - display_width) / 2.0;
    double image_y = (WINDOW_HEIGHT - display_height) / 2.0;
    int dragging = 0;

    render_image(window, window_surface, image_surface, width, height, zoom, image_x, image_y);

    int running = 1;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }

            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = 0;
            }

            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r && event.key.repeat == 0) {
                SDL_Surface *rotated = rotate_surface_90_clockwise(image_surface);
                if (!rotated) {
                    fprintf(stderr, "Rotation failed\n");
                } else {
                    SDL_FreeSurface(image_surface);
                    image_surface = rotated;
                    int old_width = width;
                    width = height;
                    height = old_width;
                    zoom = 1.0;
                    dragging = 0;
                    base_scale = get_base_scale(width, height);
                    display_width = width * base_scale;
                    display_height = height * base_scale;
                    image_x = (WINDOW_WIDTH - display_width) / 2.0;
                    image_y = (WINDOW_HEIGHT - display_height) / 2.0;
                    render_image(window, window_surface, image_surface, width, height, zoom, image_x, image_y);
                }
            }

            if (event.type == SDL_MOUSEWHEEL) {
                int mouse_x, mouse_y;
                SDL_GetMouseState(&mouse_x, &mouse_y);

                double old_scale = base_scale * zoom;
                double image_pixel_x = (mouse_x - image_x) / old_scale;
                double image_pixel_y = (mouse_y - image_y) / old_scale;

                int wheel_y = event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    wheel_y = -wheel_y;
                }

                if (wheel_y > 0) {
                    zoom *= ZOOM_STEP;
                    if (zoom > MAX_ZOOM) zoom = MAX_ZOOM;
                } else if (wheel_y < 0) {
                    zoom /= ZOOM_STEP;
                    if (zoom < MIN_ZOOM) zoom = MIN_ZOOM;
                }

                double new_scale = base_scale * zoom;
                image_x = mouse_x - image_pixel_x * new_scale;
                image_y = mouse_y - image_pixel_y * new_scale;

                clamp_image_position(&image_x, &image_y, width, height, base_scale, zoom);

                if (zoom <= 1.000001) {
                    dragging = 0;
                }

                render_image(window, window_surface, image_surface, width, height, zoom, image_x, image_y);
            }

            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                if (zoom > 1.000001) {
                    dragging = 1;
                }
            }

            if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                dragging = 0;
            }

            if (event.type == SDL_MOUSEMOTION && dragging && zoom > 1.000001) {
                image_x += event.motion.xrel;
                image_y += event.motion.yrel;
                clamp_image_position(&image_x, &image_y, width, height, base_scale, zoom);
                render_image(window, window_surface, image_surface, width, height, zoom, image_x, image_y);
            }
        }
        SDL_Delay(10);
    }

    SDL_FreeSurface(image_surface);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;

fail_input:
    if (should_close_input && input) {
        fclose(input);
    }
    return 1;
}
