#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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


/*
 * Stage 3: IHDR 的 13-byte metadata。
 *
 * PNG IHDR data layout:
 *
 *     4 bytes  width
 *     4 bytes  height
 *     1 byte   bit depth
 *     1 byte   color type
 *     1 byte   compression method
 *     1 byte   filter method
 *     1 byte   interlace method
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t compression_method;
    uint8_t filter_method;
    uint8_t interlace_method;
} PNGHeader;


/*
 * Stage 1: 只辨識輸入圖片格式。
 *
 * 為了同時支援：
 *     ./main image.ppm
 *     ./main < image.ppm
 *     ./main image.png
 *     ./main < image.png
 *
 * 這裡不使用 fseek()/rewind()，因為 stdin 可能來自 pipe。
 * 我們先讀前 2 bytes：
 *
 *     PPM P6 : 'P' '6'
 *     PNG    : 0x89 'P' ...
 *
 * 若是 PPM，這兩個 bytes 就視為已經消耗掉 magic number，
 * main() 接著直接讀 width / height / maxval。
 *
 * 若可能是 PNG，再補讀剩下 6 bytes 驗證完整 8-byte signature。
 */
static int detect_image_format(
    FILE *input,
    ImageFormat *format
)
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

    /* PPM binary pixmap magic number: P6 */
    if (b0 == 'P' && b1 == '6') {
        *format = IMAGE_FORMAT_PPM_P6;
        return 1;
    }

    /*
     * PNG 的前兩個 bytes 必須是 0x89 0x50。
     * 只有符合時才需要繼續讀剩下的 6 bytes。
     */
    if ((unsigned char)b0 == 0x89 &&
        (unsigned char)b1 == 0x50) {

        unsigned char signature[8];

        signature[0] = (unsigned char)b0;
        signature[1] = (unsigned char)b1;

        if (fread(
                signature + 2,
                1,
                6,
                input
            ) != 6) {

            return 0;
        }

        if (memcmp(
                signature,
                png_signature,
                sizeof(png_signature)
            ) == 0) {

            *format = IMAGE_FORMAT_PNG;
            return 1;
        }
    }

    *format = IMAGE_FORMAT_UNKNOWN;
    return 1;
}



/*
 * Stage 2 helper:
 * PNG 中的 4-byte integer 使用 big-endian（高位 byte 在前）。
 *
 * 例如：
 *     00 00 00 0D
 *          ->
 *     13
 */
static int read_u32_be(
    FILE *input,
    uint32_t *value
)
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


/*
 * 跳過目前 Stage 2 還不解析的 chunk data。
 *
 * 不使用 fseek()，因為 input 可能是：
 *     ./main < image.png
 * 或 pipe，這些 stream 不一定可以 seek。
 */

/*
 * 從 memory 中的 4 bytes 讀 big-endian uint32_t。
 *
 * Stage 2 的 read_u32_be() 是從 FILE 讀；
 * Stage 3 解析 IHDR 時，13 bytes 已經先讀進 memory，
 * 所以需要 memory 版本。
 */
static uint32_t u32_be_from_bytes(
    const unsigned char *b
)
{
    return
        ((uint32_t)b[0] << 24) |
        ((uint32_t)b[1] << 16) |
        ((uint32_t)b[2] << 8)  |
        ((uint32_t)b[3]);
}


/*
 * 把 IHDR 的固定 13 bytes 拆成 PNGHeader。
 */
static int parse_png_ihdr(
    const unsigned char *data,
    uint32_t length,
    PNGHeader *header
)
{
    if (length != 13) {
        fprintf(stderr,
                "PNG: IHDR length must be 13, got %u\n",
                (unsigned)length);
        return 0;
    }

    header->width =
        u32_be_from_bytes(data + 0);

    header->height =
        u32_be_from_bytes(data + 4);

    header->bit_depth = data[8];
    header->color_type = data[9];
    header->compression_method = data[10];
    header->filter_method = data[11];
    header->interlace_method = data[12];

    if (header->width == 0 ||
        header->height == 0) {
        fprintf(stderr,
                "PNG: width and height must be non-zero\n");
        return 0;
    }

    return 1;
}


