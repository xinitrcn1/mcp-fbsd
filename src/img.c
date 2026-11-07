/* Images: decode to one RGBA8 buffer, transform in place or into one other
 * buffer, encode. JPEG is decoded at 1/2, 1/4 or 1/8 scale straight from the
 * DCT when the result only needs to be small, which saves most of the work
 * and memory for camera photos. */
#include "mcp.h"
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef HAVE_PNG
#include <png.h>
#endif
#ifdef HAVE_JPEG
#include <jpeglib.h>
#endif

typedef struct { uint32_t w, h; uint8_t *px; uint32_t ow, oh; const char *fmt; } Img;

static int too_big(Res *r, uint64_t w, uint64_t h)
{
	if (!w || !h || w * h > CFG_IMG_PIXELS) return err(r, "image %llux%llu too large", (unsigned long long)w, (unsigned long long)h);
	return 0;
}

/* ---- decode ---- */

/* Netpbm: P2/P3 (ASCII) and P5/P6 (binary) gray/RGB, maxval up to 65535.
 * Needs no library, so a build without libpng still handles images. */
static int pnm_num(const unsigned char **p, const unsigned char *e, unsigned *v)
{
	const unsigned char *s = *p;
	for (;;) {
		while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
		if (s < e && *s == '#') { while (s < e && *s != '\n') s++; continue; }
		break;
	}
	if (s >= e || *s < '0' || *s > '9') return -1;
	uint64_t x = 0;
	while (s < e && *s >= '0' && *s <= '9') { x = x * 10 + (unsigned)(*s++ - '0'); if (x > 0xFFFFFFFFu) return -1; }
	*v = (unsigned)x;
	*p = s;
	return 0;
}

static int load_pnm(Res *r, const char *path, Img *im)
{
	FILE *f = fopen(path, "rb");
	if (!f) return errno_err(r, path);
	Buf b = { 0 };
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f))) {
		buf_put(&b, chunk, k);
		if (b.n > (size_t)CFG_IMG_PIXELS * 6 + 1024) break;
	}
	fclose(f);
	const unsigned char *p = (const unsigned char *)b.p + 2, *e = (const unsigned char *)b.p + b.n;
	char kind = b.p[1];
	unsigned w, h, maxv;
	int rc = T_OK;
	if (pnm_num(&p, e, &w) || pnm_num(&p, e, &h) || pnm_num(&p, e, &maxv) || !maxv || maxv > 65535) {
		rc = err(r, "pnm: bad header");
		goto out;
	}
	if ((rc = too_big(r, w, h))) goto out;
	bool ascii = kind == '2' || kind == '3', rgb = kind == '3' || kind == '6';
	unsigned ch = rgb ? 3 : 1, bps = maxv > 255 ? 2 : 1;
	size_t n = (size_t)w * h;
	if (!ascii) {
		p++;                                   /* the single whitespace after maxval */
		if ((size_t)(e - p) < n * ch * bps) { rc = err(r, "pnm: truncated"); goto out; }
	}
	im->px = xmalloc(n * 4);
	for (size_t i = 0; i < n; i++) {
		uint8_t *d = im->px + i * 4;
		for (unsigned c = 0; c < ch; c++) {
			unsigned v;
			if (ascii) {
				if (pnm_num(&p, e, &v)) { free(im->px); im->px = NULL; rc = err(r, "pnm: truncated"); goto out; }
			} else if (bps == 2) { v = (unsigned)p[0] << 8 | p[1]; p += 2; }
			else v = *p++;
			if (v > maxv) v = maxv;
			d[c] = (uint8_t)((v * 255u + maxv / 2) / maxv);
		}
		if (!rgb) d[1] = d[2] = d[0];
		d[3] = 255;
	}
	im->w = im->ow = w;
	im->h = im->oh = h;
	im->fmt = rgb ? "ppm" : "pgm";
out:
	buf_free(&b);
	return rc;
}

