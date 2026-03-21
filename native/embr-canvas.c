/* embr-canvas.c -- Emacs dynamic module for canvas frame blitting.
 *
 * Decodes JPEG data via libjpeg-turbo and writes pixels directly
 * into an Emacs canvas buffer, bypassing the Elisp image pipeline.
 *
 * Requires: Emacs 31+ with canvas patch, libjpeg-turbo.
 *
 * Build:  make -C native
 * Load:   (module-load "native/embr-canvas.so")
 */

#include <emacs-module.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

int plugin_is_GPL_compatible;

/* True if the running Emacs has canvas_pixel/canvas_refresh. */
static int canvas_api_available = 0;

/* Custom libjpeg error manager that longjmps instead of exit(). */
struct embr_jpeg_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf escape;
};

static void
embr_jpeg_error_exit (j_common_ptr cinfo)
{
  struct embr_jpeg_error_mgr *mgr =
    (struct embr_jpeg_error_mgr *) cinfo->err;
  longjmp (mgr->escape, 1);
}

/* ── embr-canvas-supported-p ─────────────────────────────────── */

static emacs_value
Fembr_canvas_supported_p (emacs_env *env, ptrdiff_t nargs,
                          emacs_value *args, void *data)
{
  (void)nargs; (void)args; (void)data;
  return env->intern (env, canvas_api_available ? "t" : "nil");
}

/* ── embr-canvas-blit-jpeg ───────────────────────────────────── */
/* Args: CANVAS  JPEG-DATA  WIDTH  HEIGHT  SEQ                   */

static emacs_value
Fembr_canvas_blit_jpeg (emacs_env *env, ptrdiff_t nargs,
                        emacs_value *args, void *data)
{
  (void)nargs; (void)data;
  if (!canvas_api_available)
    return env->intern (env, "nil");

  /* 1. Get canvas pixel buffer. */
  uint32_t *pixel = env->canvas_pixel (env, args[0]);
  if (!pixel)
    return env->intern (env, "nil");

  /* 2. Extract JPEG bytes from unibyte string. */
  ptrdiff_t buf_len = 0;
  if (!env->copy_string_contents (env, args[1], NULL, &buf_len))
    return env->intern (env, "nil");

  unsigned char *jpeg_buf = malloc (buf_len);
  if (!jpeg_buf)
    return env->intern (env, "nil");
  env->copy_string_contents (env, args[1], (char *)jpeg_buf, &buf_len);
  /* buf_len includes trailing NUL added by copy_string_contents. */
  ptrdiff_t jpeg_len = buf_len - 1;

  int canvas_w = (int)env->extract_integer (env, args[2]);
  int canvas_h = (int)env->extract_integer (env, args[3]);
  /* args[4] is seq (used by Elisp for stale detection, ignored here). */

  /* 3. Set up libjpeg with custom error handler (longjmp, not exit). */
  struct jpeg_decompress_struct cinfo;
  struct embr_jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error (&jerr.pub);
  jerr.pub.error_exit = embr_jpeg_error_exit;

  unsigned char *row = NULL;
  emacs_value result = env->intern (env, "nil");

  if (setjmp (jerr.escape))
    {
      /* libjpeg hit a fatal error.  Clean up and return nil. */
      jpeg_destroy_decompress (&cinfo);
      free (row);
      free (jpeg_buf);
      return env->intern (env, "nil");
    }

  jpeg_create_decompress (&cinfo);
  jpeg_mem_src (&cinfo, jpeg_buf, (unsigned long)jpeg_len);

  if (jpeg_read_header (&cinfo, TRUE) != JPEG_HEADER_OK)
    {
      jpeg_destroy_decompress (&cinfo);
      free (jpeg_buf);
      return env->intern (env, "nil");
    }

  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress (&cinfo);

  int img_w = (int)cinfo.output_width;
  int img_h = (int)cinfo.output_height;
  int row_stride = img_w * cinfo.output_components;

  int copy_w = img_w < canvas_w ? img_w : canvas_w;
  int copy_h = img_h < canvas_h ? img_h : canvas_h;

  row = malloc (row_stride > 0 ? row_stride : 1);
  if (!row)
    {
      jpeg_destroy_decompress (&cinfo);
      free (jpeg_buf);
      return env->intern (env, "nil");
    }

  /* 4. Write decoded RGB -> ARGB32 into canvas pixel buffer. */
  int y = 0;
  while (cinfo.output_scanline < cinfo.output_height)
    {
      unsigned char *rp = row;
      jpeg_read_scanlines (&cinfo, &rp, 1);
      if (y < copy_h)
        {
          uint32_t *dst = pixel + y * canvas_w;
          for (int x = 0; x < copy_w; x++)
            {
              uint32_t r = rp[x * 3];
              uint32_t g = rp[x * 3 + 1];
              uint32_t b = rp[x * 3 + 2];
              dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
      y++;
    }

  jpeg_finish_decompress (&cinfo);
  jpeg_destroy_decompress (&cinfo);
  free (row);
  free (jpeg_buf);

  /* 5. Tell Emacs the canvas changed. */
  env->canvas_refresh (env, args[0]);
  result = env->intern (env, "t");
  return result;
}

/* ── embr-canvas-version ─────────────────────────────────────── */

static emacs_value
Fembr_canvas_version (emacs_env *env, ptrdiff_t nargs,
                      emacs_value *args, void *data)
{
  (void)nargs; (void)args; (void)data;
  return env->make_string (env, "1.0.0", 5);
}

/* ── Module init ─────────────────────────────────────────────── */

int
emacs_module_init (struct emacs_runtime *ert)
{
  if (ert->size < (ptrdiff_t) sizeof (*ert))
    return 1;
  emacs_env *env = ert->get_environment (ert);

  /* Detect canvas API by checking env struct size. */
  canvas_api_available = (env->size >= (ptrdiff_t) sizeof (*env));

  emacs_value defalias = env->intern (env, "defalias");
  emacs_value func;
  emacs_value sym;

  func = env->make_function (env, 0, 0, Fembr_canvas_supported_p,
    "Return t if canvas pixel API is available.", NULL);
  sym = env->intern (env, "embr-canvas-supported-p");
  env->funcall (env, defalias, 2, (emacs_value[]){sym, func});

  func = env->make_function (env, 5, 5, Fembr_canvas_blit_jpeg,
    "Decode JPEG-DATA and blit to CANVAS at WIDTH x HEIGHT.\n"
    "SEQ is the frame sequence number (used by caller for ordering).", NULL);
  sym = env->intern (env, "embr-canvas-blit-jpeg");
  env->funcall (env, defalias, 2, (emacs_value[]){sym, func});

  func = env->make_function (env, 0, 0, Fembr_canvas_version,
    "Return embr-canvas module version string.", NULL);
  sym = env->intern (env, "embr-canvas-version");
  env->funcall (env, defalias, 2, (emacs_value[]){sym, func});

  env->funcall (env, env->intern (env, "provide"),
    1, (emacs_value[]){env->intern (env, "embr-canvas")});

  return 0;
}
