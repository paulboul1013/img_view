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


    if (fscanf(
            input,
            "%2s",
            magic
        ) != 1) {

        fprintf(stderr,
                "Failed to read PPM magic\n");

        goto fail_input;
    }


    if (strcmp(
            magic,
            "P6"
        ) != 0) {

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