/*
 * video output driver for gfxprim
 *
 * by Cyril Hrubis <metan@ucw.cz>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#include <gfx/gp_rect.h>
#include <gfx/gp_vline.h>
#include <core/gp_fill.h>
#include <core/gp_blit.h>
#include <core/gp_convert.h>
#include <filters/gp_dither.gen.h>
#include <backends/gp_backend.h>
#include <backends/gp_backend_init.h>
#include <text/gp_text.h>

#include "config.h"
#include "vo.h"
#include "video/mp_image.h"

#include "sub/osd_state.h"
#include "sub/dec_sub.h"

#include "osdep/io.h"
#include "osdep/timer.h"
#include "video/sws_utils.h"
#include "input/keycodes.h"
#include "input/input.h"
#include "common/msg.h"
#include "input/input.h"
#include "options/m_config.h"
#include "options/m_option.h"

#include "config.h"
#if !HAVE_GPL
#error GPL only
#endif

struct vo_gfxprim_opts {
    char *backend;
    char *osd_font;
    char *sub_font;
    int sub_font_mul;
    int osd_type;
};

enum osd_type {
    OSD_TYPE_AUTO,
    OSD_TYPE_GFXPRIM,
    OSD_TYPE_MPV,
};

#define OPT_BASE_STRUCT struct vo_gfxprim_opts

static int compiled_in_fonts_help(struct mp_log *log, const struct m_option *opt, struct bstr name)
{
    gp_fonts_iter i;
    const gp_font_family *f;

    mp_info(log, "Available font families:\n");
    GP_FONT_FAMILY_FOREACH(&i, f)
        mp_info(log, " - %s\n", f->family_name);
    return M_OPT_EXIT;
}

static int backend_help(struct mp_log *log, const struct m_option *opt, struct bstr name)
{
    mp_info(log, "backend help\n");

    return M_OPT_EXIT;
}

static const struct m_sub_options vo_gfxprim_conf = {
    .opts = (const struct m_option[]) {
        {"gfxprim-backend", OPT_STRING(backend), .help = backend_help},
        {"gfxprim-osd-font", OPT_STRING(osd_font), .help = compiled_in_fonts_help},
        {"gfxprim-sub-font", OPT_STRING(sub_font), .help = compiled_in_fonts_help},
        {"gfxprim-sub-font-mul", OPT_INT(sub_font_mul), M_RANGE(1, 99)},
        {"gfxprim-osd", OPT_CHOICE(osd_type,
         {"auto", OSD_TYPE_AUTO},
         {"gfxprim", OSD_TYPE_GFXPRIM},
         {"mpv", OSD_TYPE_MPV})
        },
        {0},
    },
    .size = sizeof(OPT_BASE_STRUCT),
};

struct priv {
    gp_backend *backend;

    gp_pixel_type mpv_pixel_type;
    int mpv_pixel_format;

    enum osd_type osd_type;

    gp_pixel white;
    gp_pixel black;

    /* currently played frame size before rescaling */
    int frame_w;
    int frame_h;

    gp_size w, h;
    gp_size x_off, y_off;

    gp_text_style sub_font;
    gp_text_style osd_font;
    gp_text_style osd_bfont;

    /* Pipe to wake up backend_wait() */
    int wakeup_pipe[2];
    gp_fd wakeup_fd;

    struct mp_image *resized_img;
    struct mp_osd_res osd;
    struct mp_sws_context *sws;
};

static void resize_buffers(struct vo *vo, gp_size screen_w, gp_size screen_h)
{
    struct priv *priv = vo->priv;

    if (priv->resized_img)
        mp_image_unrefp(&priv->resized_img);

    float rat = GP_MIN(1.00 * screen_w/priv->frame_w, 1.00 * screen_h/priv->frame_h);

    gp_size new_w = GP_MAX(1, priv->frame_w * rat);
    gp_size new_h = GP_MAX(1, priv->frame_h * rat);

    priv->resized_img = mp_image_alloc(priv->mpv_pixel_format, new_w, new_h);

    if (!priv->resized_img) {
    //VO_ERROR(vo, "Failed to allocate image\n");
        exit(1);
    }

    priv->osd = osd_res_from_image_params(&priv->resized_img->params);
    priv->osd.display_par = 1;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;

    MP_INFO(vo, "Reconfiguring %ix%i\n", params->w, params->h);

    priv->frame_w = params->w;
    priv->frame_h = params->h;

    gp_backend_resize(priv->backend, params->w, params->h);

    resize_buffers(vo, gp_backend_w(priv->backend), gp_backend_h(priv->backend));

    return 0;
}

