#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

#define ZOOM_STEP 1.1
#define MIN_ZOOM  0.1
#define MAX_ZOOM  10.0


/*
 * 把 image_surface 畫到固定 800x600 window。
 *
 * base_scale:
 *     先把原圖等比例塞進 800x600
 *
 * zoom:
 *     使用者額外的縮放倍率
 *
 * final_scale:
 *     真正顯示倍率
 */
static void render_image(
    SDL_Window *window,
    SDL_Surface *window_surface,
    SDL_Surface *image_surface,
    int image_width,
    int image_height,
    double zoom
)
{
    /*
     * --------------------------------
     * 1. 計算 contain 基礎倍率
     * --------------------------------
     */

    double scale_x =
        (double)WINDOW_WIDTH /
        (double)image_width;

    double scale_y =
        (double)WINDOW_HEIGHT /
        (double)image_height;

    double base_scale =
        scale_x < scale_y
            ? scale_x
            : scale_y;


    /*
     * --------------------------------
     * 2. 加入使用者 zoom
     * --------------------------------
     */

    double final_scale =
        base_scale * zoom;


    int scaled_width =
        (int)(image_width * final_scale);

    int scaled_height =
        (int)(image_height * final_scale);


    /*
     * --------------------------------
     * 3. 圖片保持在視窗中心
     * --------------------------------
     */

    SDL_Rect dst_rect;

    dst_rect.w = scaled_width;
    dst_rect.h = scaled_height;

    dst_rect.x =
        (WINDOW_WIDTH - scaled_width) / 2;

    dst_rect.y =
        (WINDOW_HEIGHT - scaled_height) / 2;


    /*
     * --------------------------------
     * 4. 清除上一幀
     * --------------------------------
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
     * --------------------------------
     * 5. Scale + Blit
     * --------------------------------
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


    /*
     * --------------------------------
     * 6. Present
     * --------------------------------
     */

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

        input = fopen(argv[1], "rb");

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
                "  %s image.ppm\n",
                argv[0],
                argv[0]);

        return 1;
    }


    /*
     * ================================
     * 2. Parse PPM
     * ================================
     */

    char magic[3];
    int width;
    int height;
    int maxval;


    if (fscanf(input, "%2s", magic) != 1) {
        fprintf(stderr,
                "Failed to read PPM magic\n");

        goto fail_input;
    }


    if (strcmp(magic, "P6") != 0) {
        fprintf(stderr,
                "Only P6 PPM is supported\n");

        goto fail_input;
    }


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
     * Consume whitespace after:
     *
     * 255\n
     */
    fgetc(input);


    /*
     * ================================
     * 3. SDL
     * ================================
     */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {

        fprintf(stderr,
                "SDL_Init: %s\n",
                SDL_GetError());

        goto fail_input;
    }


    SDL_Window *window =
        SDL_CreateWindow(
            "PPM Viewer",
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
     * 4. 原始圖片 Surface
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
     * 5. Decode PPM RGB
     * ================================
     */

    if (SDL_MUSTLOCK(image_surface)) {

        if (SDL_LockSurface(
                image_surface
            ) != 0) {

            fprintf(stderr,
                    "SDL_LockSurface: %s\n",
                    SDL_GetError());

            SDL_FreeSurface(image_surface);
            SDL_DestroyWindow(window);
            SDL_Quit();

            goto fail_input;
        }
    }


    for (int y = 0; y < height; ++y) {

        Uint8 *row =
            (Uint8 *)image_surface->pixels +
            y * image_surface->pitch;


        for (int x = 0; x < width; ++x) {

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


    if (SDL_MUSTLOCK(image_surface)) {
        SDL_UnlockSurface(image_surface);
    }


    /*
     * PPM 已經完全讀完。
     */
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
        SDL_GetWindowSurface(window);


    if (!window_surface) {

        fprintf(stderr,
                "SDL_GetWindowSurface: %s\n",
                SDL_GetError());

        SDL_FreeSurface(image_surface);
        SDL_DestroyWindow(window);
        SDL_Quit();

        return 1;
    }


    /*
     * ================================
     * 7. Zoom state
     * ================================
     *
     * zoom = 1.0
     *
     * 表示：
     *
     * 「剛好 contain 在 800x600」
     */

    double zoom = 1.0;


    /*
     * 第一次 Render。
     */

    render_image(
        window,
        window_surface,
        image_surface,
        width,
        height,
        zoom
    );


    /*
     * ================================
     * 8. Event loop
     * ================================
     */

    int running = 1;


    while (running) {

        SDL_Event event;


        while (SDL_PollEvent(&event)) {

            /*
             * 關閉 Window
             */
            if (event.type == SDL_QUIT) {
                running = 0;
            }


            /*
             * ESC 離開
             */
            if (event.type == SDL_KEYDOWN &&
                event.key.keysym.sym ==
                    SDLK_ESCAPE) {

                running = 0;
            }


            /*
             * ========================
             * Mouse Wheel Zoom
             * ========================
             */

            if (event.type ==
                SDL_MOUSEWHEEL) {

                /*
                 * wheel.y > 0
                 *
                 * 滾輪向上
                 * → 放大
                 */

                if (event.wheel.y > 0) {

                    zoom *= ZOOM_STEP;


                    if (zoom > MAX_ZOOM) {
                        zoom = MAX_ZOOM;
                    }
                }


                /*
                 * wheel.y < 0
                 *
                 * 滾輪向下
                 * → 縮小
                 */

                else if (event.wheel.y < 0) {

                    zoom /= ZOOM_STEP;


                    if (zoom < MIN_ZOOM) {
                        zoom = MIN_ZOOM;
                    }
                }


                printf(
                    "zoom = %.2f\n",
                    zoom
                );


                /*
                 * zoom 改變後重新畫圖。
                 */

                render_image(
                    window,
                    window_surface,
                    image_surface,
                    width,
                    height,
                    zoom
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

    SDL_FreeSurface(image_surface);

    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;


fail_input:

    if (should_close_input &&
        input) {

        fclose(input);
    }

    return 1;
}