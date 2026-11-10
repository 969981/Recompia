#include <cairo.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libwebp's runtime is present in the build image even when its headers are not. */
size_t WebPEncodeRGBA(const uint8_t *rgba, int width, int height, int stride,
                      float quality_factor, uint8_t **output);
void WebPFree(void *ptr);

static void die(const char *message) {
  fprintf(stderr, "%s\n", message);
  exit(1);
}

static uint8_t *render(const char *source, int width, int height) {
  GError *error = NULL;
  RsvgHandle *handle = rsvg_handle_new_from_file(source, &error);
  if (!handle) die(error ? error->message : "could not load SVG");
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);
  RsvgRectangle viewport = {0, 0, width, height};
  if (!rsvg_handle_render_document(handle, cr, &viewport, &error))
    die(error ? error->message : "could not render SVG");
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  int stride = cairo_image_surface_get_stride(surface);
  const uint8_t *argb = cairo_image_surface_get_data(surface);
  uint8_t *rgba = malloc((size_t)width * height * 4);
  if (!rgba) die("out of memory");
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const uint8_t *s = argb + (size_t)y * stride + (size_t)x * 4;
      uint8_t *d = rgba + ((size_t)y * width + x) * 4;
      d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
    }
  }
  cairo_surface_destroy(surface);
  g_object_unref(handle);
  return rgba;
}

/* Box-downsample the sharp render, keeping the two images aligned. */
static uint8_t *downsample(const uint8_t *sharp, int width, int height, int factor) {
  uint8_t *small = malloc((size_t)width * height * 4);
  if (!small) die("out of memory");
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < 3; c++) {
        unsigned sum = 0;
        for (int dy = 0; dy < factor; dy++)
          for (int dx = 0; dx < factor; dx++)
            sum += sharp[((size_t)(y * factor + dy) * width * factor + x * factor + dx) * 4 + c];
        small[((size_t)y * width + x) * 4 + c] = (uint8_t)((sum + factor * factor / 2) / (factor * factor));
      }
      small[((size_t)y * width + x) * 4 + 3] = 255;
    }
  }
  return small;
}

static double clamp(double value) {
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* CSS filter: blur(...) saturate(120%), in sRGB, with no alpha. */
static uint8_t *soften(const uint8_t *rgba, int width, int height, double radius) {
  int support = (int)ceil(radius * 3);
  double *kernel = malloc((size_t)(support * 2 + 1) * sizeof(double));
  size_t pixels = (size_t)width * height;
  double *a = malloc(pixels * 3 * sizeof(double));
  double *b = malloc(pixels * 3 * sizeof(double));
  uint8_t *out = malloc(pixels * 4);
  if (!kernel || !a || !b || !out) die("out of memory");
  double sum = 0;
  for (int i = -support; i <= support; i++) {
    double weight = exp(-(double)i * i / (2 * radius * radius));
    kernel[i + support] = weight;
    sum += weight;
  }
  for (int i = 0; i <= support * 2; i++) kernel[i] /= sum;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < 3; c++) {
        double value = 0;
        for (int i = -support; i <= support; i++) {
          int sx = x + i;
          if (sx < 0) sx = 0;
          if (sx >= width) sx = width - 1;
          value += kernel[i + support] * rgba[((size_t)y * width + sx) * 4 + c];
        }
        a[((size_t)y * width + x) * 3 + c] = value;
      }
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < 3; c++) {
        double value = 0;
        for (int i = -support; i <= support; i++) {
          int sy = y + i;
          if (sy < 0) sy = 0;
          if (sy >= height) sy = height - 1;
          value += kernel[i + support] * a[((size_t)sy * width + x) * 3 + c];
        }
        b[((size_t)y * width + x) * 3 + c] = value;
      }
    }
  }
  /* Filter Effects saturation matrix, with Rec.709 luminance weights. */
  for (size_t i = 0; i < pixels; i++) {
    double r = b[i * 3], g = b[i * 3 + 1], bl = b[i * 3 + 2];
    out[i * 4] = (uint8_t)lround(clamp(1.1574 * r - 0.1430 * g - 0.0144 * bl));
    out[i * 4 + 1] = (uint8_t)lround(clamp(-0.0426 * r + 1.0570 * g - 0.0144 * bl));
    out[i * 4 + 2] = (uint8_t)lround(clamp(-0.0426 * r - 0.1430 * g + 1.1856 * bl));
    out[i * 4 + 3] = 255;
  }
  free(kernel); free(a); free(b);
  return out;
}

static void encode(const char *path, uint8_t *rgba, int width, int height, float quality) {
  uint8_t *webp = NULL;
  size_t size = WebPEncodeRGBA(rgba, width, height, width * 4, quality, &webp);
  if (!size) die("WebP encoding failed");
  FILE *file = fopen(path, "wb");
  if (!file || fwrite(webp, 1, size, file) != size || fclose(file)) die("could not write WebP");
  WebPFree(webp);
  free(rgba);
}