struct text {
    const char *ass;
    const char *str;
    size_t len;

    int bold:1;
};

static void parse_fmt(struct text *text)
{
    int state = 0;

    while (text->ass) {
        switch (*(text->ass++)) {
        case '}':
            return;
        case '\\':
          state = 1;
        break;
        case 'b':
            if (state == 1)
                state = 2;
            else
                state = 0;
        break;
        case '0':
            if (state == 2)
                text->bold = 0;
            state = 0;
        break;
        case '1':
            if (state == 2)
                text->bold = 1;
            state = 0;
        break;
        default:
            state = 0;
        break;
        }
    }
}

static int next_text(struct text *text)
{
    text->str = NULL;

    for (;;) {
        switch (*(uint8_t*)text->ass) {
        case 0xfe:
        case 0xfd:
            if (text->str)
                goto ret;
            text->ass++;
        break;
        case 0:
            if (text->str)
                goto ret;
            return 0;
        case '{':
            if (text->str)
                goto ret;
            parse_fmt(text);
        break;
        case '\\':
            if (text->str)
                goto ret;

            text->str = text->ass++;
            if (!*text->ass)
                goto ret;
            text->ass++;
            goto ret;
        default:
            if (!text->str)
                text->str = text->ass;
            text->ass++;
        }
    }
ret:
    text->len = text->ass - text->str;
    return 1;
}

static void render_osd_ass(struct priv *priv, gp_pixmap *out, const char *ass)
{
    struct text text = {.ass = ass};
    gp_size text_h = gp_text_height(&priv->osd_font);
    gp_size text_w = gp_text_avg_width(&priv->osd_font, 1);
    gp_coord x = text_h, y = text_h;

    while (next_text(&text)) {
        if (text.str[0] == '\\') {
            switch (text.str[1]) {
            case 'N':
                y += text_h;
                x = text_h;
            break;
            case 'h':
                x += text_w;
            break;
            }
            continue;
        }
        gp_text_style *font = text.bold ? &priv->osd_bfont : &priv->osd_font;

        gp_text_ext(out, font, x+1, y+1, GP_ALIGN_RIGHT | GP_VALIGN_BELOW, priv->black, priv->white, text.str, text.len);
        x += gp_text_ext(out, font, x, y, GP_ALIGN_RIGHT | GP_VALIGN_BELOW, priv->white, priv->black, text.str, text.len);
    }
}

static void render_osd_text(struct priv *priv, gp_pixmap *out, char *osd_text)
{
    if (!osd_text)
        return;

    switch ((uint8_t)osd_text[0]) {
    /* ass0 and ass1 format escapes */
    case 0xfd:
    case 0xfe:
        render_osd_ass(priv, out, osd_text);
        return;
    /* Custom symbol escape */
    case 0xff:
        osd_text++;
        switch (osd_text[0]) {
        case OSD_PLAY:
            osd_text[0] = '>';
        break;
        case OSD_PAUSE:
            osd_text[0] = '"';
        break;
        default:
            fprintf(stderr, "Replacing %i\n", osd_text[0]);
            osd_text[0] = ' ';
        break;
        }
    }

    gp_size text_h = gp_text_height(&priv->osd_font);

    gp_text(out, &priv->osd_font, text_h+1, text_h+1,
            GP_ALIGN_RIGHT | GP_VALIGN_BELOW,
            priv->black, priv->white, osd_text);
    gp_text(out, &priv->osd_font, text_h, text_h,
            GP_ALIGN_RIGHT | GP_VALIGN_BELOW,
            priv->white, priv->black, osd_text);
}

static void render_sub_text(struct priv *priv, gp_pixmap *out, const char *sub_text)
{
    gp_size text_h = gp_text_height(&priv->sub_font);

    const char *lines[2] = {sub_text, NULL};
    size_t lines_len[2] = {};
    int i = 0, done = 0;

    while (!done) {
        switch (sub_text[++lines_len[i]]) {
        case 0:
            done = 1;
        break;
        case '\n':
            if (i) {
                done = 1;
            } else {
                sub_text = sub_text+lines_len[i]+1;
                lines[i+1] = sub_text;
                i++;
            }
        break;
        }
   }

    gp_coord x = gp_pixmap_w(out)/2;
    gp_coord y = gp_pixmap_h(out) - (lines[1] ? 2 * text_h : text_h);

    for (i = 0; i < 2; i++) {
        if (!lines[i])
            break;
        gp_text_ext(out, &priv->sub_font, x+1, y+1,
                    GP_ALIGN_CENTER | GP_VALIGN_ABOVE, priv->black, priv->white, lines[i], lines_len[i]);
        gp_text_ext(out, &priv->sub_font, x, y,
                    GP_ALIGN_CENTER | GP_VALIGN_ABOVE, priv->white, priv->black, lines[i], lines_len[i]);
        y+=text_h;
    }
}

