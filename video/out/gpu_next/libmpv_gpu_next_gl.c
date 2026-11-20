/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * OpenGL API context for the libmpv gpu-next render backend.
 *
 * This is the GL implementation that PR #16818 kept inside
 * video/out/gpu_next/context.c (a file otherwise dedicated to the vo=gpu-next
 * gpu_ctx). It is split out here so that the embedded/render-API path does
 * not have to pull in (or modify) vo_gpu_next.c at all.
 */

#include "config.h"
#include <libplacebo/config.h>

#if HAVE_GL && defined(PL_HAVE_OPENGL)

#include <stdbool.h>
#include <stddef.h>

#include <libplacebo/log.h>
#include <libplacebo/opengl.h>

#include "common/msg.h"
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"
#include "ta/ta_talloc.h"
#include "video/out/libmpv.h"

#include "libmpv_gpu_next.h"
#include "ra.h"

struct priv {
    pl_log pl_log;
    pl_opengl gl;
    pl_gpu gpu;
    struct ra_next *ra;

    /* Persistent copy: params only need to stay valid during the create call. */
    mpv_opengl_init_params gl_params;
};

static bool pl_callback_makecurrent_gl(void *priv)
{
    mpv_opengl_init_params *gl_params = priv;
    /* The mpv render API contract says the client makes the context current
     * inside get_proc_address; trigger it with a harmless name. */
    if (gl_params && gl_params->get_proc_address) {
        gl_params->get_proc_address(gl_params->get_proc_address_ctx, "glGetString");
        return true;
    }
    return false;
}

static void pl_callback_releasecurrent_gl(void *priv)
{
}

static void pl_log_cb(void *log_priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = log_priv;
    mp_msg(log, MSGL_WARN, "[gpu-next:pl] %s\n", msg);
}

static int libmpv_gpu_next_init_gl(struct libmpv_gpu_next_context *ctx,
                                   mpv_render_param *params)
{
    struct priv *p = ctx->priv = talloc_zero(NULL, struct priv);

    mpv_opengl_init_params *gl_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!gl_params || !gl_params->get_proc_address)
        return MPV_ERROR_INVALID_PARAMETER;

    p->gl_params = *gl_params;

    struct pl_log_params log_params = { .log_level = PL_LOG_DEBUG };
    if (mp_msg_test(ctx->log, MSGL_TRACE)) {
        log_params.log_cb = pl_log_cb;
        log_params.log_priv = ctx->log;
    }

    p->pl_log = pl_log_create(PL_API_VER, &log_params);
    if (!p->pl_log)
        return MPV_ERROR_UNSUPPORTED;

    p->gl = pl_opengl_create(p->pl_log, pl_opengl_params(
        .get_proc_addr_ex =
            (pl_voidfunc_t (*)(void*, const char*))gl_params->get_proc_address,
        .proc_ctx = gl_params->get_proc_address_ctx,
        .make_current = pl_callback_makecurrent_gl,
        .release_current = pl_callback_releasecurrent_gl,
        .priv = &p->gl_params,
    ));
    if (!p->gl) {
        MP_ERR(ctx, "Failed to create libplacebo OpenGL context.\n");
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_UNSUPPORTED;
    }
    p->gpu = p->gl->gpu;

    p->ra = ra_pl_create(p->gpu, ctx->log, p->pl_log);
    if (!p->ra) {
        pl_opengl_destroy(&p->gl);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    ctx->ra = p->ra;
    ctx->gpu = p->gpu;
    return 0;
}

static int libmpv_gpu_next_wrap_fbo_gl(struct libmpv_gpu_next_context *ctx,
                                       mpv_render_param *params, pl_tex *out_tex)
{
    struct priv *p = ctx->priv;
    *out_tex = NULL;

    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    pl_tex tex = pl_opengl_wrap(p->gpu, pl_opengl_wrap_params(
        .framebuffer = fbo->fbo,
        .width = fbo->w,
        .height = fbo->h,
        .iformat = fbo->internal_format,
    ));
    if (!tex) {
        MP_ERR(ctx, "Failed to wrap provided FBO as a libplacebo texture.\n");
        return MPV_ERROR_GENERIC;
    }

    *out_tex = tex;
    return 0;
}

static void libmpv_gpu_next_done_frame_gl(struct libmpv_gpu_next_context *ctx)
{
    /* Nothing to do: the host owns the FBO and the swap/present step. */
}

static void libmpv_gpu_next_destroy_gl(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->ra)
        ra_pl_destroy(&p->ra);
    pl_opengl_destroy(&p->gl);
    pl_log_destroy(&p->pl_log);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl = {
    .api_name = MPV_RENDER_API_TYPE_OPENGL,
    .init = libmpv_gpu_next_init_gl,
    .wrap_fbo = libmpv_gpu_next_wrap_fbo_gl,
    .done_frame = libmpv_gpu_next_done_frame_gl,
    .destroy = libmpv_gpu_next_destroy_gl,
};

#endif /* HAVE_GL && PL_HAVE_OPENGL */