static const char *png_color_type_name(
    uint8_t color_type
)
{
    switch (color_type) {
        case 0:
            return "Grayscale";

        case 2:
            return "Truecolor RGB";

        case 3:
            return "Indexed-color / Palette";

        case 4:
            return "Grayscale + Alpha";

        case 6:
            return "Truecolor RGBA";

        default:
            return "Invalid / Unknown";
    }
}


static void print_png_ihdr(
    const PNGHeader *header
)
{
    printf("\nIHDR metadata\n");
    printf("--------------------------------\n");
    printf("Width       : %u\n",
           (unsigned)header->width);
    printf("Height      : %u\n",
           (unsigned)header->height);
    printf("Bit depth   : %u\n",
           (unsigned)header->bit_depth);
    printf("Color type  : %u (%s)\n",
           (unsigned)header->color_type,
           png_color_type_name(header->color_type));
    printf("Compression : %u\n",
           (unsigned)header->compression_method);
    printf("Filter      : %u\n",
           (unsigned)header->filter_method);
    printf("Interlace   : %u (%s)\n",
           (unsigned)header->interlace_method,
           header->interlace_method == 0
               ? "none"
               : header->interlace_method == 1
                   ? "Adam7"
                   : "invalid");
    printf("--------------------------------\n\n");
}


static int skip_stream_bytes(
    FILE *input,
    uint32_t count
)
{
    unsigned char buffer[4096];

    while (count > 0) {

        size_t amount =
            count < sizeof(buffer)
                ? (size_t)count
                : sizeof(buffer);

        if (fread(buffer, 1, amount, input) != amount) {
            return 0;
        }

        count -= (uint32_t)amount;
    }

    return 1;
}


/*
 * Stage 3: 走訪 PNG chunk，並解析 IHDR metadata。
 *
 * PNG Signature 已經由 detect_image_format() 讀掉，因此此函式
 * 一進來時，FILE position 正好指向第一個 chunk 的 Length。
 *
 * 每個 chunk 固定是：
 *
 *     4 bytes  Length
 *     4 bytes  Type
 *     N bytes  Data
 *     4 bytes  CRC
 *
 * 本階段新增：
 *     - IHDR 13-byte metadata parsing
 *
 * 還不做：
 *     - IDAT 保存
 *     - CRC 驗證
 *     - zlib inflate
 */