static void render_progbar(struct priv *priv, gp_pixmap *out,
                           const struct osd_progbar_state *progbar_state)
{
    if (progbar_state->type < 0)
        return;

    gp_size text_h = gp_text_height(&priv->osd_font);
    gp_size sub_h = gp_text_height(&priv->sub_font);

    gp_coord x = text_h;
    gp_coord y = gp_pixmap_h(out) - 4 * sub_h;
    gp_coord w = gp_pixmap_w(out) - 2 * text_h;
    gp_coord h = text_h;

    gp_rect_xywh(out, x-2, y-2, w+4, h+4, priv->white);
    gp_rect_xywh(out, x-1, y-1, w+2, h+2, priv->black);
    gp_rect_xywh(out, x, y, w, h, priv->white);
    gp_fill_rect_xywh(out, x, y, w * progbar_state->value, h, priv->white);
    gp_vline_xyh(out, x + w * progbar_state->value, y, h, priv->black);

    int i;

    for (i = 0; i < progbar_state->num_stops; i++) {
        gp_coord stop_x = x + w * progbar_state->stops[i];
        gp_vline_xyh(out, stop_x-1, y, h, priv->white);
        gp_vline_xyh(out, stop_x, y, h, priv->black);
        gp_vline_xyh(out, stop_x+1, y, h, priv->white);
    }
}

static void osd_draw_gfxprim(struct vo *vo, struct vo_frame *frame, gp_pixmap *out)
{
    struct priv *priv = vo->priv;

    render_osd_text(priv, out, vo->osd->objs[OSDTYPE_OSD]->text);
    render_progbar(priv, out, &vo->osd->objs[OSDTYPE_OSD]->progbar_state);

    if (vo->osd->objs[OSDTYPE_SUB]->sub) {
            const char *sub = sub_get_text(vo->osd->objs[OSDTYPE_SUB]->sub, frame->current->pts, SD_TEXT_TYPE_PLAIN);
            if (sub && sub[0])
                render_sub_text(priv, out, sub);
    }
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *priv = vo->priv;
    struct mp_image *cur_frame = frame->current;
    gp_pixmap *dither;
    gp_pixmap mpv_frame;

    if (!cur_frame)
        return VO_TRUE;

    mp_sws_scale(priv->sws, priv->resized_img, cur_frame);

    gp_pixmap_init_ex(&mpv_frame, priv->resized_img->w, priv->resized_img->h,
                      priv->mpv_pixel_type, priv->resized_img->stride[0],
                      priv->resized_img->planes[0], 0);

    gp_pixmap *out = priv->backend->pixmap;

    priv->w = GP_MIN(mpv_frame.w, gp_pixmap_w(out));
    priv->h = GP_MIN(mpv_frame.h, gp_pixmap_h(out));

    priv->x_off = (gp_pixmap_w(out) - priv->w)/2;
    priv->y_off = (gp_pixmap_h(out) - priv->h)/2;


    gp_fill_rect_xywh(out, 0, 0, gp_backend_w(priv->backend), priv->y_off, priv->black);
    gp_fill_rect_xywh(out, 0, 0, priv->x_off, gp_backend_h(priv->backend), priv->black);
    gp_fill_rect_xywh(out, 0, priv->y_off + priv->h,
                      gp_backend_w(priv->backend),
                      gp_backend_h(priv->backend) - priv->y_off - priv->h, priv->black);
    gp_fill_rect_xywh(out, priv->x_off + priv->w, 0,
                      gp_backend_w(priv->backend) - priv->x_off - priv->w,
                      gp_backend_h(priv->backend), priv->black);

    if (priv->osd_type == OSD_TYPE_MPV)
        osd_draw_on_image(vo->osd, priv->osd, frame->current->pts, 0, priv->resized_img);

    switch (out->pixel_type) {
    case GP_PIXEL_G1_UB:
    case GP_PIXEL_G1_DB:
    case GP_PIXEL_G2_UB:
    case GP_PIXEL_G2_DB:
    case GP_PIXEL_G4_UB:
    case GP_PIXEL_G4_DB:
        dither = gp_filter_sierra_alloc(&mpv_frame, out->pixel_type, NULL);
        gp_blit_xywh(dither, 0, 0, priv->w, priv->h, out, priv->x_off, priv->y_off);
        gp_pixmap_free(dither);
    break;
    default:
        gp_blit_xywh(&mpv_frame, 0, 0, priv->w, priv->h, out, priv->x_off, priv->y_off);
    break;
    }

    if (priv->osd_type == OSD_TYPE_GFXPRIM)
        osd_draw_gfxprim(vo, frame, out);

    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *priv = vo->priv;

    gp_backend_flip(priv->backend);
}