/* The Sunrise theme's amber tint, baked in. This is the CSS filter it used to put on the art layer, as the Filter Effects spec
 * defines it, so the picture is the same but the browser has nothing to recompute when it repaints:
 *   sepia(.85) saturate(2.4) hue-rotate(-12deg) brightness(1.04), each step clamped to 0..1 like a browser does. */
static void tint_sunrise(uint8_t *rgba, int width, int height) {
  const double a = 0.85, s = 2.4, br = 1.04, k = 1 - a, th = -12.0 * 3.14159265358979323846 / 180.0, c = cos(th), n = sin(th);
  const double M[4][3][3] = {
    {{0.393 + 0.607 * k, 0.769 - 0.769 * k, 0.189 - 0.189 * k},
     {0.349 - 0.349 * k, 0.686 + 0.314 * k, 0.168 - 0.168 * k},
     {0.272 - 0.272 * k, 0.534 - 0.534 * k, 0.131 + 0.869 * k}},
    {{0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s},
     {0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s},
     {0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s}},
    {{0.213 + c * 0.787 - n * 0.213, 0.715 - c * 0.715 - n * 0.715, 0.072 - c * 0.072 + n * 0.928},
     {0.213 - c * 0.213 + n * 0.143, 0.715 + c * 0.285 + n * 0.140, 0.072 - c * 0.072 - n * 0.283},
     {0.213 - c * 0.213 - n * 0.787, 0.715 - c * 0.715 + n * 0.715, 0.072 + c * 0.928 + n * 0.072}},
    {{br, 0, 0}, {0, br, 0}, {0, 0, br}}};
  for (size_t i = 0, count = (size_t)width * height; i < count; i++) {
    double v[3] = {rgba[i * 4] / 255.0, rgba[i * 4 + 1] / 255.0, rgba[i * 4 + 2] / 255.0};
    for (int step = 0; step < 4; step++) {
      double o[3];
      for (int r = 0; r < 3; r++) {
        o[r] = M[step][r][0] * v[0] + M[step][r][1] * v[1] + M[step][r][2] * v[2];
        o[r] = o[r] < 0 ? 0 : o[r] > 1 ? 1 : o[r];
      }
      v[0] = o[0]; v[1] = o[1]; v[2] = o[2];
    }
    for (int r = 0; r < 3; r++) rgba[i * 4 + r] = (uint8_t)lround(v[r] * 255.0);
  }
}

static uint8_t *copy(const uint8_t *rgba, int width, int height) {
  size_t size = (size_t)width * height * 4;
  uint8_t *out = malloc(size);
  if (!out) die("out of memory");
  memcpy(out, rgba, size);
  return out;
}

/* The sharp image is drawn at 2560x1600: a 1040x600 window at 2x scale is 2080x1200, and a maximised 1440p window still
 * scales down. Anything larger only costs decode time and memory (3840x2400 was a 37 MB bitmap). */
#define SHARP_W 2560
#define SHARP_H 1600
#define SHARP_QUALITY 84

int main(int argc, char **argv) {
  if (argc != 6 && argc != 4)
    die("usage: render-art SOURCE.svg SHARP.webp SOFT.webp [SHARP-SUNRISE.webp SOFT-SUNRISE.webp]");
  uint8_t *sharp = render(argv[1], SHARP_W, SHARP_H);
  /* Regenerate the small image from the sharp render, then blur at its own scale. */
  uint8_t *small = downsample(sharp, 160, 100, SHARP_W / 160);
  /* At 160px wide, an 8px Gaussian radius is 56px at the displayed window scale. */
  uint8_t *soft = soften(small, 160, 100, 8);
  if (argc == 4) {   /* a picture with no Sunrise variant (the Meadow theme) */
    encode(argv[2], sharp, SHARP_W, SHARP_H, SHARP_QUALITY);
    encode(argv[3], soft, 160, 100, 85);
    free(small);
    return 0;
  }
  uint8_t *sharp_sunrise = copy(sharp, SHARP_W, SHARP_H);
  uint8_t *soft_sunrise = copy(soft, 160, 100);
  tint_sunrise(sharp_sunrise, SHARP_W, SHARP_H);
  tint_sunrise(soft_sunrise, 160, 100);
  encode(argv[2], sharp, SHARP_W, SHARP_H, SHARP_QUALITY);
  encode(argv[3], soft, 160, 100, 85);
  encode(argv[4], sharp_sunrise, SHARP_W, SHARP_H, SHARP_QUALITY);
  encode(argv[5], soft_sunrise, 160, 100, 85);
  free(small);
  return 0;
}