static int inspect_png_chunks(
    FILE *input,
    PNGHeader *header
)
{
    unsigned int chunk_index = 0;
    int got_ihdr = 0;

    while (1) {
        uint32_t length;
        uint32_t crc;
        char type[5];

        /* 1. Length */
        if (!read_u32_be(input, &length)) {
            fprintf(stderr,
                    "PNG: failed to read chunk length\n");
            return 0;
        }

        if (length > 0x7FFFFFFFU) {
            fprintf(stderr,
                    "PNG: invalid chunk length: %u\n",
                    (unsigned)length);
            return 0;
        }

        /* 2. Type */
        if (fread(type, 1, 4, input) != 4) {
            fprintf(stderr,
                    "PNG: failed to read chunk type\n");
            return 0;
        }

        type[4] = '\0';

        /*
         * 3. Data
         *
         * Stage 3 只有 IHDR 真的讀入並解析。
         * 其他 chunk（包含 IDAT）仍然全部跳過。
         */
        if (memcmp(type, "IHDR", 4) == 0) {

            unsigned char ihdr_data[13];

            if (got_ihdr) {
                fprintf(stderr,
                        "PNG: duplicate IHDR chunk\n");
                return 0;
            }

            if (chunk_index != 0) {
                fprintf(stderr,
                        "PNG: IHDR must be the first chunk\n");
                return 0;
            }

            if (length != sizeof(ihdr_data)) {
                fprintf(stderr,
                        "PNG: IHDR length must be 13, got %u\n",
                        (unsigned)length);
                return 0;
            }

            if (fread(
                    ihdr_data,
                    1,
                    sizeof(ihdr_data),
                    input
                ) != sizeof(ihdr_data)) {

                fprintf(stderr,
                        "PNG: truncated IHDR data\n");
                return 0;
            }

            if (!parse_png_ihdr(
                    ihdr_data,
                    length,
                    header)) {

                return 0;
            }

            got_ihdr = 1;
        }
        else {
            if (!skip_stream_bytes(
                    input,
                    length)) {

                fprintf(stderr,
                        "PNG: truncated chunk data for %.4s\n",
                        type);
                return 0;
            }
        }

        /* 4. CRC -- 目前仍只讀、不驗證 */
        if (!read_u32_be(input, &crc)) {
            fprintf(stderr,
                    "PNG: failed to read CRC for %.4s\n",
                    type);
            return 0;
        }

        printf(
            "Chunk %u: type=%.4s, length=%u, crc=0x%08X\n",
            chunk_index,
            type,
            (unsigned)length,
            (unsigned)crc
        );

        if (memcmp(type, "IHDR", 4) == 0) {
            print_png_ihdr(header);
        }

        ++chunk_index;

        if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    if (!got_ihdr) {
        fprintf(stderr,
                "PNG: missing IHDR chunk\n");
        return 0;
    }

    return 1;
}

/*
 * 計算圖片剛好 contain 在 Window 中的倍率。
 */
static double get_base_scale(
    int image_width,
    int image_height
)
{
    double scale_x =
        (double)WINDOW_WIDTH /
        (double)image_width;

    double scale_y =
        (double)WINDOW_HEIGHT /
        (double)image_height;

    return scale_x < scale_y
        ? scale_x
        : scale_y;
}


/*
 * 限制圖片可移動的範圍。
 *
 * 規則：
 * 1. 某一軸的圖片尺寸 <= Window 時，該軸固定置中。
 * 2. 某一軸的圖片尺寸 > Window 時，只允許在
 *    [WindowSize - ImageSize, 0] 範圍內移動。
 *
 * 這可以避免把圖片拖到完全離開視窗、露出大片黑邊。
 */
static void clamp_image_position(
    double *image_x,
    double *image_y,
    int image_width,
    int image_height,
    double base_scale,
    double zoom
)
{
    double final_scale =
        base_scale * zoom;

    double scaled_width =
        image_width * final_scale;

    double scaled_height =
        image_height * final_scale;


    /* X 軸 */
    if (scaled_width <= WINDOW_WIDTH) {
        *image_x =
            (WINDOW_WIDTH - scaled_width) / 2.0;
    } else {
        double min_x =
            WINDOW_WIDTH - scaled_width;

        if (*image_x > 0.0) {
            *image_x = 0.0;
        }

        if (*image_x < min_x) {
            *image_x = min_x;
        }
    }


    /* Y 軸 */
    if (scaled_height <= WINDOW_HEIGHT) {
        *image_y =
            (WINDOW_HEIGHT - scaled_height) / 2.0;
    } else {
        double min_y =
            WINDOW_HEIGHT - scaled_height;

        if (*image_y > 0.0) {
            *image_y = 0.0;
        }

        if (*image_y < min_y) {
            *image_y = min_y;
        }
    }
}


/*
 * 將 SDL_Surface 順時針旋轉 90 度。
 *
 * 原圖尺寸：W x H
 * 新圖尺寸：H x W
 *
 * 座標映射：
 *
 *     src(x, y)
 *         ->
 *     dst(H - 1 - y, x)
 *
 * 此程式目前的 image_surface 固定是 32-bit ARGB8888，
 * 因此可以用 Uint32 逐 pixel 複製。
 */
static SDL_Surface *rotate_surface_90_clockwise(
    SDL_Surface *src
)
{
    int src_width = src->w;
    int src_height = src->h;

    SDL_Surface *dst =
        SDL_CreateRGBSurfaceWithFormat(
            0,
            src_height,   /* new width  = old height */
            src_width,    /* new height = old width  */
            32,
            src->format->format
        );

    if (!dst) {
        fprintf(stderr,
                "rotate: SDL_CreateRGBSurfaceWithFormat: %s\n",
                SDL_GetError());
        return NULL;
    }

    int src_locked = 0;
    int dst_locked = 0;

    if (SDL_MUSTLOCK(src)) {
        if (SDL_LockSurface(src) != 0) {
            fprintf(stderr,
                    "rotate: SDL_LockSurface(src): %s\n",
                    SDL_GetError());
            SDL_FreeSurface(dst);
            return NULL;
        }
        src_locked = 1;
    }

    if (SDL_MUSTLOCK(dst)) {
        if (SDL_LockSurface(dst) != 0) {
            fprintf(stderr,
                    "rotate: SDL_LockSurface(dst): %s\n",
                    SDL_GetError());

            if (src_locked) {
                SDL_UnlockSurface(src);
            }

            SDL_FreeSurface(dst);
            return NULL;
        }
        dst_locked = 1;
    }

    for (int y = 0; y < src_height; ++y) {

        Uint32 *src_row =
            (Uint32 *)((Uint8 *)src->pixels +
                       y * src->pitch);

        for (int x = 0; x < src_width; ++x) {

            int dst_x =
                src_height - 1 - y;

            int dst_y = x;

            Uint32 *dst_row =
                (Uint32 *)((Uint8 *)dst->pixels +
                           dst_y * dst->pitch);

            dst_row[dst_x] =
                src_row[x];
        }
    }

    if (dst_locked) {
        SDL_UnlockSurface(dst);
    }

    if (src_locked) {
        SDL_UnlockSurface(src);
    }

    return dst;
}


/*
 * Render image。
 *
 * image_x / image_y：
 *     圖片左上角在 Window 中的位置。
 *
 * zoom：
 *     相對於 contain size 的縮放倍率。
 */
static void render_image(
    SDL_Window *window,
    SDL_Surface *window_surface,
    SDL_Surface *image_surface,
    int image_width,
    int image_height,
    double zoom,
    double image_x,
    double image_y
)
{
    double base_scale =
        get_base_scale(
            image_width,
            image_height
        );

    double final_scale =
        base_scale * zoom;


    int scaled_width =
        (int)(image_width *
              final_scale);

    int scaled_height =
        (int)(image_height *
              final_scale);


    SDL_Rect dst_rect;

    /*
     * 現在不再自動置中。
     *
     * 圖片位置完全由 viewport state
     * image_x/image_y 控制。
     */
    dst_rect.x = (int)image_x;
    dst_rect.y = (int)image_y;

    dst_rect.w = scaled_width;
    dst_rect.h = scaled_height;


    /*
     * 清除上一幀。
     */
    Uint32 background =
        SDL_MapRGB(
            window_surface->format,
            0,
            0,
            0
        );

    SDL_FillRect(
        window_surface,
        NULL,
        background
    );


    /*
     * Scale + Blit
     */
    if (SDL_BlitScaled(
            image_surface,
            NULL,
            window_surface,
            &dst_rect
        ) != 0) {

        fprintf(stderr,
                "SDL_BlitScaled: %s\n",
                SDL_GetError());
    }


    SDL_UpdateWindowSurface(window);
}


int main(int argc, char *argv[])
{
    FILE *input = stdin;
    int should_close_input = 0;


    /*
     * ================================
     * 1. Input
     * ================================
     */

    if (argc == 2) {

        input =
            fopen(argv[1], "rb");

        if (!input) {

            fprintf(stderr,
                    "Failed to open file: %s\n",
                    argv[1]);

            return 1;
        }

        should_close_input = 1;

    } else if (argc > 2) {

        fprintf(stderr,
                "Usage:\n"
                "  %s < image.ppm\n"
                "  %s image.ppm\n"
                "  %s < image.png    (Stage 3 IHDR inspection)\n"
                "  %s image.png      (Stage 3 IHDR inspection)\n",
                argv[0],
                argv[0],
                argv[0],
                argv[0]);

        return 1;
    }


    /*
     * ================================
     * 2. Stage 1 - Detect image format
     * ================================
     */

    ImageFormat format = IMAGE_FORMAT_UNKNOWN;

    if (!detect_image_format(input, &format)) {
        fprintf(stderr,
                "Failed to read image signature\n");
        goto fail_input;
    }

    if (format == IMAGE_FORMAT_PNG) {
        PNGHeader png_header = {0};

        printf("PNG detected\n");
        printf("Stage 3: parsing PNG chunks + IHDR metadata...\n\n");

        if (!inspect_png_chunks(
                input,
                &png_header)) {

            goto fail_input;
        }

        printf("Stage 3 complete: IHDR metadata parsed successfully.\n");
        printf("IDAT image data is still not decoded.\n");

        if (should_close_input) {
            fclose(input);
        }

        return 0;
    }

    if (format != IMAGE_FORMAT_PPM_P6) {
        fprintf(stderr,
                "Unsupported image format\n");
        goto fail_input;
    }

    printf("PPM P6 detected\n");


    /*
     * ================================
     * 3. Parse remaining PPM header
     * ================================
     *
     * detect_image_format() 已經吃掉了 'P' '6'，
     * 因此這裡直接從 width / height 開始讀。
     */

    int width;
    int height;
    int maxval;


    if (fscanf(
            input,
            "%d %d",
            &width,
            &height
        ) != 2) {

        fprintf(stderr,
                "Failed to read image size\n");

        goto fail_input;
    }


    if (fscanf(
            input,
            "%d",
            &maxval
        ) != 1 ||
        maxval != 255) {

        fprintf(stderr,
                "Only maxval=255 is supported\n");

        goto fail_input;
    }


    /*
     * Consume newline after 255.
     */
    fgetc(input);


    /*
     * ================================
     * 3. SDL
     * ================================
     */

    if (SDL_Init(
            SDL_INIT_VIDEO
        ) != 0) {

        fprintf(stderr,
                "SDL_Init: %s\n",
                SDL_GetError());

        goto fail_input;
    }


    SDL_Window *window =
        SDL_CreateWindow(
            "Image Viewer",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            WINDOW_WIDTH,
            WINDOW_HEIGHT,
            0
        );


    if (!window) {

        fprintf(stderr,
                "SDL_CreateWindow: %s\n",
                SDL_GetError());

        SDL_Quit();

        goto fail_input;
    }


    /*
     * ================================
     * 4. Image Surface
     * ================================
     */

    SDL_Surface *image_surface =
        SDL_CreateRGBSurfaceWithFormat(
            0,
            width,
            height,
            32,
            SDL_PIXELFORMAT_ARGB8888
        );


    if (!image_surface) {

        fprintf(stderr,
                "Create image surface: %s\n",
                SDL_GetError());

        SDL_DestroyWindow(window);
        SDL_Quit();

        goto fail_input;
    }


    /*
     * ================================
     * 5. Decode PPM
     * ================================
     */

    if (SDL_MUSTLOCK(
            image_surface)) {

        if (SDL_LockSurface(
                image_surface
            ) != 0) {

            fprintf(stderr,
                    "SDL_LockSurface: %s\n",
                    SDL_GetError());

            SDL_FreeSurface(
                image_surface);

            SDL_DestroyWindow(
                window);

            SDL_Quit();

            goto fail_input;
        }
    }


    for (int y = 0;
         y < height;
         ++y) {

        Uint8 *row =
            (Uint8 *)
                image_surface->pixels +
            y *
                image_surface->pitch;


        for (int x = 0;
             x < width;
             ++x) {

            int r = fgetc(input);
            int g = fgetc(input);
            int b = fgetc(input);


            if (r == EOF ||
                g == EOF ||
                b == EOF) {

                fprintf(stderr,
                        "Unexpected end of image\n");

                if (SDL_MUSTLOCK(
                        image_surface)) {

                    SDL_UnlockSurface(
                        image_surface);
                }

                SDL_FreeSurface(
                    image_surface);

                SDL_DestroyWindow(
                    window);

                SDL_Quit();

                goto fail_input;
            }


            Uint32 color =
                SDL_MapRGB(
                    image_surface->format,
                    (Uint8)r,
                    (Uint8)g,
                    (Uint8)b
                );


            ((Uint32 *)row)[x] =
                color;
        }
    }


    if (SDL_MUSTLOCK(
            image_surface)) {

        SDL_UnlockSurface(
            image_surface);
    }


    if (should_close_input) {

        fclose(input);

        input = NULL;

        should_close_input = 0;
    }


    /*
     * ================================
     * 6. Window Surface
     * ================================
     */

    SDL_Surface *window_surface =
        SDL_GetWindowSurface(
            window);


    if (!window_surface) {

        fprintf(stderr,
                "SDL_GetWindowSurface: %s\n",
                SDL_GetError());

        SDL_FreeSurface(
            image_surface);

        SDL_DestroyWindow(
            window);

        SDL_Quit();

        return 1;
    }


    /*
     * ================================
     * 7. Viewport State
     * ================================
     */

    double zoom = 1.0;

    /*
     * Pan / drag state。
     *
     * dragging == 1：
     *     左鍵目前正在拖曳圖片。
     *
     * 注意：只有 zoom > 1.0 時才會進入 dragging。
     */
    int dragging = 0;


    /*
     * zoom = 1.0 時：
     *
     * 圖片先 contain 到 800x600。
     */
    double base_scale =
        get_base_scale(
            width,
            height
        );


    double display_width =
        width * base_scale;

    double display_height =
        height * base_scale;


    /*
     * 初始圖片置中。
     */
    double image_x =
        (WINDOW_WIDTH -
         display_width) / 2.0;

    double image_y =
        (WINDOW_HEIGHT -
         display_height) / 2.0;


    /*
     * 第一次 render。
     */
    render_image(
        window,
        window_surface,
        image_surface,
        width,
        height,
        zoom,
        image_x,
        image_y
    );


    /*
     * ================================
     * 8. Event Loop
     * ================================
     */

    int running = 1;


    while (running) {

        SDL_Event event;


        while (SDL_PollEvent(
                &event)) {


            if (event.type ==
                SDL_QUIT) {

                running = 0;
            }


            if (event.type ==
                    SDL_KEYDOWN &&
                event.key.keysym.sym ==
                    SDLK_ESCAPE) {

                running = 0;
            }


            /*
             * ========================
             * Rotate 90 degrees clockwise
             * ========================
             *
             * 按 R：
             * 1. 建立一張旋轉後的新 Surface。
             * 2. 釋放舊 Surface。
             * 3. width / height 交換。
             * 4. zoom 重設成 1.0。
             * 5. 重新計算 contain scale 並置中。
             */
            if (event.type ==
                    SDL_KEYDOWN &&
                event.key.keysym.sym ==
                    SDLK_r &&
                event.key.repeat == 0) {

                SDL_Surface *rotated_surface =
                    rotate_surface_90_clockwise(
                        image_surface
                    );

                if (rotated_surface) {

                    SDL_FreeSurface(
                        image_surface
                    );

                    image_surface =
                        rotated_surface;


                    /*
                     * 旋轉 90 度後，圖片尺寸互換。
                     *
                     * old: W x H
                     * new: H x W
                     */
                    int old_width = width;
                    width = height;
                    height = old_width;


                    /*
                     * 旋轉後重新 fit 到 800x600。
                     * 同時取消舊的 pan / zoom 狀態，
                     * 避免舊座標套到新的方向。
                     */
                    zoom = 1.0;
                    dragging = 0;

                    base_scale =
                        get_base_scale(
                            width,
                            height
                        );

                    display_width =
                        width * base_scale;

                    display_height =
                        height * base_scale;

                    image_x =
                        (WINDOW_WIDTH -
                         display_width) / 2.0;

                    image_y =
                        (WINDOW_HEIGHT -
                         display_height) / 2.0;


                    printf(
                        "rotate 90 CW: %d x %d\n",
                        width,
                        height
                    );


                    render_image(
                        window,
                        window_surface,
                        image_surface,
                        width,
                        height,
                        zoom,
                        image_x,
                        image_y
                    );
                }
            }


            /*
             * ========================
             * Mouse Drag / Pan
             * ========================
             *
             * 只有 zoom > 1.0 才允許拖曳。
             */

            /* 左鍵按下：開始拖曳 */
            if (event.type ==
                    SDL_MOUSEBUTTONDOWN &&
                event.button.button ==
                    SDL_BUTTON_LEFT) {

                if (zoom > 1.000001) {
                    dragging = 1;
                }
            }


            /* 左鍵放開：結束拖曳 */
            if (event.type ==
                    SDL_MOUSEBUTTONUP &&
                event.button.button ==
                    SDL_BUTTON_LEFT) {

                dragging = 0;
            }


            /*
             * 滑鼠移動時，SDL_MOUSEMOTION 直接提供
             * 相對位移 xrel / yrel。
             */
            if (event.type ==
                    SDL_MOUSEMOTION &&
                dragging &&
                zoom > 1.000001) {

                image_x +=
                    event.motion.xrel;

                image_y +=
                    event.motion.yrel;


                /* 防止拖出合法範圍。 */
                clamp_image_position(
                    &image_x,
                    &image_y,
                    width,
                    height,
                    base_scale,
                    zoom
                );


                render_image(
                    window,
                    window_surface,
                    image_surface,
                    width,
                    height,
                    zoom,
                    image_x,
                    image_y
                );
            }


            /*
             * ========================
             * Cursor-centered Zoom
             * ========================
             */
            if (event.type ==
                SDL_MOUSEWHEEL) {

                /*
                 * --------------------
                 * 1. 取得 mouse
                 * --------------------
                 */

                int mouse_x;
                int mouse_y;

                SDL_GetMouseState(
                    &mouse_x,
                    &mouse_y
                );


                /*
                 * --------------------
                 * 2. old scale
                 * --------------------
                 */

                double old_scale =
                    base_scale * zoom;


                /*
                 * --------------------
                 * 3. Window coordinate
                 *    →
                 *    Image coordinate
                 * --------------------
                 *
                 * 算滑鼠現在指到
                 * 原始圖片哪一點。
                 */

                double image_pixel_x =
                    (mouse_x -
                     image_x) /
                    old_scale;

                double image_pixel_y =
                    (mouse_y -
                     image_y) /
                    old_scale;


                /*
                 * --------------------
                 * 4. 改 zoom
                 * --------------------
                 */

                int wheel_y =
                    event.wheel.y;


                /*
                 * 某些系統會回報 flipped。
                 */
                if (event.wheel.direction ==
                    SDL_MOUSEWHEEL_FLIPPED) {

                    wheel_y =
                        -wheel_y;
                }


                if (wheel_y > 0) {

                    zoom *=
                        ZOOM_STEP;

                    if (zoom >
                        MAX_ZOOM) {

                        zoom =
                            MAX_ZOOM;
                    }
                }


                else if (wheel_y < 0) {

                    zoom /=
                        ZOOM_STEP;

                    if (zoom <
                        MIN_ZOOM) {

                        zoom =
                            MIN_ZOOM;
                    }
                }


                /*
                 * --------------------
                 * 5. new scale
                 * --------------------
                 */

                double new_scale =
                    base_scale *
                    zoom;


                /*
                 * --------------------
                 * 6. Fix anchor
                 * --------------------
                 *
                 * 保證：
                 *
                 * image_pixel_x/y
                 *
                 * 放大後仍然出現在
                 *
                 * mouse_x/y
                 */

                image_x =
                    mouse_x -
                    image_pixel_x *
                    new_scale;

                image_y =
                    mouse_y -
                    image_pixel_y *
                    new_scale;


                /*
                 * zoom <= 1 時不允許 pan；clamp 也會
                 * 自動把不足 Window 大小的軸重新置中。
                 *
                 * zoom > 1 時則限制在合法可拖曳範圍。
                 */
                if (zoom <= 1.000001) {
                    dragging = 0;
                }

                clamp_image_position(
                    &image_x,
                    &image_y,
                    width,
                    height,
                    base_scale,
                    zoom
                );


                printf(
                    "zoom = %.2f "
                    "mouse=(%d,%d) "
                    "image=(%.1f,%.1f)\n",
                    zoom,
                    mouse_x,
                    mouse_y,
                    image_pixel_x,
                    image_pixel_y
                );


                /*
                 * --------------------
                 * 7. Render
                 * --------------------
                 */

                render_image(
                    window,
                    window_surface,
                    image_surface,
                    width,
                    height,
                    zoom,
                    image_x,
                    image_y
                );
            }
        }


        SDL_Delay(10);
    }


    /*
     * ================================
     * 9. Cleanup
     * ================================
     */

    SDL_FreeSurface(
        image_surface);

    SDL_DestroyWindow(
        window);

    SDL_Quit();

    return 0;


fail_input:

    if (should_close_input &&
        input) {

        fclose(input);
    }

    return 1;
}