static const struct mp_keymap keysym_map[] = {
    {GP_KEY_PAUSE, MP_KEY_PAUSE},
    {GP_KEY_ESC, MP_KEY_ESC},
    {GP_KEY_BACKSPACE, MP_KEY_BS},
    {GP_KEY_TAB, MP_KEY_TAB},
    {GP_KEY_ENTER, MP_KEY_ENTER},
    {GP_KEY_MENU, MP_KEY_MENU},
    {GP_KEY_PRINT, MP_KEY_PRINT},
    {GP_KEY_CANCEL, MP_KEY_CANCEL},
    {GP_KEY_TAB, MP_KEY_TAB},

    {GP_KEY_LEFT, MP_KEY_LEFT},
    {GP_KEY_RIGHT, MP_KEY_RIGHT},
    {GP_KEY_UP, MP_KEY_UP},
    {GP_KEY_DOWN, MP_KEY_DOWN},

    {GP_KEY_INSERT, MP_KEY_INSERT},
    {GP_KEY_DELETE, MP_KEY_DELETE},
    {GP_KEY_HOME, MP_KEY_HOME},
    {GP_KEY_END, MP_KEY_END},
    {GP_KEY_PAGE_UP, MP_KEY_PAGE_UP},
    {GP_KEY_PAGE_DOWN, MP_KEY_PAGE_DOWN},

    {GP_KEY_F1, MP_KEY_F+1},
    {GP_KEY_F2, MP_KEY_F+2},
    {GP_KEY_F3, MP_KEY_F+3},
    {GP_KEY_F4, MP_KEY_F+4},
    {GP_KEY_F5, MP_KEY_F+5},
    {GP_KEY_F6, MP_KEY_F+6},
    {GP_KEY_F7, MP_KEY_F+7},
    {GP_KEY_F8, MP_KEY_F+8},
    {GP_KEY_F9, MP_KEY_F+9},
    {GP_KEY_F10, MP_KEY_F+10},
    {GP_KEY_F11, MP_KEY_F+11},
    {GP_KEY_F12, MP_KEY_F+12},

    {GP_KEY_F13, MP_KEY_F+13},
    {GP_KEY_F14, MP_KEY_F+14},
    {GP_KEY_F15, MP_KEY_F+15},
    {GP_KEY_F16, MP_KEY_F+16},
    {GP_KEY_F17, MP_KEY_F+17},
    {GP_KEY_F18, MP_KEY_F+18},
    {GP_KEY_F19, MP_KEY_F+19},
    {GP_KEY_F20, MP_KEY_F+20},
    {GP_KEY_F21, MP_KEY_F+21},
    {GP_KEY_F22, MP_KEY_F+22},
    {GP_KEY_F23, MP_KEY_F+23},
    {GP_KEY_F24, MP_KEY_F+24},

    /* TODO: different mappings with numlock off? */
#ifdef MP_KEY_KPADD
    {GP_KEY_KP_PLUS, MP_KEY_KPADD},
    {GP_KEY_KP_MINUS, MP_KEY_KPSUBTRACT},
    {GP_KEY_KP_ASTERISK, MP_KEY_KPMULTIPLY},
    {GP_KEY_KP_SLASH, MP_KEY_KPDIVIDE},
#endif
    {GP_KEY_KP_ENTER, MP_KEY_KPENTER},
    {GP_KEY_KP_0, MP_KEY_KP0},
    {GP_KEY_KP_1, MP_KEY_KP1},
    {GP_KEY_KP_2, MP_KEY_KP2},
    {GP_KEY_KP_3, MP_KEY_KP3},
    {GP_KEY_KP_4, MP_KEY_KP4},
    {GP_KEY_KP_5, MP_KEY_KP5},
    {GP_KEY_KP_6, MP_KEY_KP6},
    {GP_KEY_KP_7, MP_KEY_KP7},
    {GP_KEY_KP_8, MP_KEY_KP8},
    {GP_KEY_KP_9, MP_KEY_KP9},
    {GP_KEY_KP_DOT, MP_KEY_KPDEC},

    {}
};

