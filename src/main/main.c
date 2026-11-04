// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "theme.h"
#include "util.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool renderer_available(const char *name)
{
    if (!name || !*name) return true;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
    int n = SDL_GetNumRenderDrivers();
    for (int i = 0; i < n; i++) {
        SDL_RendererInfo info;
        if (SDL_GetRenderDriverInfo(i, &info) == 0 && info.name && str_ieq(info.name, name)) {
            return true;
        }
    }
    return false;
}

static void print_usage(const char *argv0)
{
    printf("Taiji Desktop %s\n", TAIJI_VERSION);
    printf("Usage: %s [--fullscreen] [--kiosk] [--windowed]\n", argv0);
    printf("          [--display opengl|opengles|vulkan|sdl|software]\n");
    printf("          [--renderer opengl|opengles2|vulkan|software|auto] [--help]\n\n");
    printf("  --fullscreen  start fullscreen (desktop fullscreen)\n");
    printf("  --kiosk       start fullscreen without borders (display-manager session)\n");
    printf("  --windowed    force a normal window (overrides session env)\n");
    printf("  --display N   display backend (default: opengl)\n");
    printf("                opengl | opengles | vulkan | sdl | software\n");
    printf("  --renderer N  SDL render driver: opengl, opengles2, vulkan, software, auto\n");
    printf("  --opengl      shorthand for --renderer opengl\n");
    printf("  --gles        shorthand for --renderer opengles2\n");
    printf("  --vulkan      shorthand for --renderer vulkan\n");
    printf("  --software    shorthand for --renderer software\n");
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    bool fullscreen = false, kiosk = false, windowed = false;
    const char *renderer = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0) fullscreen = true;
        else if (strcmp(argv[i], "--kiosk") == 0) { kiosk = true; fullscreen = true; }
        else if (strcmp(argv[i], "--windowed") == 0) { windowed = true; fullscreen = false; kiosk = false; }
        else if (strcmp(argv[i], "--opengl") == 0) renderer = "opengl";
        else if (strcmp(argv[i], "--gles") == 0 || strcmp(argv[i], "--opengles") == 0) renderer = "opengles2";
        else if (strcmp(argv[i], "--vulkan") == 0) renderer = "vulkan";
        else if (strcmp(argv[i], "--software") == 0) renderer = "software";
        else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) renderer = argv[++i];
        else if (strncmp(argv[i], "--renderer=", 11) == 0) renderer = argv[i] + 11;
        else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) renderer = argv[++i];
        else if (strncmp(argv[i], "--display=", 10) == 0) renderer = argv[i] + 10;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", TAIJI_VERSION);
            return 0;
        }
    }
    if (windowed) {
        unsetenv("TAIJI_FULLSCREEN");
        unsetenv("TAIJI_KIOSK");
    } else if (kiosk) {
        setenv("TAIJI_FULLSCREEN", "1", 1);
        setenv("TAIJI_KIOSK", "1", 1);
    } else if (fullscreen) {
        setenv("TAIJI_FULLSCREEN", "1", 1);
    }

    App app;
    app_init(&app);
    if (!renderer) renderer = getenv("TAIJI_DISPLAY");
    if (!renderer) renderer = getenv("TAIJI_RENDERER");
    if (!renderer && !getenv("SDL_RENDER_DRIVER"))
        renderer = app.config.renderer[0] ? app.config.renderer : "opengles2";
    if (renderer && *renderer && strcmp(renderer, "auto") != 0 && strcmp(renderer, "sdl") != 0) {
        if (strcmp(renderer, "gl") == 0 || strcmp(renderer, "GL") == 0) renderer = "opengl";
        else if (strcmp(renderer, "gles") == 0 || strcmp(renderer, "gles2") == 0 ||
                 strcmp(renderer, "opengles") == 0) renderer = "opengles2";
#ifdef __APPLE__
        /* SDL3's OpenGL ES driver is not usable on macOS; OpenGL is the closest
         * accelerated fallback. Linux ARM/Mali uses the real opengles2 path. */
        if (renderer && strcmp(renderer, "opengles2") == 0) {
            fprintf(stderr, "[taiji] OpenGL ES is not available on macOS, using OpenGL\n");
            renderer = "opengl";
        }
#endif
        else if (strcmp(renderer, "sw") == 0) renderer = "software";
        if (!renderer_available(renderer)) {
            fprintf(stderr, "[taiji] requested renderer '%s' is not available, using auto\n", renderer);
            renderer = NULL;
        } else {
            setenv("SDL_RENDER_DRIVER", renderer, 1);
            if (strcmp(renderer, "opengl") == 0 || strcmp(renderer, "opengles2") == 0) {
                setenv("SDL_RENDER_OPENGL_SHADERS", "1", 0);
            }
        }
    }

    app_run(&app);
    int exit_code = app.exit_code;
    app_free(&app);
    return exit_code;
}
