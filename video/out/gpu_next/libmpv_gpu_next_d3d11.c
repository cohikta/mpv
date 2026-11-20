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
 * D3D11 API context for the libmpv gpu-next render backend.
 *
 * Embedded hosts use MPV_RENDER_API_TYPE_D3D11 and pass, on every frame, an
 * ID3D11Texture2D via MPV_RENDER_PARAM_D3D11_FBO. We build a libplacebo
 * pl_gpu on the host's ID3D11Device (pl_d3d11_create) and wrap that texture
 * into a temporary pl_tex (pl_d3d11_wrap). The libplacebo renderer then draws
 * into it; no swapchain is created and no Present() is issued by mpv.
 *
 * Lifetime / ownership rules (see include/mpv/render_d3d11.h):
 *   - mpv adds a reference to the host ID3D11Device; the caller may release its
 *     own reference after mpv_render_context_create() returns.
 *   - The FBO texture is owned by the host. mpv keeps no reference to it.
 *   - All rendering is submitted to the immediate context before
 *     mpv_render_context_render() returns, so the host's subsequent Flush() /
 *     CopyResource() needs no extra synchronization on this device.
 *   - mpv does not restore D3D11 state; the host must set its own state again.
 */

#include "config.h"
#include <libplacebo/config.h>

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)

#include <stdbool.h>
#include <stddef.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/log.h>

#include "common/msg.h"
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_d3d11.h"
#include "ta/ta_talloc.h"
#include "video/out/libmpv.h"

#include "libmpv_gpu_next.h"
#include "ra.h"

struct priv {
    pl_log pl_log;
    pl_d3d11 d3d11;
    pl_gpu gpu;
    struct ra_next *ra;
};

static void pl_log_cb(void *log_priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = log_priv;
    mp_msg(log, MSGL_WARN, "[gpu-next:pl/d3d11] %s\n", msg);
}

static int libmpv_gpu_next_init_d3d11(struct libmpv_gpu_next_context *ctx,
                                      mpv_render_param *params)
{
    struct priv *p = ctx->priv = talloc_zero(NULL, struct priv);

    mpv_d3d11_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!init_params || !init_params->device) {
        MP_ERR(ctx, "MPV_RENDER_PARAM_D3D11_INIT_PARAMS is missing or has no device.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    struct pl_log_params log_params = { .log_level = PL_LOG_DEBUG };
    if (mp_msg_test(ctx->log, MSGL_TRACE)) {
        log_params.log_cb = pl_log_cb;
        log_params.log_priv = ctx->log;
    }

    p->pl_log = pl_log_create(PL_API_VER, &log_params);
    if (!p->pl_log)
        return MPV_ERROR_UNSUPPORTED;

    /* pl_d3d11_params() supplies PL_D3D11_DEFAULTS; do not zero-init by hand. */
    p->d3d11 = pl_d3d11_create(p->pl_log, pl_d3d11_params(
        .device = (ID3D11Device *)init_params->device,
    ));
    if (!p->d3d11) {
        MP_ERR(ctx, "Failed to create a libplacebo D3D11 context. "
                    "Is libplacebo built with D3D11 support?\n");
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_UNSUPPORTED;
    }
    p->gpu = p->d3d11->gpu;

    p->ra = ra_pl_create(p->gpu, ctx->log, p->pl_log);
    if (!p->ra) {
        MP_ERR(ctx, "Failed to create the libplacebo render abstraction.\n");
        pl_d3d11_destroy(&p->d3d11);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    ctx->ra = p->ra;
    ctx->gpu = p->gpu;
    MP_VERBOSE(ctx, "gpu-next: D3D11 context ready (libplacebo).\n");
    return 0;
}

static int libmpv_gpu_next_wrap_fbo_d3d11(struct libmpv_gpu_next_context *ctx,
                                          mpv_render_param *params,
                                          pl_tex *out_tex)
{
    struct priv *p = ctx->priv;
    *out_tex = NULL;

    mpv_d3d11_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    if (!fbo || !fbo->tex) {
        MP_ERR(ctx, "MPV_RENDER_PARAM_D3D11_FBO is missing or has no texture.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    /* For non-video RGB(A) textures the remaining fields are unnecessary and
     * must stay zero. The pl_tex format is derived from the DXGI_FORMAT.
     * Note: the FBO must have D3D11_BIND_RENDER_TARGET + D3D11_USAGE_DEFAULT,
     * must be non-MSAA, non-array, non-mipmapped and non-sRGB. This is already
     * guaranteed by include/mpv/render_d3d11.h and by the host. */
    struct pl_d3d11_wrap_params wrap = {
        .tex = (ID3D11Resource *)fbo->tex,
    };

    pl_tex tex = pl_d3d11_wrap(p->gpu, &wrap);
    if (!tex) {
        MP_ERR(ctx, "Failed to wrap the D3D11 FBO texture as a pl_tex. "
                    "Check D3D11_BIND_RENDER_TARGET / D3D11_USAGE_DEFAULT / format.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    *out_tex = tex;
    return 0;
}

static void libmpv_gpu_next_done_frame_d3d11(struct libmpv_gpu_next_context *ctx)
{
    /* Deliberately empty.
     *
     * render_d3d11.h guarantees that mpv_render_context_render() submits all
     * work to the immediate context before returning. The host (in this
     * project: D3D11SurfaceManager::Draw) calls ID3D11DeviceContext::Flush()
     * and then performs CopyResource(internal -> shared texture), followed by
     * MarkTextureFrameAvailable(). Creating a swapchain or calling Present()
     * here would break the embedded/Flutter texture path. */
}

static void libmpv_gpu_next_destroy_d3d11(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    /* Order matters: the RA holds a pl_renderer referencing pl_gpu. */
    if (p->ra)
        ra_pl_destroy(&p->ra);
    pl_d3d11_destroy(&p->d3d11);
    pl_log_destroy(&p->pl_log);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_D3D11,
    .init = libmpv_gpu_next_init_d3d11,
    .wrap_fbo = libmpv_gpu_next_wrap_fbo_d3d11,
    .done_frame = libmpv_gpu_next_done_frame_d3d11,
    .destroy = libmpv_gpu_next_destroy_d3d11,
};

#endif /* HAVE_D3D11 && PL_HAVE_D3D11 */