static int lookup_key(gp_event *ev)
{
    return lookup_keymap_table(keysym_map, ev->key.key);
}

static void wait_events(struct vo *vo, int64_t until_time)
{
    struct priv *priv = vo->priv;
    gp_event *ev;
    int mpkey;
#ifdef MP_TIME_MS_TO_NS
    int timeout_ms = (until_time - mp_time_ns())/MP_TIME_MS_TO_NS(1);
#else
    int timeout_ms = (until_time - mp_time_us())/1000;
#endif

    gp_backend_wait_timeout(priv->backend, timeout_ms);

    while ((ev = gp_backend_ev_poll(priv->backend))) {
        //gp_ev_dump(ev);
        switch (ev->type) {
        case GP_EV_SYS:
            switch (ev->code) {
            case GP_EV_SYS_QUIT:
                mp_input_put_key(vo->input_ctx, MP_KEY_CLOSE_WIN);
            break;
            case GP_EV_SYS_RESIZE:
                resize_buffers(vo, ev->sys.w, ev->sys.h);
                gp_backend_resize_ack(priv->backend);
                gp_fill(priv->backend->pixmap, 0);
                gp_backend_flip(priv->backend);
            break;
            }
        break;
        case GP_EV_REL:
            switch (ev->code) {
            case GP_EV_REL_POS:
                mp_input_set_mouse_pos(vo->input_ctx, ev->st->cursor_x, ev->st->cursor_y, false);
            break;
            }
        break;
        case GP_EV_KEY:
            switch (ev->key.key) {
            case GP_BTN_LEFT:
                mp_input_put_key(vo->input_ctx, MP_MBTN_LEFT | (ev->code ? MP_KEY_STATE_DOWN : MP_KEY_STATE_UP));
            break;
            case GP_BTN_RIGHT:
                mp_input_put_key(vo->input_ctx, MP_MBTN_RIGHT | (ev->code ? MP_KEY_STATE_DOWN : MP_KEY_STATE_UP));
            break;
            default:
                if (ev->code == GP_EV_KEY_DOWN) {
                   if (ev->key.utf && !gp_ev_utf_is_ctrl(ev)) {
                       mp_input_put_key(vo->input_ctx, ev->key.utf);
                       return;
                   }
                   mpkey = lookup_key(ev);
                   if (mpkey)
                       mp_input_put_key(vo->input_ctx, mpkey);
                }
            break;
            }
        break;
        case GP_EV_FD:
                mp_flush_wakeup_pipe(priv->wakeup_pipe[0]);
        break;
        }
    }
}

static void uninit(struct vo *vo)
{
    struct priv *priv = vo->priv;

    gp_backend_exit(priv->backend);
}

static void setup_osd_fonts(gp_backend *backend, struct priv *priv,
                            const char *osd_family_name, const char *sub_family_name,
                            int sub_font_mul)
{
    const gp_font_family *font_family = gp_font_family_lookup(osd_family_name);

    priv->osd_font = (gp_text_style) {
        .font = gp_font_family_face_lookup(font_family, GP_FONT_REGULAR | GP_FONT_FALLBACK),
        .pixel_xmul = 1,
        .pixel_ymul = 1,
    };

    priv->osd_bfont = (gp_text_style) {
        .font = gp_font_family_face_lookup(font_family, GP_FONT_REGULAR | GP_FONT_BOLD | GP_FONT_FALLBACK),
        .pixel_xmul = 1,
        .pixel_ymul = 1,
    };

    font_family = gp_font_family_lookup(sub_family_name);
    priv->sub_font = (gp_text_style) {
        .font = gp_font_family_face_lookup(font_family, GP_FONT_REGULAR | GP_FONT_FALLBACK),
        .pixel_xmul = sub_font_mul,
        .pixel_ymul = sub_font_mul,
    };

    priv->white = gp_rgb_to_pixmap_pixel(0xff, 0xff, 0xff, backend->pixmap);
    priv->black = gp_rgb_to_pixmap_pixel(0x00, 0x00, 0x00, backend->pixmap);
}