#ifdef HAVE_PNG
static int load_png(Res *r, const char *path, Img *im)
{
	png_image pi;
	memset(&pi, 0, sizeof pi);
	pi.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&pi, path)) return err(r, "png: %s", pi.message);
	if (too_big(r, pi.width, pi.height)) { png_image_free(&pi); return T_ERR; }
	pi.format = PNG_FORMAT_RGBA;
	im->px = xmalloc(PNG_IMAGE_SIZE(pi));
	if (!png_image_finish_read(&pi, NULL, im->px, 0, NULL)) {
		free(im->px); im->px = NULL;
		return err(r, "png: %s", pi.message);
	}
	im->w = im->ow = pi.width;
	im->h = im->oh = pi.height;
	im->fmt = "png";
	return T_OK;
}
#endif

#ifdef HAVE_JPEG
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; char msg[JMSG_LENGTH_MAX]; } JErr;
static void jerr_exit(j_common_ptr c)
{
	JErr *e = (JErr *)c->err;
	(*c->err->format_message)(c, e->msg);
	longjmp(e->jb, 1);
}
static void jerr_quiet(j_common_ptr c) { (void)c; }

static int load_jpeg(Res *r, const char *path, Img *im, unsigned want)
{
	FILE *f = fopen(path, "rb");
	if (!f) return errno_err(r, path);
	struct jpeg_decompress_struct ci;
	JErr je;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jerr_exit;
	je.pub.output_message = jerr_quiet;
	uint8_t *volatile px = NULL;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		fclose(f);
		free(px);
		return err(r, "jpeg: %s", je.msg);
	}
	jpeg_create_decompress(&ci);
	jpeg_stdio_src(&ci, f);
	jpeg_read_header(&ci, TRUE);
	if (ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK) {
		jpeg_destroy_decompress(&ci); fclose(f);
		return err(r, "jpeg: CMYK not supported");
	}
	im->ow = ci.image_width;
	im->oh = ci.image_height;
	unsigned big = im->ow > im->oh ? im->ow : im->oh, d = 1;
	if (want) while (d < 8 && big / (d * 2) >= want) d *= 2;
	ci.scale_num = 1;
	ci.scale_denom = d;
	ci.out_color_space = JCS_EXT_RGBA;
	jpeg_start_decompress(&ci);
	if (too_big(r, ci.output_width, ci.output_height)) {
		jpeg_destroy_decompress(&ci); fclose(f);
		return T_ERR;
	}
	size_t stride = (size_t)ci.output_width * 4;
	px = xmalloc(stride * ci.output_height);
	while (ci.output_scanline < ci.output_height) {
		JSAMPROW row = px + stride * ci.output_scanline;
		jpeg_read_scanlines(&ci, &row, 1);
	}
	im->w = ci.output_width;
	im->h = ci.output_height;
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	fclose(f);
	im->px = px;
	im->fmt = "jpeg";
	return T_OK;
}
#endif

static int load(Res *r, const char *path, Img *im, unsigned want)
{
	FILE *f = fopen(path, "rb");
	if (!f) return errno_err(r, path);
	unsigned char h[8] = { 0 };
	size_t n = fread(h, 1, sizeof h, f);
	fclose(f);
	memset(im, 0, sizeof *im);
#ifdef HAVE_PNG
	if (n >= 8 && !memcmp(h, "\x89PNG\r\n\x1a\n", 8)) return load_png(r, path, im);
#endif
#ifdef HAVE_JPEG
	if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return load_jpeg(r, path, im, want);
#endif
	if (n >= 3 && h[0] == 'P' && strchr("2356", h[1]) && h[1] &&
	    (h[2] == ' ' || h[2] == '\n' || h[2] == '\r' || h[2] == '\t' || h[2] == '#'))
		return load_pnm(r, path, im);
	(void)want; (void)n;
	return err(r, "%s: unsupported image format", path);
}

/* ---- encode ---- */

static bool is_gray(const Img *im);

