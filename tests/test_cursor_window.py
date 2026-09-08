#!/usr/bin/env python3
"""Exercise the real ANativeWindow adapter across resize/fbdev transitions."""
from pathlib import Path
import os
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#define open fixture_open
#define ioctl fixture_ioctl
#define close fixture_close
#include "android.c"
#undef open
#undef ioctl
#undef close
static int fb_w, fb_h;
int fixture_open(const char *path, int flags, ...) {
    (void)flags;
    assert(!strcmp(path, "/dev/fb0"));
    return fb_w ? 99 : -1;
}
int fixture_ioctl(int fd, unsigned long op, ...) {
    assert(fd == 99 && op == FBIOGET_VSCREENINFO);
    va_list ap;
    va_start(ap, op);
    struct fb_var_screeninfo *v = va_arg(ap, struct fb_var_screeninfo *);
    va_end(ap);
    memset(v, 0, sizeof *v);
    v->xres = fb_w; v->yres = fb_h;
    return 0;
}
int fixture_close(int fd) { assert(fd == 99); return 0; }
void nx_log(const char *fmt, ...) { (void)fmt; }

static void points(void *window, int rw, int rh) {
    const float xs[] = {0, 160, 640, 1120, 1279};
    const float ys[] = {0, 90, 360, 630, 719};
    for (unsigned x = 0; x < 5; ++x) for (unsigned y = 0; y < 5; ++y) {
        float tx, ty;
        st_window_cursor_to_touch(xs[x], ys[y], &tx, &ty);
        /* Android input is in window space; Unity rescales to rendering
         * resolution. Presentation (stretch or bars) happens afterwards. */
        float ux = tx * rw / a_window_getWidth(window);
        float uy = ty * rh / a_window_getHeight(window);
        assert(fabsf(ux - xs[x] * rw / 1280) < 0.001f);
        assert(fabsf(uy - ys[y] * rh / 720) < 0.001f);
        assert(ux >= 0 && ux < rw && uy >= 0 && uy < rh);
    }
}
int main(void) {
    const int panels[][2] = {{640,480}, {1024,768}, {720,720}, {1280,720}};
    for (unsigned p = 0; p < 4; ++p) {
        int rw = panels[p][0], rh = rw * 9 / 16;
        for (unsigned fb = 0; fb < 3; ++fb) {
            fb_w = fb == 0 ? 0 : fb == 1 ? rw : 640;
            fb_h = fb == 0 ? 0 : fb == 1 ? panels[p][1] : 480;
            st_window_set_size(rw, rh);
            void *window = a_window_fromSurface(NULL, NULL);
            points(window, rw, rh);
            a_window_setBuffersGeometry(window, rw, rh, 1);
            points(window, rw, rh);
            a_window_setBuffersGeometry(window, 0, 0, 0);
            points(window, rw, rh);
            a_window_release(window);
        }
    }
    puts("PASS: 900 cursor points across native-window/fbdev/geometry transitions");
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-cursor-",
                                 dir=os.getenv("TMPDIR")) as tmp:
    source, binary = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
    source.write_text(FIXTURE)
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-O2",
                    "-ffunction-sections", "-fdata-sections",
                    "-I", str(PORT / "src"), str(source), "-Wl,--gc-sections",
                    "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

assert "st_window_cursor_to_touch(cursor_x, cursor_y," in (PORT / "src/input.c").read_text()
assert "st_input_set_touch_rect" not in (PORT / "src/egl.c").read_text()
