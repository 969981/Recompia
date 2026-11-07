#include <cairo.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv) {
  if (argc != 4) die("usage: render-art SOURCE.svg SHARP.webp SOFT.webp");
  uint8_t *sharp = render(argv[1], 3840, 2400);
  /* Regenerate the small image from the sharp render, then blur at its own scale. */
  uint8_t *small = downsample(sharp, 160, 100, 24);
  encode(argv[2], sharp, 3840, 2400, 90);
  /* At 160px wide, an 8px Gaussian radius is 56px at the displayed window scale. */
  encode(argv[3], soften(small, 160, 100, 8), 160, 100, 85);
  free(small);
  return 0;
}