/* P6, or P5 when every pixel is gray. Alpha is dropped. */
static int enc_pnm(Res *r, const Img *im, const char *file)
{
	FILE *f = fopen(file, "wb");
	if (!f) return errno_err(r, file);
	bool g = is_gray(im);
	fprintf(f, "P%c\n%u %u\n255\n", g ? '5' : '6', im->w, im->h);
	size_t n = (size_t)im->w * im->h;
	unsigned char row[3 * 1024];
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		const uint8_t *s = im->px + i * 4;
		if (g) row[k++] = s[0];
		else { row[k++] = s[0]; row[k++] = s[1]; row[k++] = s[2]; }
		if (k > sizeof row - 3) { fwrite(row, 1, k, f); k = 0; }
	}
	fwrite(row, 1, k, f);
	if (ferror(f) | fclose(f)) return errno_err(r, file);
	return T_OK;
}

#ifdef HAVE_PNG
static bool opaque(const Img *im)
{
	size_t n = (size_t)im->w * im->h;
	for (size_t i = 0; i < n; i++) if (im->px[i * 4 + 3] != 255) return false;
	return true;
}
#endif

static bool is_gray(const Img *im)
{
	size_t n = (size_t)im->w * im->h;
	for (size_t i = 0; i < n; i++) {
		const uint8_t *p = im->px + i * 4;
		if (p[0] != p[1] || p[1] != p[2]) return false;
	}
	return true;
}

#ifdef HAVE_PNG
/* Pack RGBA in place to fewer channels: 3 (RGB), 1 (G) or 2 (GA). */
static void pack(Img *im, int ch)
{
	size_t n = (size_t)im->w * im->h;
	uint8_t *s = im->px, *d = im->px;
	for (size_t i = 0; i < n; i++, s += 4) {
		if (ch == 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; }
		else if (ch == 1) { *d++ = s[0]; }
		else { d[0] = s[0]; d[1] = s[3]; d += 2; }
	}
}

/* Encodes with the fewest channels that lose nothing. Destroys im->px layout. */
static int enc_png(Res *r, Img *im, Buf *out, const char *file)
{
	bool op = opaque(im), gr = is_gray(im);
	int ch = gr ? (op ? 1 : 2) : (op ? 3 : 4);
	if (ch != 4) pack(im, ch);
	png_image pi;
	memset(&pi, 0, sizeof pi);
	pi.version = PNG_IMAGE_VERSION;
	pi.width = im->w;
	pi.height = im->h;
	pi.format = ch == 1 ? PNG_FORMAT_GRAY : ch == 2 ? PNG_FORMAT_GA : ch == 3 ? PNG_FORMAT_RGB : PNG_FORMAT_RGBA;
	if (file) {
		if (!png_image_write_to_file(&pi, file, 0, im->px, 0, NULL)) return err(r, "png: %s", pi.message);
		return T_OK;
	}
	png_alloc_size_t sz = 0;
	if (!png_image_write_to_memory(&pi, NULL, &sz, 0, im->px, 0, NULL)) return err(r, "png: %s", pi.message);
	buf_clear(out);
	buf_reserve(out, sz);
	if (!png_image_write_to_memory(&pi, out->p, &sz, 0, im->px, 0, NULL)) return err(r, "png: %s", pi.message);
	out->n = sz;
	return T_OK;
}
#endif

