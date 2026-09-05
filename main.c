#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

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
    }
    else if (argc > 2) {
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
     * 2. Parse PPM header
     * ================================
     */

    char magic[3];
    int width;
    int height;
    int maxval;

    if (fscanf(input, "%2s", magic) != 1) {
        fprintf(stderr, "Failed to read PPM magic\n");
        goto fail_input;
    }

    if (strcmp(magic, "P6") != 0) {
        fprintf(stderr, "Only P6 PPM is supported\n");
        goto fail_input;
    }

    if (fscanf(input, "%d %d", &width, &height) != 2) {
        fprintf(stderr, "Failed to read image size\n");
        goto fail_input;
    }

    if (fscanf(input, "%d", &maxval) != 1 ||
        maxval != 255) {

        fprintf(stderr,
                "Only maxval=255 is supported\n");

        goto fail_input;
    }

    /*
     * Skip whitespace after maxval.
     */
    fgetc(input);

    /*
     * ================================
     * 3. Initialize SDL
     * ================================
     */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr,
                "SDL_Init: %s\n",
                SDL_GetError());

        goto fail_input;
    }

    /*
     * Window is ALWAYS 800 x 600.
     */
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
     * 4. Create source image surface
     * ================================
     *
     * Important:
     *
     * image_surface = 原始 PPM 尺寸
     *
     * window_surface = 800 x 600
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
                "SDL_CreateRGBSurfaceWithFormat: %s\n",
                SDL_GetError());

        SDL_DestroyWindow(window);
        SDL_Quit();
        goto fail_input;
    }

    /*
     * ================================
     * 5. Decode PPM → image surface
     * ================================
     */

    if (SDL_MUSTLOCK(image_surface)) {
        if (SDL_LockSurface(image_surface) != 0) {
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

                if (SDL_MUSTLOCK(image_surface)) {
                    SDL_UnlockSurface(image_surface);
                }

                SDL_FreeSurface(image_surface);
                SDL_DestroyWindow(window);
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

            ((Uint32 *)row)[x] = color;
        }
    }

    if (SDL_MUSTLOCK(image_surface)) {
        SDL_UnlockSurface(image_surface);
    }

    /*
     * PPM input has been completely decoded.
     */
    if (should_close_input) {
        fclose(input);
        input = NULL;
        should_close_input = 0;
    }

    /*
     * ================================
     * 6. Window surface
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
     * 7. Calculate aspect-ratio scaling
     * ================================
     */

    double scale_x =
        (double)WINDOW_WIDTH /
        (double)width;

    double scale_y =
        (double)WINDOW_HEIGHT /
        (double)height;

    double scale =
        scale_x < scale_y
            ? scale_x
            : scale_y;

    int scaled_width =
        (int)(width * scale);

    int scaled_height =
        (int)(height * scale);

    /*
     * Center image.
     */
    SDL_Rect dst_rect;

    dst_rect.w = scaled_width;
    dst_rect.h = scaled_height;

    dst_rect.x =
        (WINDOW_WIDTH - scaled_width) / 2;

    dst_rect.y =
        (WINDOW_HEIGHT - scaled_height) / 2;

    printf("Original : %d x %d\n",
           width,
           height);

    printf("Window   : %d x %d\n",
           WINDOW_WIDTH,
           WINDOW_HEIGHT);

    printf("Scaled   : %d x %d\n",
           scaled_width,
           scaled_height);

    printf("Scale    : %.3f\n",
           scale);

    /*
     * ================================
     * 8. Draw
     * ================================
     */

    /*
     * Fill entire window black first.
     *
     * This creates letterbox / pillarbox
     * areas around the image.
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
     * Scale original image_surface
     *
     * width x height
     *
     *          ↓
     *
     * scaled_width x scaled_height
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

    /*
     * ================================
     * 9. Event loop
     * ================================
     */

    int running = 1;

    while (running) {

        SDL_Event event;

        while (SDL_PollEvent(&event)) {

            if (event.type == SDL_QUIT) {
                running = 0;
            }

            if (event.type == SDL_KEYDOWN &&
                event.key.keysym.sym == SDLK_ESCAPE) {

                running = 0;
            }
        }

        SDL_Delay(10);
    }

    /*
     * ================================
     * 10. Cleanup
     * ================================
     */

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