static int preinit(struct vo *vo)
{
    struct priv *priv = vo->priv;
    struct vo_gfxprim_opts *opts;

    priv->sws = mp_sws_alloc(vo);
    priv->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(priv->sws, vo->global);

    opts = mp_get_config_group(vo, vo->global, &vo_gfxprim_conf);

    priv->backend = gp_backend_init(opts->backend, 0, 0, "mpv");
    if (!priv->backend)
        return -1;

    if (!opts->sub_font_mul)
        opts->sub_font_mul = 1;

    setup_osd_fonts(priv->backend, priv, opts->osd_font, opts->sub_font, opts->sub_font_mul);

    priv->mpv_pixel_type = GP_PIXEL_UNKNOWN;
    priv->osd_type = OSD_TYPE_MPV;

    switch (gp_backend_pixel_type(priv->backend)) {
    case GP_PIXEL_G1_UB:
    case GP_PIXEL_G1_DB:
    case GP_PIXEL_G2_UB:
    case GP_PIXEL_G2_DB:
    case GP_PIXEL_G4_UB:
    case GP_PIXEL_G4_DB:
    case GP_PIXEL_G8:
        priv->mpv_pixel_format = IMGFMT_Y8;
        priv->mpv_pixel_type = GP_PIXEL_G8;
        priv->osd_type = OSD_TYPE_GFXPRIM;
    break;
#ifdef GP_PIXEL_G16
    case GP_PIXEL_G16:
        priv->mpv_pixel_type = GP_PIXEL_G16;
        priv->mpv_pixel_format = IMGFMT_Y16;
    break;
#endif
    case GP_PIXEL_xRGB8888:
        priv->mpv_pixel_type = GP_PIXEL_xRGB8888;
        priv->mpv_pixel_format = IMGFMT_BGR0;
    break;
    case GP_PIXEL_RGB565_LE:
    case GP_PIXEL_RGB565_BE:
        priv->mpv_pixel_type = GP_PIXEL_RGB565_LE;
        priv->mpv_pixel_format = IMGFMT_RGB565;
    break;
    default:
        priv->mpv_pixel_type = GP_PIXEL_RGB888;
        priv->mpv_pixel_format = IMGFMT_BGR24;
    }

    if (opts->osd_type != OSD_TYPE_AUTO)
            priv->osd_type = opts->osd_type;

    MP_INFO(vo, "mpv format %s mapped to GFXprim pixel type %s\n",
            mp_imgfmt_to_name(priv->mpv_pixel_format),
            gp_pixel_type_name(priv->mpv_pixel_type));

    mp_make_wakeup_pipe(priv->wakeup_pipe);

    priv->wakeup_fd = (gp_fd) {
        .fd = priv->wakeup_pipe[0],
        .events = GP_POLLIN,
    };

    gp_backend_poll_add(priv->backend, &priv->wakeup_fd);

    return priv->mpv_pixel_type == GP_PIXEL_UNKNOWN;
}

static int query_format(struct vo *vo, int format)
{
    struct priv *priv = vo->priv;

    return mp_sws_supports_formats(priv->sws, priv->mpv_pixel_format, format) ? 1 : 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *priv = vo->priv;

    switch (request) {
    case VOCTRL_SET_CURSOR_VISIBILITY:
        gp_backend_cursor_set(priv->backend, (*(bool *)data) ? GP_BACKEND_CURSOR_SHOW : GP_BACKEND_CURSOR_HIDE);
        return VO_TRUE;
    case VOCTRL_UPDATE_WINDOW_TITLE:
        gp_backend_set_caption(priv->backend, (char*)data);
        return VO_TRUE;
    case VOCTRL_CHECK_EVENTS:
    break;
    case VOCTRL_VO_OPTS_CHANGED:
        printf("VO opts changed\n");
    break;
    case VOCTRL_UPDATE_PLAYBACK_STATE:
    break;
    default:
        printf("Unimplemented VO request %i\n", request);
    }
    return VO_NOTIMPL;
}

static void wakeup(struct vo *vo)
{
    struct priv *priv = vo->priv;

    write(priv->wakeup_pipe[1], &(char){0}, 1);
}

const struct vo_driver video_out_gfxprim = {
    .description = "Video output for libgfxprim",
    .name = "gfxprim",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .wait_events = wait_events,
    .uninit = uninit,
    .wakeup = wakeup,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_gfxprim_conf,
};