#ifdef HAVE_JPEG
static int enc_jpeg(Res *r, Img *im, int q, Buf *out)
{
	struct jpeg_compress_struct ci;
	JErr je;
	unsigned char *volatile mem = NULL;
	unsigned long msz = 0;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jerr_exit;
	je.pub.output_message = jerr_quiet;
	if (setjmp(je.jb)) {
		jpeg_destroy_compress(&ci);
		free(mem);
		return err(r, "jpeg: %s", je.msg);
	}
	jpeg_create_compress(&ci);
	unsigned char *m = NULL;
	jpeg_mem_dest(&ci, &m, &msz);
	ci.image_width = im->w;
	ci.image_height = im->h;
	ci.input_components = 4;
	ci.in_color_space = JCS_EXT_RGBA;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, q < 1 || q > 100 ? 85 : q, TRUE);
	jpeg_start_compress(&ci, TRUE);
	size_t stride = (size_t)im->w * 4;
	while (ci.next_scanline < ci.image_height) {
		JSAMPROW row = im->px + stride * ci.next_scanline;
		jpeg_write_scanlines(&ci, &row, 1);
	}
	jpeg_finish_compress(&ci);
	mem = m;
	buf_clear(out);
	buf_put(out, m, msz);
	jpeg_destroy_compress(&ci);
	free(m);
	return T_OK;
}
#endif

/* ---- transforms ---- */

static void crop(Img *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	for (uint32_t row = 0; row < h; row++)
		memmove(im->px + (size_t)row * w * 4, im->px + ((size_t)(y + row) * im->w + x) * 4, (size_t)w * 4);
	im->w = w;
	im->h = h;
}

static void rotate(Img *im, int deg)
{
	uint32_t w = im->w, h = im->h;
	uint32_t *s = (uint32_t *)(void *)im->px;
	if (deg == 180) {
		size_t n = (size_t)w * h;
		for (size_t i = 0; i < n / 2; i++) { uint32_t t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
		return;
	}
	uint32_t *d = xmalloc((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t px = s[(size_t)y * w + x];
			if (deg == 90) d[(size_t)x * h + (h - 1 - y)] = px;     /* clockwise */
			else d[(size_t)(w - 1 - x) * h + y] = px;
		}
	free(im->px);
	im->px = (uint8_t *)d;
	im->w = h;
	im->h = w;
}

static void flip(Img *im, bool horiz)
{
	uint32_t *p = (uint32_t *)(void *)im->px;
	uint32_t w = im->w, h = im->h;
	if (horiz) {
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w / 2; x++) {
				uint32_t *a = p + (size_t)y * w + x, *b = p + (size_t)y * w + (w - 1 - x), t = *a;
				*a = *b; *b = t;
			}
	} else {
		for (uint32_t y = 0; y < h / 2; y++)
			for (uint32_t x = 0; x < w; x++) {
				uint32_t *a = p + (size_t)y * w + x, *b = p + (size_t)(h - 1 - y) * w + x, t = *a;
				*a = *b; *b = t;
			}
	}
}

/* Area average when shrinking, nearest when growing. Alpha-weighted so
 * transparent pixels don't bleed their colour. */
static void resize(Img *im, uint32_t dw, uint32_t dh)
{
	if (dw == im->w && dh == im->h) return;
	uint8_t *d = xmalloc((size_t)dw * dh * 4), *o = d;
	uint32_t sw = im->w, sh = im->h;
	for (uint32_t y = 0; y < dh; y++) {
		uint32_t y0 = (uint32_t)((uint64_t)y * sh / dh), y1 = (uint32_t)((uint64_t)(y + 1) * sh / dh);
		if (y1 <= y0) y1 = y0 + 1;
		for (uint32_t x = 0; x < dw; x++) {
			uint32_t x0 = (uint32_t)((uint64_t)x * sw / dw), x1 = (uint32_t)((uint64_t)(x + 1) * sw / dw);
			if (x1 <= x0) x1 = x0 + 1;
			uint64_t c[3] = { 0, 0, 0 }, a = 0, cnt = 0;
			for (uint32_t yy = y0; yy < y1; yy++) {
				const uint8_t *s = im->px + ((size_t)yy * sw + x0) * 4;
				for (uint32_t xx = x0; xx < x1; xx++, s += 4) {
					c[0] += (uint64_t)s[0] * s[3]; c[1] += (uint64_t)s[1] * s[3];
					c[2] += (uint64_t)s[2] * s[3]; a += s[3]; cnt++;
				}
			}
			for (int k = 0; k < 3; k++) *o++ = a ? (uint8_t)((c[k] + a / 2) / a) : 0;
			*o++ = (uint8_t)((a + cnt / 2) / cnt);
		}
	}
	free(im->px);
	im->px = d;
	im->w = dw;
	im->h = dh;
}

static void gray(Img *im)
{
	size_t n = (size_t)im->w * im->h;
	for (uint8_t *p = im->px; n--; p += 4) {
		uint8_t g = (uint8_t)((p[0] * 77u + p[1] * 150u + p[2] * 29u + 128u) >> 8);
		p[0] = p[1] = p[2] = g;
	}
}

static void fit(Img *im, unsigned maxd)
{
	if (!maxd || (im->w <= maxd && im->h <= maxd)) return;
	uint32_t w, h;
	if (im->w >= im->h) { w = maxd; h = (uint32_t)(((uint64_t)im->h * maxd + im->w / 2) / im->w); }
	else { h = maxd; w = (uint32_t)(((uint64_t)im->w * maxd + im->h / 2) / im->h); }
	resize(im, w ? w : 1, h ? h : 1);
}

/* Encode for the client: JPEG sources stay JPEG (far smaller), else PNG. */
static int encode_view(Res *r, Img *im)
{
#if defined(HAVE_JPEG) && defined(HAVE_PNG)
	if (strcmp(im->fmt, "jpeg")) { r->mime = "image/png"; return enc_png(r, im, &r->img, NULL); }
#endif
#if defined(HAVE_JPEG)
	r->mime = "image/jpeg";
	return enc_jpeg(r, im, 85, &r->img);
#elif defined(HAVE_PNG)
	r->mime = "image/png";
	return enc_png(r, im, &r->img, NULL);
#else
	(void)im;
	return err(r, "built without png/jpeg: can only write .ppm/.pgm (use out)");
#endif
}

int img_view(Res *r, const char *path, unsigned maxd)
{
	Img im;
	if (load(r, path, &im, maxd)) return T_ERR;
	struct stat st;
	bool small = (!strcmp(im.fmt, "png") || !strcmp(im.fmt, "jpeg")) &&
	             im.w == im.ow && im.h == im.oh && im.w <= maxd && im.h <= maxd &&
	             !stat(path, &st) && st.st_size <= (off_t)CFG_IMG_PASS;
	buf_clear(&r->out);
	if (small) {                         /* send the file as is */
		FILE *f = fopen(path, "rb");
		if (!f) { free(im.px); return errno_err(r, path); }
		buf_clear(&r->img);
		buf_reserve(&r->img, (size_t)st.st_size);
		r->img.n = fread(r->img.p, 1, (size_t)st.st_size, f);
		fclose(f);
		r->mime = !strcmp(im.fmt, "png") ? "image/png" : "image/jpeg";
		buf_printf(&r->out, "%ux%u", im.w, im.h);
		free(im.px);
		return T_OK;
	}
	fit(&im, maxd);
	int rc = encode_view(r, &im);
	if (rc == T_OK) buf_printf(&r->out, "%ux%u (from %ux%u)", im.w, im.h, im.ow, im.oh);
	free(im.px);
	return rc;
}

int t_img(const Json *j, uint32_t a, Res *r, const Id *id)
{
	(void)id;
	char path[PATH_MAX], out[PATH_MAX];
	if (path_resolve(r, arg_str(j, a, "path", NULL), path, true)) return T_ERR;
	const char *out_in = arg_str(j, a, "out", NULL);
	if (out_in && g_cfg.ro) return err(r, "read-only");
	if (out_in && path_resolve(r, out_in, out, false)) return T_ERR;

	uint32_t crop_t = json_get(j, a, "crop");
	long long w = arg_int(j, a, "w", 0), h = arg_int(j, a, "h", 0), maxd = arg_int(j, a, "max", 0);
	long long rot = arg_int(j, a, "rot", 0);
	const char *fl = arg_str(j, a, "flip", NULL);
	bool gr = arg_bool(j, a, "gray", 0), info = arg_bool(j, a, "info", 0);
	if (rot < 0) rot = 360 - (-rot % 360);
	rot %= 360;
	if (rot % 90) return err(r, "rot must be a multiple of 90");
	if (w < 0 || h < 0 || maxd < 0) return err(r, "bad size");

	/* With no crop or exact size, a JPEG may be decoded already scaled. */
	unsigned want = 0;
	if (!crop_t && !w && !h) want = (unsigned)(maxd ? maxd : out_in ? 0 : CFG_IMG_DIM);
	Img im;
	if (load(r, path, &im, want)) return T_ERR;
	buf_clear(&r->out);
	if (info) {
		buf_printf(&r->out, "%s %ux%u", im.fmt, im.ow, im.oh);
		free(im.px);
		return T_OK;
	}
	int rc = T_OK;
	if (crop_t) {
		double v[4];
		uint32_t i = crop_t + 1, k = 0;
		if (j->type[crop_t] != J_ARR || j->len[crop_t] != 4) { rc = err(r, "crop is [x,y,w,h]"); goto done; }
		for (; k < 4 && json_num(j, i, &v[k]); k++, i = j->next[i]) {}
		if (k != 4) { rc = err(r, "crop is [x,y,w,h]"); goto done; }
		/* crop is in source pixels; adjust if JPEG was pre-scaled (never here) */
		if (v[0] < 0 || v[1] < 0 || v[2] < 1 || v[3] < 1 || v[0] + v[2] > im.w || v[1] + v[3] > im.h) {
			rc = err(r, "crop outside %ux%u", im.w, im.h);
			goto done;
		}
		crop(&im, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3]);
	}
	if (rot) rotate(&im, (int)rot);
	if (fl) flip(&im, fl[0] == 'h' || fl[0] == 'H');
	if (w || h) {
		if (!w) w = (long long)((uint64_t)im.w * (uint64_t)h / im.h);
		if (!h) h = (long long)((uint64_t)im.h * (uint64_t)w / im.w);
		if (w < 1) w = 1;
		if (h < 1) h = 1;
		if ((uint64_t)w * (uint64_t)h > CFG_IMG_PIXELS) { rc = err(r, "target too large"); goto done; }
		resize(&im, (uint32_t)w, (uint32_t)h);
	}
	if (maxd) fit(&im, (unsigned)maxd);
	if (gr) gray(&im);
	uint32_t fw = im.w, fh = im.h;

	if (out_in) {
		const char *dot = strrchr(out, '.');
		bool jpg = dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
		bool pnm = dot && (!strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".pgm") || !strcasecmp(dot, ".pnm"));
		if (pnm) {
			rc = enc_pnm(r, &im, out);
		} else if (jpg) {
#ifdef HAVE_JPEG
			Buf b = { 0 };
			rc = enc_jpeg(r, &im, (int)arg_int(j, a, "q", 85), &b);
			if (rc == T_OK) {
				FILE *f = fopen(out, "wb");
				if (!f || fwrite(b.p, 1, b.n, f) != b.n) rc = errno_err(r, out);
				if (f && fclose(f) && rc == T_OK) rc = errno_err(r, out);
			}
			buf_free(&b);
#else
			rc = err(r, "built without jpeg");
#endif
		} else {
#ifdef HAVE_PNG
			rc = enc_png(r, &im, NULL, out);
#else
			rc = err(r, "built without png");
#endif
		}
		if (rc == T_OK) buf_printf(&r->out, "%ux%u", fw, fh);
	} else {
		if (!maxd) fit(&im, CFG_IMG_DIM), fw = im.w, fh = im.h;
		rc = encode_view(r, &im);
		if (rc == T_OK) buf_printf(&r->out, "%ux%u", fw, fh);
	}
done:
	free(im.px);
	return rc;
}
