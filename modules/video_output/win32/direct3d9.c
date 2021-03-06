/*****************************************************************************
 * direct3d9.c: Windows Direct3D9 video output module
 *****************************************************************************
 * Copyright (C) 2006-2014 VLC authors and VideoLAN
 *$Id$
 *
 * Authors: Martell Malone <martellmalone@gmail.com>,
 *          Damien Fouilleul <damienf@videolan.org>,
 *          Sasha Koruga <skoruga@gmail.com>,
 *          Felix Abecassis <felix.abecassis@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble:
 *
 * This plugin will use YUV surface if supported, using YUV will result in
 * the best video quality (hardware filtering when rescaling the picture)
 * and the fastest display as it requires less processing.
 *
 * If YUV overlay is not supported this plugin will use RGB offscreen video
 * surfaces that will be blitted onto the primary surface (display) to
 * effectively display the pictures.
 *
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_charset.h> /* ToT function */

#include <windows.h>
#include <d3d9.h>
#ifdef HAVE_D3DX9EFFECT_H
#include <d3dx9effect.h>
#endif
#include "../../video_chroma/d3d9_fmt.h"

#include "common.h"
#include "builtin_shaders.h"

#include <assert.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);

static int  GLConvOpen(vlc_object_t *);
static void GLConvClose(vlc_object_t *);

#define DESKTOP_LONGTEXT N_(\
    "The desktop mode allows you to display the video on the desktop.")

#define HW_BLENDING_TEXT N_("Use hardware blending support")
#define HW_BLENDING_LONGTEXT N_(\
    "Try to use hardware acceleration for subtitle/OSD blending.")

#define PIXEL_SHADER_TEXT N_("Pixel Shader")
#define PIXEL_SHADER_LONGTEXT N_(\
        "Choose a pixel shader to apply.")
#define PIXEL_SHADER_FILE_TEXT N_("Path to HLSL file")
#define PIXEL_SHADER_FILE_LONGTEXT N_("Path to an HLSL file containing a single pixel shader.")
/* The latest option in the selection list: used for loading a shader file. */
#define SELECTED_SHADER_FILE N_("HLSL File")

#define D3D9_HELP N_("Recommended video output for Windows Vista and later versions")

static int FindShadersCallback(vlc_object_t *, const char *,
                               char ***, char ***);

vlc_module_begin ()
    set_shortname("Direct3D9")
    set_description(N_("Direct3D9 video output"))
    set_help(D3D9_HELP)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)

    add_bool("direct3d9-hw-blending", true, HW_BLENDING_TEXT, HW_BLENDING_LONGTEXT, true)

    add_string("direct3d9-shader", "", PIXEL_SHADER_TEXT, PIXEL_SHADER_LONGTEXT, true)
        change_string_cb(FindShadersCallback)
    add_loadfile("direct3d9-shader-file", NULL, PIXEL_SHADER_FILE_TEXT, PIXEL_SHADER_FILE_LONGTEXT, false)

    set_capability("vout display", 280)
    add_shortcut("direct3d9", "direct3d")
    set_callbacks(Open, Close)

    add_submodule()
    set_description("DX OpenGL surface converter for D3D9")
    set_capability("glconv", 1)
    set_callbacks(GLConvOpen, GLConvClose)
vlc_module_end ()

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/
static const vlc_fourcc_t d3d_subpicture_chromas[] = {
    VLC_CODEC_RGBA,
    0
};

typedef struct
{
    const char   *name;
    D3DFORMAT    format;    /* D3D format */
    vlc_fourcc_t fourcc;    /* VLC fourcc */
    uint32_t     rmask;
    uint32_t     gmask;
    uint32_t     bmask;
} d3d_format_t;

struct d3dctx
{
    HINSTANCE               hdll;       /* handle of the opened d3d9 dll */
    HINSTANCE               hxdll;      /* handle of the opened d3d9x dll */
    union
    {
        LPDIRECT3D9         obj;
        LPDIRECT3D9EX       objex;
    };
    union
    {
        LPDIRECT3DDEVICE9   dev;
        LPDIRECT3DDEVICE9EX devex;
    };
    D3DPRESENT_PARAMETERS   pp;
    D3DCAPS9                caps;
    HWND                    hwnd;
    bool                    use_ex;
};

struct vout_display_sys_t
{
    vout_display_sys_win32_t sys;

    bool allow_hw_yuv;    /* Should we use hardware YUV->RGB conversions */
    struct {
        bool is_fullscreen;
        bool is_on_top;
        RECT win;
    } desktop_save;
    vout_display_cfg_t cfg_saved; /* configuration used before going into desktop mode */

    // core objects
    struct d3dctx           d3dctx;
    IDirect3DPixelShader9*  d3dx_shader;

    // scene objects
    LPDIRECT3DTEXTURE9      d3dtex;
    LPDIRECT3DVERTEXBUFFER9 d3dvtc;
    D3DFORMAT               d3dregion_format;    /* Backbuffer output format */
    int                     d3dregion_count;
    struct d3d_region_t     *d3dregion;
    const d3d_format_t      *d3dtexture_format;  /* Rendering texture(s) format */

    /* */
    bool                    reset_device;
    bool                    reopen_device;
    bool                    lost_not_ready;
    bool                    clear_scene;

    /* It protects the following variables */
    vlc_mutex_t    lock;
    bool           ch_desktop;
    bool           desktop_requested;
};

static const d3d_format_t *Direct3DFindFormat(vout_display_t *vd, vlc_fourcc_t chroma, D3DFORMAT target);

static int  Open(vlc_object_t *);

static picture_pool_t *Direct3D9CreatePicturePool  (vlc_object_t *, struct d3dctx *,
     const d3d_format_t *, const video_format_t *, unsigned);

static void           Prepare(vout_display_t *, picture_t *, subpicture_t *subpicture);
static void           Display(vout_display_t *, picture_t *, subpicture_t *subpicture);
static picture_pool_t*DisplayPool(vout_display_t *, unsigned);
static int            Control(vout_display_t *, int, va_list);
static void           Manage (vout_display_t *);

static int  Direct3D9Create (vlc_object_t *, struct d3dctx *, const video_format_t *);
static int  Direct3D9Reset  (vout_display_t *);
static void Direct3D9Destroy(vlc_object_t *, struct d3dctx *);

static int Direct3D9CreateDevice(vlc_object_t *, struct d3dctx *, const video_format_t *);
static void Direct3D9DestroyDevice(vlc_object_t *, struct d3dctx *);

static int  Direct3D9Open (vout_display_t *, video_format_t *);
static void Direct3D9Close(vout_display_t *);

/* */
typedef struct
{
    FLOAT       x,y,z;      // vertex untransformed position
    FLOAT       rhw;        // eye distance
    D3DCOLOR    diffuse;    // diffuse color
    FLOAT       tu, tv;     // texture relative coordinates
} CUSTOMVERTEX;
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)

typedef struct d3d_region_t {
    D3DFORMAT          format;
    unsigned           width;
    unsigned           height;
    CUSTOMVERTEX       vertex[4];
    LPDIRECT3DTEXTURE9 texture;
} d3d_region_t;

static void Direct3D9DeleteRegions(int, d3d_region_t *);

static int  Direct3D9ImportPicture(vout_display_t *vd, d3d_region_t *, LPDIRECT3DSURFACE9 surface);
static void Direct3D9ImportSubpicture(vout_display_t *vd, int *, d3d_region_t **, subpicture_t *);

static void Direct3D9RenderScene(vout_display_t *vd, d3d_region_t *, int, d3d_region_t *);

/* */
static int DesktopCallback(vlc_object_t *, char const *, vlc_value_t, vlc_value_t, void *);

static bool is_d3d9_opaque(vlc_fourcc_t chroma)
{
    switch (chroma)
    {
    case VLC_CODEC_D3D9_OPAQUE:
    case VLC_CODEC_D3D9_OPAQUE_10B:
        return true;
    default:
        return false;
    }
}

/**
 * It creates a Direct3D vout display.
 */
static int Open(vlc_object_t *object)
{
    vout_display_t *vd = (vout_display_t *)object;
    vout_display_sys_t *sys;

    if ( !vd->obj.force && vd->source.projection_mode != PROJECTION_MODE_RECTANGULAR)
        return VLC_EGENERIC; /* let a module who can handle it do it */

    OSVERSIONINFO winVer;
    winVer.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    if(GetVersionEx(&winVer) && winVer.dwMajorVersion < 6 && !object->obj.force)
        return VLC_EGENERIC;

    /* Allocate structure */
    vd->sys = sys = calloc(1, sizeof(vout_display_sys_t));
    if (!sys)
        return VLC_ENOMEM;

    if (Direct3D9Create(VLC_OBJECT(vd), &sys->d3dctx, &vd->fmt)) {
        msg_Err(vd, "Direct3D9 could not be initialized");
        free(sys);
        return VLC_EGENERIC;
    }

    sys->sys.use_desktop = var_CreateGetBool(vd, "video-wallpaper");
    sys->reset_device = false;
    sys->reopen_device = false;
    sys->lost_not_ready = false;
    sys->allow_hw_yuv = var_CreateGetBool(vd, "directx-hw-yuv");
    sys->desktop_save.is_fullscreen = vd->cfg->is_fullscreen;
    sys->desktop_save.is_on_top     = false;
    sys->desktop_save.win.left      = var_InheritInteger(vd, "video-x");
    sys->desktop_save.win.right     = vd->cfg->display.width;
    sys->desktop_save.win.top       = var_InheritInteger(vd, "video-y");
    sys->desktop_save.win.bottom    = vd->cfg->display.height;

    if (CommonInit(vd))
        goto error;

    /* */
    video_format_t fmt;
    if (Direct3D9Open(vd, &fmt)) {
        msg_Err(vd, "Direct3D9 could not be opened");
        goto error;
    }

    /* */
    vout_display_info_t info = vd->info;
    info.is_slow = !is_d3d9_opaque(fmt.i_chroma);
    info.has_double_click = true;
    info.has_pictures_invalid = !is_d3d9_opaque(fmt.i_chroma);
    if (var_InheritBool(vd, "direct3d9-hw-blending") &&
        sys->d3dregion_format != D3DFMT_UNKNOWN &&
        (sys->d3dctx.caps.SrcBlendCaps  & D3DPBLENDCAPS_SRCALPHA) &&
        (sys->d3dctx.caps.DestBlendCaps & D3DPBLENDCAPS_INVSRCALPHA) &&
        (sys->d3dctx.caps.TextureCaps   & D3DPTEXTURECAPS_ALPHA) &&
        (sys->d3dctx.caps.TextureOpCaps & D3DTEXOPCAPS_SELECTARG1) &&
        (sys->d3dctx.caps.TextureOpCaps & D3DTEXOPCAPS_MODULATE))
        info.subpicture_chromas = d3d_subpicture_chromas;
    else
        info.subpicture_chromas = NULL;

    /* Interaction */
    vlc_mutex_init(&sys->lock);
    sys->ch_desktop = false;
    sys->desktop_requested = sys->sys.use_desktop;

    vlc_value_t val;
    val.psz_string = _("Desktop");
    var_Change(vd, "video-wallpaper", VLC_VAR_SETTEXT, &val, NULL);
    var_AddCallback(vd, "video-wallpaper", DesktopCallback, NULL);

    /* Setup vout_display now that everything is fine */
    video_format_Clean(&vd->fmt);
    video_format_Copy(&vd->fmt, &fmt);
    vd->info = info;

    vd->pool = DisplayPool;
    vd->prepare = Prepare;
    vd->display = Display;
    vd->control = Control;
    vd->manage  = Manage;

    /* Fix state in case of desktop mode */
    if (sys->sys.use_desktop && vd->cfg->is_fullscreen)
        vout_display_SendEventFullscreen(vd, false, false);

    return VLC_SUCCESS;
error:
    Direct3D9Close(vd);
    CommonClean(vd);
    Direct3D9Destroy(VLC_OBJECT(vd), &sys->d3dctx);
    free(vd->sys);
    return VLC_EGENERIC;
}

/**
 * It destroyes a Direct3D vout display.
 */
static void Close(vlc_object_t *object)
{
    vout_display_t * vd = (vout_display_t *)object;

    var_DelCallback(vd, "video-wallpaper", DesktopCallback, NULL);
    vlc_mutex_destroy(&vd->sys->lock);

    Direct3D9Close(vd);

    CommonClean(vd);

    Direct3D9Destroy(VLC_OBJECT(vd), &vd->sys->d3dctx);

    free(vd->sys);
}

static void DestroyPicture(picture_t *picture)
{
    ReleasePictureSys(picture->p_sys);

    free(picture->p_sys);
    free(picture);
}

/**
 * It locks the surface associated to the picture and get the surface
 * descriptor which amongst other things has the pointer to the picture
 * data and its pitch.
 */
static int Direct3D9LockSurface(picture_t *picture)
{
    /* Lock the surface to get a valid pointer to the picture buffer */
    D3DLOCKED_RECT d3drect;
    HRESULT hr = IDirect3DSurface9_LockRect(picture->p_sys->surface, &d3drect, NULL, 0);
    if (FAILED(hr)) {
        return VLC_EGENERIC;
    }

    CommonUpdatePicture(picture, NULL, d3drect.pBits, d3drect.Pitch);
    return VLC_SUCCESS;
}
/**
 * It unlocks the surface associated to the picture.
 */
static void Direct3D9UnlockSurface(picture_t *picture)
{
    /* Unlock the Surface */
    HRESULT hr = IDirect3DSurface9_UnlockRect(picture->p_sys->surface);
    if (FAILED(hr)) {
        //msg_Dbg(vd, "Failed IDirect3DSurface9_UnlockRect: 0x%0lx", hr);
    }
}

/* */
static picture_pool_t *Direct3D9CreatePicturePool(vlc_object_t *o,
    struct d3dctx *d3dctx, const d3d_format_t *default_d3dfmt, const video_format_t *fmt, unsigned count)
{
    picture_pool_t*   pool = NULL;
    picture_t**       pictures = NULL;
    unsigned          picture_count = 0;

    pictures = calloc(count, sizeof(*pictures));
    if (!pictures)
        goto error;

    D3DFORMAT format;
    switch (fmt->i_chroma)
    {
    case VLC_CODEC_D3D9_OPAQUE_10B:
        format = MAKEFOURCC('P','0','1','0');
        break;
    case VLC_CODEC_D3D9_OPAQUE:
        format = MAKEFOURCC('N','V','1','2');
        break;
    default:
        if (!default_d3dfmt)
            goto error;
        format = default_d3dfmt->format;
        break;
    }

    for (picture_count = 0; picture_count < count; ++picture_count)
    {
        picture_sys_t *picsys = malloc(sizeof(*picsys));
        if (unlikely(picsys == NULL))
            goto error;

        HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(d3dctx->dev,
                                                          fmt->i_width,
                                                          fmt->i_height,
                                                          format,
                                                          D3DPOOL_DEFAULT,
                                                          &picsys->surface,
                                                          NULL);
        if (FAILED(hr)) {
           msg_Err(o, "Failed to allocate surface %d (hr=0x%0lx)", picture_count, hr);
           free(picsys);
           goto error;
        }

        picture_resource_t resource = {
            .p_sys = picsys,
            .pf_destroy = DestroyPicture,
        };

        picture_t *picture = picture_NewFromResource(fmt, &resource);
        if (unlikely(picture == NULL)) {
            free(picsys);
            goto error;
        }

        pictures[picture_count] = picture;
    }

    picture_pool_configuration_t pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.picture_count = count;
    pool_cfg.picture       = pictures;
    if( !is_d3d9_opaque( fmt->i_chroma ) )
    {
        pool_cfg.lock = Direct3D9LockSurface;
        pool_cfg.unlock = Direct3D9UnlockSurface;
    }

    pool = picture_pool_NewExtended( &pool_cfg );

error:
    if (pool == NULL && pictures) {
        for (unsigned i=0;i<picture_count; ++i)
            DestroyPicture(pictures[i]);
    }
    free(pictures);
    return pool;
}

static picture_pool_t *DisplayPool(vout_display_t *vd, unsigned count)
{
    if ( vd->sys->sys.pool != NULL )
        return vd->sys->sys.pool;
    vd->sys->sys.pool = Direct3D9CreatePicturePool(VLC_OBJECT(vd), &vd->sys->d3dctx,
        vd->sys->d3dtexture_format, &vd->fmt, count);
    return vd->sys->sys.pool;
}

static void Prepare(vout_display_t *vd, picture_t *picture, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    LPDIRECT3DSURFACE9 surface = picture->p_sys->surface;

    /* FIXME it is a bit ugly, we need the surface to be unlocked for
     * rendering.
     *  The clean way would be to release the picture (and ensure that
     * the vout doesn't keep a reference). But because of the vout
     * wrapper, we can't */
    if ( !is_d3d9_opaque(picture->format.i_chroma) )
        Direct3D9UnlockSurface(picture);
    else if (picture->context)
    {
        const struct va_pic_context *pic_ctx = (struct va_pic_context*)picture->context;
        if (pic_ctx->picsys.surface != picture->p_sys->surface)
        {
            HRESULT hr;
            RECT visibleSource;
            visibleSource.left = 0;
            visibleSource.top = 0;
            visibleSource.right = picture->format.i_visible_width;
            visibleSource.bottom = picture->format.i_visible_height;
            hr = IDirect3DDevice9_StretchRect( sys->d3dctx.dev, pic_ctx->picsys.surface, &visibleSource, surface, &visibleSource, D3DTEXF_NONE);
            if (FAILED(hr)) {
                msg_Err(vd, "Failed to copy the hw surface to the decoder surface (hr=0x%0lx)", hr );
            }
        }
    }

    /* check if device is still available */
    HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(sys->d3dctx.dev);
    if (FAILED(hr)) {
        if (hr == D3DERR_DEVICENOTRESET && !sys->reset_device) {
            vout_display_SendEventPicturesInvalid(vd);
            sys->reset_device = true;
            sys->lost_not_ready = false;
        }
        if (hr == D3DERR_DEVICELOST && !sys->lost_not_ready) {
            /* Device is lost but not yet ready for reset. */
            sys->lost_not_ready = true;
        }
        return;
    }

    d3d_region_t picture_region;
    if (!Direct3D9ImportPicture(vd, &picture_region, surface)) {
        picture_region.width = picture->format.i_visible_width;
        picture_region.height = picture->format.i_visible_height;
        int subpicture_region_count     = 0;
        d3d_region_t *subpicture_region = NULL;
        if (subpicture)
            Direct3D9ImportSubpicture(vd, &subpicture_region_count, &subpicture_region,
                                     subpicture);

        Direct3D9RenderScene(vd, &picture_region,
                            subpicture_region_count, subpicture_region);

        Direct3D9DeleteRegions(sys->d3dregion_count, sys->d3dregion);
        sys->d3dregion_count = subpicture_region_count;
        sys->d3dregion       = subpicture_region;
    }
}

static void Display(vout_display_t *vd, picture_t *picture, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->lost_not_ready) {
        picture_Release(picture);
        if (subpicture)
            subpicture_Delete(subpicture);
        return;
    }

    // Present the back buffer contents to the display
    // No stretching should happen here !
    const RECT src = sys->sys.rect_dest_clipped;
    const RECT dst = sys->sys.rect_dest_clipped;

    HRESULT hr;
    if (sys->d3dctx.use_ex) {
        hr = IDirect3DDevice9Ex_PresentEx(sys->d3dctx.devex, &src, &dst, NULL, NULL, 0);
    } else {
        hr = IDirect3DDevice9_Present(sys->d3dctx.dev, &src, &dst, NULL, NULL);
    }
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_Present: 0x%0lx", hr);
    }

    /* XXX See Prepare() */
    if ( !is_d3d9_opaque(picture->format.i_chroma) )
        Direct3D9LockSurface(picture);
    picture_Release(picture);
    if (subpicture)
        subpicture_Delete(subpicture);

    CommonDisplay(vd);
}

static int ControlReopenDevice(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->sys.use_desktop) {
        /* Save non-desktop state */
        sys->desktop_save.is_fullscreen = vd->cfg->is_fullscreen;
        sys->desktop_save.is_on_top     = sys->sys.is_on_top;

        WINDOWPLACEMENT wp = { .length = sizeof(wp), };
        GetWindowPlacement(sys->sys.hparent ? sys->sys.hparent : sys->sys.hwnd, &wp);
        sys->desktop_save.win = wp.rcNormalPosition;
    }

    /* */
    Direct3D9Close(vd);
    EventThreadStop(sys->sys.event);

    /* */
    vlc_mutex_lock(&sys->lock);
    sys->sys.use_desktop = sys->desktop_requested;
    sys->ch_desktop = false;
    vlc_mutex_unlock(&sys->lock);

    /* */
    event_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.use_desktop = sys->sys.use_desktop;
    if (!sys->sys.use_desktop) {
        cfg.x      = sys->desktop_save.win.left;
        cfg.y      = sys->desktop_save.win.top;
        cfg.width  = sys->desktop_save.win.right  - sys->desktop_save.win.left;
        cfg.height = sys->desktop_save.win.bottom - sys->desktop_save.win.top;
    }

    event_hwnd_t hwnd;
    if (EventThreadStart(sys->sys.event, &hwnd, &cfg)) {
        msg_Err(vd, "Failed to restart event thread");
        return VLC_EGENERIC;
    }
    sys->sys.parent_window = hwnd.parent_window;
    sys->sys.hparent       = hwnd.hparent;
    sys->sys.hwnd          = hwnd.hwnd;
    sys->sys.hvideownd     = hwnd.hvideownd;
    sys->sys.hfswnd        = hwnd.hfswnd;
    SetRectEmpty(&sys->sys.rect_parent);

    /* */
    video_format_t fmt;
    if (Direct3D9Open(vd, &fmt)) {
        CommonClean(vd);
        msg_Err(vd, "Failed to reopen device");
        return VLC_EGENERIC;
    }
    vd->fmt = fmt;
    sys->sys.is_first_display = true;

    if (sys->sys.use_desktop) {
        /* Disable fullscreen/on_top while using desktop */
        if (sys->desktop_save.is_fullscreen)
            vout_display_SendEventFullscreen(vd, false, false);
        if (sys->desktop_save.is_on_top)
            vout_display_SendWindowState(vd, VOUT_WINDOW_STATE_NORMAL);
    } else {
        /* Restore fullscreen/on_top */
        if (sys->desktop_save.is_fullscreen)
            vout_display_SendEventFullscreen(vd, true, false);
        if (sys->desktop_save.is_on_top)
            vout_display_SendWindowState(vd, VOUT_WINDOW_STATE_ABOVE);
    }
    return VLC_SUCCESS;
}
static int Control(vout_display_t *vd, int query, va_list args)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query) {
    case VOUT_DISPLAY_RESET_PICTURES:
        /* FIXME what to do here in case of failure */
        if (sys->reset_device) {
            if (Direct3D9Reset(vd)) {
                msg_Err(vd, "Failed to reset device");
                return VLC_EGENERIC;
            }
            sys->reset_device = false;
        } else if(sys->reopen_device) {
            if (ControlReopenDevice(vd)) {
                msg_Err(vd, "Failed to reopen device");
                return VLC_EGENERIC;
            }
            sys->reopen_device = false;
        }
        return VLC_SUCCESS;
    default:
        return CommonControl(vd, query, args);
    }
}
static void Manage (vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    CommonManage(vd);

    /* Desktop mode change */
    vlc_mutex_lock(&sys->lock);
    const bool ch_desktop = sys->ch_desktop;
    sys->ch_desktop = false;
    vlc_mutex_unlock(&sys->lock);

    if (ch_desktop) {
        sys->reopen_device = true;
        if (vd->info.has_pictures_invalid)
            vout_display_SendEventPicturesInvalid(vd);
    }

    /* Position Change */
    if (sys->sys.changes & DX_POSITION_CHANGE) {
#if 0 /* need that when bicubic filter is available */
        RECT rect;
        UINT width, height;

        GetClientRect(p_sys->sys.hvideownd, &rect);
        width  = rect.right-rect.left;
        height = rect.bottom-rect.top;

        if (width != p_sys->d3dctx.pp.BackBufferWidth || height != p_sys->d3dctx.pp.BackBufferHeight)
        {
            msg_Dbg(vd, "resizing device back buffers to (%lux%lu)", width, height);
            // need to reset D3D device to resize back buffer
            if (VLC_SUCCESS != Direct3D9ResetDevice(vd, width, height))
                return VLC_EGENERIC;
        }
#endif
        sys->clear_scene = true;
        sys->sys.changes &= ~DX_POSITION_CHANGE;
    }
}

static HINSTANCE Direct3D9LoadShaderLibrary(void)
{
    HINSTANCE instance = NULL;
    for (int i = 43; i > 23; --i) {
        TCHAR filename[16];
        _sntprintf(filename, 16, TEXT("D3dx9_%d.dll"), i);
        instance = LoadLibrary(filename);
        if (instance)
            break;
    }
    return instance;
}

/**
 * It initializes an instance of Direct3D9
 */
static int Direct3D9Create(vlc_object_t *o, struct d3dctx *d3dctx, const video_format_t *fmt)
{
    d3dctx->hdll = LoadLibrary(TEXT("D3D9.DLL"));
    if (!d3dctx->hdll) {
        msg_Warn(o, "cannot load d3d9.dll, aborting");
        return VLC_EGENERIC;
    }

    LPDIRECT3D9 (WINAPI *OurDirect3DCreate9)(UINT SDKVersion);
    OurDirect3DCreate9 =
        (void *)GetProcAddress(d3dctx->hdll, "Direct3DCreate9");
    if (!OurDirect3DCreate9) {
        msg_Err(o, "Cannot locate reference to Direct3DCreate9 ABI in DLL");
        goto error;
    }

    HRESULT (WINAPI *OurDirect3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex **ppD3D);
    OurDirect3DCreate9Ex =
        (void *)GetProcAddress(d3dctx->hdll, "Direct3DCreate9Ex");

    /* Create the D3D object. */
    if (OurDirect3DCreate9Ex) {
        HRESULT hr = OurDirect3DCreate9Ex(D3D_SDK_VERSION, &d3dctx->objex);
        if(!FAILED(hr)) {
            msg_Dbg(o, "Using Direct3D9 Extended API!");
            d3dctx->use_ex = true;
        }
    }

    if (!d3dctx->obj)
    {
        d3dctx->obj = OurDirect3DCreate9(D3D_SDK_VERSION);
        if (!d3dctx->obj) {
            msg_Err(o, "Could not create Direct3D9 instance.");
            goto error;
        }
    }

    d3dctx->hxdll = Direct3D9LoadShaderLibrary();
    if (!d3dctx->hxdll)
        msg_Warn(o, "cannot load Direct3D9 Shader Library; HLSL pixel shading will be disabled.");

    /*
    ** Get device capabilities
    */
    ZeroMemory(&d3dctx->caps, sizeof(d3dctx->caps));
    HRESULT hr = IDirect3D9_GetDeviceCaps(d3dctx->obj, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &d3dctx->caps);
    if (FAILED(hr)) {
       msg_Err(o, "Could not read adapter capabilities. (hr=0x%0lx)", hr);
       goto error;
    }

    /* TODO: need to test device capabilities and select the right render function */
    if (!(d3dctx->caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES)) {
        msg_Err(o, "Device does not support stretching from textures.");
        goto error;
    }

    if ( fmt->i_width > d3dctx->caps.MaxTextureWidth ||
         fmt->i_height > d3dctx->caps.MaxTextureHeight )
    {
        msg_Err(o, "Textures too large %ux%u max possible: %ux%u",
                fmt->i_width, fmt->i_height,
                (unsigned) d3dctx->caps.MaxTextureWidth,
                (unsigned) d3dctx->caps.MaxTextureHeight);
        goto error;
    }

    return VLC_SUCCESS;
error:
    Direct3D9Destroy(o, d3dctx);
    return VLC_EGENERIC;
}

/**
 * It releases an instance of Direct3D9
 */
static void Direct3D9Destroy(vlc_object_t *o, struct d3dctx *d3dctx)
{
    VLC_UNUSED(o);

    if (d3dctx->obj)
       IDirect3D9_Release(d3dctx->obj);
    if (d3dctx->hdll)
        FreeLibrary(d3dctx->hdll);
    if (d3dctx->hxdll)
        FreeLibrary(d3dctx->hxdll);

    d3dctx->obj = NULL;
    d3dctx->hdll = NULL;
    d3dctx->hxdll = NULL;
}


/**
 * It setup vout_display_sys_t::d3dpp and vout_display_sys_t::rect_display
 * from the default adapter.
 */
static int Direct3D9FillPresentationParameters(vlc_object_t *o, struct d3dctx *d3dctx, const video_format_t *source)
{
    /*
    ** Get the current desktop display mode, so we can set up a back
    ** buffer of the same format
    */
    D3DDISPLAYMODE d3ddm;
    HRESULT hr = IDirect3D9_GetAdapterDisplayMode(d3dctx->obj,
                                                  D3DADAPTER_DEFAULT, &d3ddm);
    if (FAILED(hr)) {
       msg_Err(o, "Could not read adapter display mode. (hr=0x%0lx)", hr);
       return VLC_EGENERIC;
    }

    /* Set up the structure used to create the D3DDevice. */
    D3DPRESENT_PARAMETERS *d3dpp = &d3dctx->pp;
    ZeroMemory(d3dpp, sizeof(D3DPRESENT_PARAMETERS));
    d3dpp->Flags                  = D3DPRESENTFLAG_VIDEO;
    d3dpp->Windowed               = TRUE;
    d3dpp->hDeviceWindow          = d3dctx->hwnd;
    d3dpp->BackBufferWidth        = __MAX((unsigned int)GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                          source->i_width);
    d3dpp->BackBufferHeight       = __MAX((unsigned int)GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                          source->i_height);
    d3dpp->SwapEffect             = D3DSWAPEFFECT_COPY;
    d3dpp->MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp->PresentationInterval   = D3DPRESENT_INTERVAL_DEFAULT;
    d3dpp->BackBufferFormat       = d3ddm.Format;
    d3dpp->BackBufferCount        = 1;
    d3dpp->EnableAutoDepthStencil = FALSE;

    return VLC_SUCCESS;
}

/* */
static int  Direct3D9CreateResources (vout_display_t *, video_format_t *);
static void Direct3D9DestroyResources(vout_display_t *);

static int Direct3D9CreateDevice(vlc_object_t *o, struct d3dctx *d3dctx, const video_format_t *source)
{
    if (Direct3D9FillPresentationParameters(o, d3dctx, source))
        return VLC_EGENERIC;

    UINT AdapterToUse = D3DADAPTER_DEFAULT;
    D3DDEVTYPE DeviceType = D3DDEVTYPE_HAL;

#ifndef NDEBUG
    // Look for 'NVIDIA PerfHUD' adapter
    // If it is present, override default settings
    for (UINT Adapter=0; Adapter< IDirect3D9_GetAdapterCount(d3dctx->obj); ++Adapter) {
        D3DADAPTER_IDENTIFIER9 Identifier;
        HRESULT Res = IDirect3D9_GetAdapterIdentifier(d3dctx->obj,Adapter,0,&Identifier);
        if (SUCCEEDED(Res) && strstr(Identifier.Description,"PerfHUD") != 0) {
            AdapterToUse = Adapter;
            DeviceType = D3DDEVTYPE_REF;
            break;
        }
    }
#endif

    /* */
    D3DADAPTER_IDENTIFIER9 d3dai;
    if (FAILED(IDirect3D9_GetAdapterIdentifier(d3dctx->obj, AdapterToUse,0, &d3dai))) {
        msg_Warn(o, "IDirect3D9_GetAdapterIdentifier failed");
    } else {
        msg_Dbg(o, "Direct3d9 Device: %s %lu %lu %lu", d3dai.Description,
                d3dai.VendorId, d3dai.DeviceId, d3dai.Revision );
    }

    DWORD creationFlags = D3DCREATE_MULTITHREADED;
    if ( (d3dctx->caps.DevCaps & D3DDEVCAPS_DRAWPRIMTLVERTEX) &&
         (d3dctx->caps.DevCaps & D3DDEVCAPS_HWRASTERIZATION) ) {
        creationFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
    } else if (d3dctx->caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) {
        creationFlags |= D3DCREATE_MIXED_VERTEXPROCESSING;
    } else {
        creationFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }

    // Create the D3DDevice
    HRESULT hr;
    if (d3dctx->use_ex) {
        hr = IDirect3D9Ex_CreateDeviceEx(d3dctx->objex, AdapterToUse,
                                         DeviceType, d3dctx->hwnd,
                                         creationFlags,
                                         &d3dctx->pp, NULL, &d3dctx->devex);
    } else {
        hr = IDirect3D9_CreateDevice(d3dctx->obj, AdapterToUse,
                                     DeviceType, d3dctx->hwnd,
                                     creationFlags,
                                     &d3dctx->pp, &d3dctx->dev);
    }

    return FAILED(hr) ? VLC_EGENERIC : VLC_SUCCESS;
}

static void Direct3D9DestroyDevice(vlc_object_t *o, struct d3dctx *d3dctx)
{
    VLC_UNUSED(o);

    if (d3dctx->dev)
       IDirect3DDevice9_Release(d3dctx->dev);
    d3dctx->dev = NULL;
}

/**
 * It creates a Direct3D9 device and the associated resources.
 */
static int Direct3D9Open(vout_display_t *vd, video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;

    sys->d3dctx.hwnd = sys->sys.hvideownd;

    if (Direct3D9CreateDevice(VLC_OBJECT(vd), &sys->d3dctx, &vd->source) != VLC_SUCCESS)
        return VLC_EGENERIC;

    /* */
    RECT *display = &vd->sys->sys.rect_display;
    display->left   = 0;
    display->top    = 0;
    display->right  = sys->d3dctx.pp.BackBufferWidth;
    display->bottom = sys->d3dctx.pp.BackBufferHeight;

    *fmt = vd->source;

    /* Find the appropriate D3DFORMAT for the render chroma, the format will be the closest to
     * the requested chroma which is usable by the hardware in an offscreen surface, as they
     * typically support more formats than textures */
    const d3d_format_t *d3dfmt = Direct3DFindFormat(vd, fmt->i_chroma, sys->d3dctx.pp.BackBufferFormat);
    if (!d3dfmt) {
        msg_Err(vd, "surface pixel format is not supported.");
        goto error;
    }
    fmt->i_chroma = d3dfmt->fourcc;
    fmt->i_rmask  = d3dfmt->rmask;
    fmt->i_gmask  = d3dfmt->gmask;
    fmt->i_bmask  = d3dfmt->bmask;
    sys->d3dtexture_format = d3dfmt;

    UpdateRects(vd, NULL, true);

    if (Direct3D9CreateResources(vd, fmt)) {
        msg_Err(vd, "Failed to allocate resources");
        goto error;
    }

    /* Change the window title bar text */
    EventThreadUpdateTitle(sys->sys.event, VOUT_TITLE " (Direct3D9 output)");

    msg_Dbg(vd, "Direct3D9 device adapter successfully initialized");
    return VLC_SUCCESS;

error:
    Direct3D9DestroyDevice(VLC_OBJECT(vd), &sys->d3dctx);
    return VLC_EGENERIC;
}

/**
 * It releases the Direct3D9 device and its resources.
 */
static void Direct3D9Close(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    Direct3D9DestroyResources(vd);
    Direct3D9DestroyDevice(VLC_OBJECT(vd), &sys->d3dctx);
}

/**
 * It reset the Direct3D9 device and its resources.
 */
static int Direct3D9Reset(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (Direct3D9FillPresentationParameters(VLC_OBJECT(vd), &sys->d3dctx, &vd->source))
        return VLC_EGENERIC;

    /* release all D3D objects */
    Direct3D9DestroyResources(vd);

    /* */
    HRESULT hr;
    if (sys->d3dctx.use_ex){
        hr = IDirect3DDevice9Ex_ResetEx(sys->d3dctx.devex, &sys->d3dctx.pp, NULL);
    } else {
        hr = IDirect3DDevice9_Reset(sys->d3dctx.dev, &sys->d3dctx.pp);
    }
    if (FAILED(hr)) {
        msg_Err(vd, "IDirect3DDevice9_Reset failed! (hr=0x%0lx)", hr);
        return VLC_EGENERIC;
    }

    UpdateRects(vd, NULL, true);

    /* re-create them */
    if (Direct3D9CreateResources(vd, &vd->fmt)) {
        msg_Dbg(vd, "Direct3D9CreateResources failed !");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/* */
static int  Direct3D9CreateScene(vout_display_t *vd, const video_format_t *fmt);
static void Direct3D9DestroyScene(vout_display_t *vd);

static int  Direct3D9CreateShaders(vout_display_t *vd);
static void Direct3D9DestroyShaders(vout_display_t *vd);

/**
 * It creates the picture and scene resources.
 */
static int Direct3D9CreateResources(vout_display_t *vd, video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;

    if (Direct3D9CreateScene(vd, fmt)) {
        msg_Err(vd, "Direct3D scene initialization failed !");
        return VLC_EGENERIC;
    }
    if (Direct3D9CreateShaders(vd)) {
        /* Failing to initialize shaders is not fatal. */
        msg_Warn(vd, "Direct3D shaders initialization failed !");
    }

    sys->d3dregion_format = D3DFMT_UNKNOWN;
    for (int i = 0; i < 2; i++) {
        D3DFORMAT fmt = i == 0 ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8;
        if (SUCCEEDED(IDirect3D9_CheckDeviceFormat(sys->d3dctx.obj,
                                                   D3DADAPTER_DEFAULT,
                                                   D3DDEVTYPE_HAL,
                                                   sys->d3dctx.pp.BackBufferFormat,
                                                   D3DUSAGE_DYNAMIC,
                                                   D3DRTYPE_TEXTURE,
                                                   fmt))) {
            sys->d3dregion_format = fmt;
            break;
        }
    }
    return VLC_SUCCESS;
}
/**
 * It destroys the picture and scene resources.
 */
static void Direct3D9DestroyResources(vout_display_t *vd)
{
    Direct3D9DestroyScene(vd);
    if (vd->sys->sys.pool)
    {
        picture_pool_Release(vd->sys->sys.pool);
        vd->sys->sys.pool = NULL;
    }
    Direct3D9DestroyShaders(vd);
}

/**
 * It tests if the conversion from src to dst is supported.
 */
static int Direct3D9CheckConversion(vout_display_t *vd,
                                   D3DFORMAT src, D3DFORMAT dst)
{
    vout_display_sys_t *sys = vd->sys;
    LPDIRECT3D9 d3dobj = sys->d3dctx.obj;
    HRESULT hr;

    /* test whether device can create a surface of that format */
    hr = IDirect3D9_CheckDeviceFormat(d3dobj, D3DADAPTER_DEFAULT,
                                      D3DDEVTYPE_HAL, dst, 0,
                                      D3DRTYPE_SURFACE, src);
    if (SUCCEEDED(hr)) {
        /* test whether device can perform color-conversion
        ** from that format to target format
        */
        hr = IDirect3D9_CheckDeviceFormatConversion(d3dobj,
                                                    D3DADAPTER_DEFAULT,
                                                    D3DDEVTYPE_HAL,
                                                    src, dst);
    }
    if (!SUCCEEDED(hr)) {
        if (D3DERR_NOTAVAILABLE != hr)
            msg_Err(vd, "Could not query adapter supported formats. (hr=0x%0lx)", hr);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static const d3d_format_t d3d_formats[] = {
    /* YV12 is always used for planar 420, the planes are then swapped in Lock() */
    { "YV12",       MAKEFOURCC('Y','V','1','2'),    VLC_CODEC_YV12,  0,0,0 },
    { "YV12",       MAKEFOURCC('Y','V','1','2'),    VLC_CODEC_I420,  0,0,0 },
    { "YV12",       MAKEFOURCC('Y','V','1','2'),    VLC_CODEC_J420,  0,0,0 },
    { "NV12",       MAKEFOURCC('N','V','1','2'),    VLC_CODEC_NV12,  0,0,0 },
    { "DXA9",       MAKEFOURCC('N','V','1','2'),    VLC_CODEC_D3D9_OPAQUE,  0,0,0 },
    { "DXA9_10",    MAKEFOURCC('P','0','1','0'),    VLC_CODEC_D3D9_OPAQUE_10B,  0,0,0 },
    { "UYVY",       D3DFMT_UYVY,    VLC_CODEC_UYVY,  0,0,0 },
    { "YUY2",       D3DFMT_YUY2,    VLC_CODEC_YUYV,  0,0,0 },
    { "X8R8G8B8",   D3DFMT_X8R8G8B8,VLC_CODEC_RGB32, 0xff0000, 0x00ff00, 0x0000ff },
    { "A8R8G8B8",   D3DFMT_A8R8G8B8,VLC_CODEC_RGB32, 0xff0000, 0x00ff00, 0x0000ff },
    { "8G8B8",      D3DFMT_R8G8B8,  VLC_CODEC_RGB24, 0xff0000, 0x00ff00, 0x0000ff },
    { "R5G6B5",     D3DFMT_R5G6B5,  VLC_CODEC_RGB16, 0x1f<<11, 0x3f<<5,  0x1f<<0 },
    { "X1R5G5B5",   D3DFMT_X1R5G5B5,VLC_CODEC_RGB15, 0x1f<<10, 0x1f<<5,  0x1f<<0 },

    { NULL, 0, 0, 0,0,0}
};

/**
 * It returns the format (closest to chroma) that can be converted to target */
static const d3d_format_t *Direct3DFindFormat(vout_display_t *vd, vlc_fourcc_t chroma, D3DFORMAT target)
{
    vout_display_sys_t *sys = vd->sys;
    bool hardware_scale_ok = !(vd->fmt.i_visible_width & 1) && !(vd->fmt.i_visible_height & 1);
    if( !hardware_scale_ok )
        msg_Warn( vd, "Disabling hardware chroma conversion due to odd dimensions" );

    for (unsigned pass = 0; pass < 2; pass++) {
        const vlc_fourcc_t *list;
        const vlc_fourcc_t dxva_chroma[] = {chroma, 0};

        if (pass == 0 && is_d3d9_opaque(chroma))
            list = dxva_chroma;
        else if (pass == 0 && hardware_scale_ok && sys->allow_hw_yuv && vlc_fourcc_IsYUV(chroma))
            list = vlc_fourcc_GetYUVFallback(chroma);
        else if (pass == 1)
            list = vlc_fourcc_GetRGBFallback(chroma);
        else
            continue;

        for (unsigned i = 0; list[i] != 0; i++) {
            for (unsigned j = 0; d3d_formats[j].name; j++) {
                const d3d_format_t *format = &d3d_formats[j];

                if (format->fourcc != list[i])
                    continue;

                msg_Warn(vd, "trying surface pixel format: %s",
                         format->name);
                if (!Direct3D9CheckConversion(vd, format->format, target)) {
                    msg_Dbg(vd, "selected surface pixel format is %s",
                            format->name);
                    return format;
                }
            }
        }
    }
    return NULL;
}

/**
 * It allocates and initializes the resources needed to render the scene.
 */
static int Direct3D9CreateScene(vout_display_t *vd, const video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;
    LPDIRECT3DDEVICE9       d3ddev = sys->d3dctx.dev;
    HRESULT hr;

    /*
     * Create a texture for use when rendering a scene
     * for performance reason, texture format is identical to backbuffer
     * which would usually be a RGB format
     */
    LPDIRECT3DTEXTURE9 d3dtex;
    hr = IDirect3DDevice9_CreateTexture(d3ddev,
                                        fmt->i_width,
                                        fmt->i_height,
                                        1,
                                        D3DUSAGE_RENDERTARGET,
                                        sys->d3dctx.pp.BackBufferFormat,
                                        D3DPOOL_DEFAULT,
                                        &d3dtex,
                                        NULL);
    if (FAILED(hr)) {
        msg_Err(vd, "Failed to create texture. (hr=0x%lx)", hr);
        return VLC_EGENERIC;
    }

#ifndef NDEBUG
    msg_Dbg(vd, "Direct3D created texture: %ix%i",
                fmt->i_width, fmt->i_height);
#endif

    /*
    ** Create a vertex buffer for use when rendering scene
    */
    LPDIRECT3DVERTEXBUFFER9 d3dvtc;
    hr = IDirect3DDevice9_CreateVertexBuffer(d3ddev,
                                             sizeof(CUSTOMVERTEX)*4,
                                             D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,
                                             D3DFVF_CUSTOMVERTEX,
                                             D3DPOOL_DEFAULT,
                                             &d3dvtc,
                                             NULL);
    if (FAILED(hr)) {
        msg_Err(vd, "Failed to create vertex buffer. (hr=0x%lx)", hr);
        IDirect3DTexture9_Release(d3dtex);
        return VLC_EGENERIC;
    }

    /* */
    sys->d3dtex = d3dtex;
    sys->d3dvtc = d3dvtc;

    sys->d3dregion_count = 0;
    sys->d3dregion       = NULL;

    sys->clear_scene = true;

    // Texture coordinates outside the range [0.0, 1.0] are set
    // to the texture color at 0.0 or 1.0, respectively.
    IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Set linear filtering quality
    if (sys->d3dctx.caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) {
        //msg_Dbg(vd, "Using D3DTEXF_LINEAR for minification");
        IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    } else {
        //msg_Dbg(vd, "Using D3DTEXF_POINT for minification");
        IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (sys->d3dctx.caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) {
        //msg_Dbg(vd, "Using D3DTEXF_LINEAR for magnification");
        IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    } else {
        //msg_Dbg(vd, "Using D3DTEXF_POINT for magnification");
        IDirect3DDevice9_SetSamplerState(d3ddev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }

    // set maximum ambient light
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_AMBIENT, D3DCOLOR_XRGB(255,255,255));

    // Turn off culling
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_CULLMODE, D3DCULL_NONE);

    // Turn off the zbuffer
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ZENABLE, D3DZB_FALSE);

    // Turn off lights
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_LIGHTING, FALSE);

    // Enable dithering
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_DITHERENABLE, TRUE);

    // disable stencil
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_STENCILENABLE, FALSE);

    // manage blending
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

    if (sys->d3dctx.caps.AlphaCmpCaps & D3DPCMPCAPS_GREATER) {
        IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHATESTENABLE,TRUE);
        IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHAREF, 0x00);
        IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHAFUNC,D3DCMP_GREATER);
    }

    // Set texture states
    IDirect3DDevice9_SetTextureStageState(d3ddev, 0, D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(d3ddev, 0, D3DTSS_COLORARG1,D3DTA_TEXTURE);

    IDirect3DDevice9_SetTextureStageState(d3ddev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(d3ddev, 0, D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d3ddev, 0, D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);

    msg_Dbg(vd, "Direct3D9 scene created successfully");

    return VLC_SUCCESS;
}

/**
 * It releases the scene resources.
 */
static void Direct3D9DestroyScene(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    Direct3D9DeleteRegions(sys->d3dregion_count, sys->d3dregion);

    LPDIRECT3DVERTEXBUFFER9 d3dvtc = sys->d3dvtc;
    if (d3dvtc)
        IDirect3DVertexBuffer9_Release(d3dvtc);

    LPDIRECT3DTEXTURE9 d3dtex = sys->d3dtex;
    if (d3dtex)
        IDirect3DTexture9_Release(d3dtex);

    sys->d3dvtc = NULL;
    sys->d3dtex = NULL;

    sys->d3dregion_count = 0;
    sys->d3dregion       = NULL;

    msg_Dbg(vd, "Direct3D9 scene released successfully");
}

static int Direct3D9CompileShader(vout_display_t *vd, const char *shader_source, size_t source_length)
{
#ifdef HAVE_D3DX9EFFECT_H
    vout_display_sys_t *sys = vd->sys;

    HRESULT (WINAPI * OurD3DXCompileShader)(
            LPCSTR pSrcData,
            UINT srcDataLen,
            const D3DXMACRO *pDefines,
            LPD3DXINCLUDE pInclude,
            LPCSTR pFunctionName,
            LPCSTR pProfile,
            DWORD Flags,
            LPD3DXBUFFER *ppShader,
            LPD3DXBUFFER *ppErrorMsgs,
            LPD3DXCONSTANTTABLE *ppConstantTable);

    OurD3DXCompileShader = (void*)GetProcAddress(sys->d3dctx.hxdll, "D3DXCompileShader");
    if (!OurD3DXCompileShader) {
        msg_Warn(vd, "Cannot locate reference to D3DXCompileShader; pixel shading will be disabled");
        return VLC_EGENERIC;
    }

    LPD3DXBUFFER error_msgs = NULL;
    LPD3DXBUFFER compiled_shader = NULL;

    DWORD shader_flags = 0;
    HRESULT hr = OurD3DXCompileShader(shader_source, source_length, NULL, NULL,
                "main", "ps_3_0", shader_flags, &compiled_shader, &error_msgs, NULL);

    if (FAILED(hr)) {
        msg_Warn(vd, "D3DXCompileShader Error (hr=0x%0lx)", hr);
        if (error_msgs) {
            msg_Warn(vd, "HLSL Compilation Error: %s", (char*)ID3DXBuffer_GetBufferPointer(error_msgs));
            ID3DXBuffer_Release(error_msgs);
    }
        return VLC_EGENERIC;
    }

    hr = IDirect3DDevice9_CreatePixelShader(sys->d3dctx.dev,
            ID3DXBuffer_GetBufferPointer(compiled_shader),
            &sys->d3dx_shader);

    if (compiled_shader)
        ID3DXBuffer_Release(compiled_shader);
    if (error_msgs)
        ID3DXBuffer_Release(error_msgs);

    if (FAILED(hr)) {
        msg_Warn(vd, "IDirect3DDevice9_CreatePixelShader error (hr=0x%0lx)", hr);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
#else
    return VLC_EGENERIC;
#endif
}

#define MAX_SHADER_FILE_SIZE 1024*1024

static int Direct3D9CreateShaders(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->d3dctx.hxdll)
        return VLC_EGENERIC;

    /* Find which shader was selected in the list. */
    char *selected_shader = var_InheritString(vd, "direct3d9-shader");
    if (!selected_shader)
        return VLC_SUCCESS; /* Nothing to do */

    const char *shader_source_builtin = NULL;
    char *shader_source_file = NULL;
    FILE *fs = NULL;

    for (size_t i = 0; i < BUILTIN_SHADERS_COUNT; ++i) {
        if (!strcmp(selected_shader, builtin_shaders[i].name)) {
            shader_source_builtin = builtin_shaders[i].code;
            break;
        }
    }

    if (shader_source_builtin) {
        /* A builtin shader was selected. */
        int err = Direct3D9CompileShader(vd, shader_source_builtin, strlen(shader_source_builtin));
        if (err)
            goto error;
    } else {
        if (strcmp(selected_shader, SELECTED_SHADER_FILE))
            goto error; /* Unrecognized entry in the list. */
        /* The source code of the shader needs to be read from a file. */
        char *filepath = var_InheritString(vd, "direct3d9-shader-file");
        if (!filepath || !*filepath)
        {
            free(filepath);
            goto error;
        }
        /* Open file, find its size with fseek/ftell and read its content in a buffer. */
        fs = fopen(filepath, "rb");
        if (!fs)
            goto error;
        int ret = fseek(fs, 0, SEEK_END);
        if (ret == -1)
            goto error;
        long length = ftell(fs);
        if (length == -1 || length >= MAX_SHADER_FILE_SIZE)
            goto error;
        rewind(fs);
        shader_source_file = vlc_alloc(length, sizeof(*shader_source_file));
        if (!shader_source_file)
            goto error;
        ret = fread(shader_source_file, length, 1, fs);
        if (ret != 1)
            goto error;
        ret = Direct3D9CompileShader(vd, shader_source_file, length);
        if (ret)
            goto error;
    }

    free(selected_shader);
    free(shader_source_file);
    fclose(fs);

    return VLC_SUCCESS;

error:
    Direct3D9DestroyShaders(vd);
    free(selected_shader);
    free(shader_source_file);
    if (fs)
        fclose(fs);
    return VLC_EGENERIC;
}

static void Direct3D9DestroyShaders(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->d3dx_shader)
        IDirect3DPixelShader9_Release(sys->d3dx_shader);
    sys->d3dx_shader = NULL;
}

/**
 * Compute the vertex ordering needed to rotate the video. Without
 * rotation, the vertices of the rectangle are defined in a clockwise
 * order. This function computes a remapping of the coordinates to
 * implement the rotation, given fixed texture coordinates.
 * The unrotated order is the following:
 * 0--1
 * |  |
 * 3--2
 * For a 180 degrees rotation it should like this:
 * 2--3
 * |  |
 * 1--0
 * Vertex 0 should be assigned coordinates at index 2 from the
 * unrotated order and so on, thus yielding order: 2 3 0 1.
 */
static void orientationVertexOrder(video_orientation_t orientation, int vertex_order[static 4])
{
    switch (orientation) {
        case ORIENT_ROTATED_90:
            vertex_order[0] = 1;
            vertex_order[1] = 2;
            vertex_order[2] = 3;
            vertex_order[3] = 0;
            break;
        case ORIENT_ROTATED_270:
            vertex_order[0] = 3;
            vertex_order[1] = 0;
            vertex_order[2] = 1;
            vertex_order[3] = 2;
            break;
        case ORIENT_ROTATED_180:
            vertex_order[0] = 2;
            vertex_order[1] = 3;
            vertex_order[2] = 0;
            vertex_order[3] = 1;
            break;
        case ORIENT_TRANSPOSED:
            vertex_order[0] = 0;
            vertex_order[1] = 3;
            vertex_order[2] = 2;
            vertex_order[3] = 1;
            break;
        case ORIENT_HFLIPPED:
            vertex_order[0] = 3;
            vertex_order[1] = 2;
            vertex_order[2] = 1;
            vertex_order[3] = 0;
            break;
        case ORIENT_VFLIPPED:
            vertex_order[0] = 1;
            vertex_order[1] = 0;
            vertex_order[2] = 3;
            vertex_order[3] = 2;
            break;
        case ORIENT_ANTI_TRANSPOSED: /* transpose + vflip */
            vertex_order[0] = 1;
            vertex_order[1] = 2;
            vertex_order[2] = 3;
            vertex_order[3] = 0;
            break;
       default:
            vertex_order[0] = 0;
            vertex_order[1] = 1;
            vertex_order[2] = 2;
            vertex_order[3] = 3;
            break;
    }
}

static void  Direct3D9SetupVertices(CUSTOMVERTEX *vertices,
                                  const RECT *src, const RECT *src_clipped,
                                  const RECT *dst,
                                  int alpha,
                                  video_orientation_t orientation)
{
    /* Vertices of the dst rectangle in the unrotated (clockwise) order. */
    const int vertices_coords[4][2] = {
        { dst->left,  dst->top    },
        { dst->right, dst->top    },
        { dst->right, dst->bottom },
        { dst->left,  dst->bottom },
    };

    /* Compute index remapping necessary to implement the rotation. */
    int vertex_order[4];
    orientationVertexOrder(orientation, vertex_order);

    for (int i = 0; i < 4; ++i) {
        vertices[i].x  = vertices_coords[vertex_order[i]][0];
        vertices[i].y  = vertices_coords[vertex_order[i]][1];
    }

    float right = (float)src_clipped->right / (float)src->right;
    float left = (float)src_clipped->left / (float)src->right;
    float top = (float)src_clipped->top / (float)src->bottom;
    float bottom = (float)src_clipped->bottom / (float)src->bottom;

    vertices[0].tu = left;
    vertices[0].tv = top;

    vertices[1].tu = right;
    vertices[1].tv = top;

    vertices[2].tu = right;
    vertices[2].tv = bottom;

    vertices[3].tu = left;
    vertices[3].tv = bottom;

    for (int i = 0; i < 4; i++) {
        /* -0.5f is a "feature" of DirectX and it seems to apply to Direct3d also */
        /* http://www.sjbrown.co.uk/2003/05/01/fix-directx-rasterisation/ */
        vertices[i].x -= 0.5;
        vertices[i].y -= 0.5;

        vertices[i].z       = 0.0f;
        vertices[i].rhw     = 1.0f;
        vertices[i].diffuse = D3DCOLOR_ARGB(alpha, 255, 255, 255);
    }
}

/**
 * It copies picture surface into a texture and setup the associated d3d_region_t.
 */
static int Direct3D9ImportPicture(vout_display_t *vd,
                                 d3d_region_t *region,
                                 LPDIRECT3DSURFACE9 source)
{
    vout_display_sys_t *sys = vd->sys;
    HRESULT hr;

    if (!source) {
        msg_Dbg(vd, "no surface to render?");
        return VLC_EGENERIC;
    }

    /* retrieve texture top-level surface */
    LPDIRECT3DSURFACE9 destination;
    hr = IDirect3DTexture9_GetSurfaceLevel(sys->d3dtex, 0, &destination);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DTexture9_GetSurfaceLevel: 0x%0lx", hr);
        return VLC_EGENERIC;
    }

    /* Copy picture surface into texture surface
     * color space conversion happen here */
    RECT copy_rect = sys->sys.rect_src_clipped;
    // On nVidia & AMD, StretchRect will fail if the visible size isn't even.
    // When copying the entire buffer, the margin end up being blended in the actual picture
    // on nVidia (regardless of even/odd dimensions)
    if ( copy_rect.right & 1 ) copy_rect.right++;
    if ( copy_rect.left & 1 ) copy_rect.left--;
    if ( copy_rect.bottom & 1 ) copy_rect.bottom++;
    if ( copy_rect.top & 1 ) copy_rect.top--;
    hr = IDirect3DDevice9_StretchRect(sys->d3dctx.dev, source, &copy_rect, destination,
                                      &copy_rect, D3DTEXF_NONE);
    IDirect3DSurface9_Release(destination);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_StretchRect: source 0x%p 0x%0lx",
                (LPVOID)source, hr);
        return VLC_EGENERIC;
    }

    /* */
    region->texture = sys->d3dtex;
    Direct3D9SetupVertices(region->vertex, &vd->sys->sys.rect_src, &vd->sys->sys.rect_src_clipped,
                           &vd->sys->sys.rect_dest_clipped, 255, vd->fmt.orientation);
    return VLC_SUCCESS;
}

static void Direct3D9DeleteRegions(int count, d3d_region_t *region)
{
    for (int i = 0; i < count; i++) {
        if (region[i].texture)
            IDirect3DTexture9_Release(region[i].texture);
    }
    free(region);
}

static void Direct3D9ImportSubpicture(vout_display_t *vd,
                                     int *count_ptr, d3d_region_t **region,
                                     subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    int count = 0;
    for (subpicture_region_t *r = subpicture->p_region; r; r = r->p_next)
        count++;

    *count_ptr = count;
    *region    = calloc(count, sizeof(**region));
    if (*region == NULL) {
        *count_ptr = 0;
        return;
    }

    int i = 0;
    for (subpicture_region_t *r = subpicture->p_region; r; r = r->p_next, i++) {
        d3d_region_t *d3dr = &(*region)[i];
        HRESULT hr;

        d3dr->texture = NULL;
        for (int j = 0; j < sys->d3dregion_count; j++) {
            d3d_region_t *cache = &sys->d3dregion[j];
            if (cache->texture &&
                cache->format == sys->d3dregion_format &&
                cache->width  == r->fmt.i_width &&
                cache->height == r->fmt.i_height) {
                *d3dr = *cache;
                memset(cache, 0, sizeof(*cache));
                break;
            }
        }
        if (!d3dr->texture) {
            d3dr->format = sys->d3dregion_format;
            d3dr->width  = r->fmt.i_width;
            d3dr->height = r->fmt.i_height;
            hr = IDirect3DDevice9_CreateTexture(sys->d3dctx.dev,
                                                d3dr->width, d3dr->height,
                                                1,
                                                D3DUSAGE_DYNAMIC,
                                                d3dr->format,
                                                D3DPOOL_DEFAULT,
                                                &d3dr->texture,
                                                NULL);
            if (FAILED(hr)) {
                d3dr->texture = NULL;
                msg_Err(vd, "Failed to create %dx%d texture for OSD (hr=0x%0lx)",
                        d3dr->width, d3dr->height, hr);
                continue;
            }
#ifndef NDEBUG
            msg_Dbg(vd, "Created %dx%d texture for OSD",
                    r->fmt.i_width, r->fmt.i_height);
#endif
        }

        D3DLOCKED_RECT lock;
        hr = IDirect3DTexture9_LockRect(d3dr->texture, 0, &lock, NULL, 0);
        if (SUCCEEDED(hr)) {
            uint8_t  *dst_data   = lock.pBits;
            int       dst_pitch  = lock.Pitch;
            uint8_t  *src_data   = r->p_picture->p->p_pixels;
            int       src_pitch  = r->p_picture->p->i_pitch;

            if (d3dr->format == D3DFMT_A8B8G8R8) {
                if (dst_pitch == r->p_picture->p->i_pitch) {
                    memcpy(dst_data, src_data, r->fmt.i_height * dst_pitch);
                } else {
                    int copy_pitch = __MIN(dst_pitch, r->p_picture->p->i_pitch);
                    for (unsigned y = 0; y < r->fmt.i_height; y++) {
                        memcpy(&dst_data[y * dst_pitch], &src_data[y * src_pitch], copy_pitch);
                    }
                }
            } else {
                int copy_pitch = __MIN(dst_pitch, r->p_picture->p->i_pitch);
                for (unsigned y = 0; y < r->fmt.i_height; y++) {
                    for (int x = 0; x < copy_pitch; x += 4) {
                        dst_data[y * dst_pitch + x + 0] = src_data[y * src_pitch + x + 2];
                        dst_data[y * dst_pitch + x + 1] = src_data[y * src_pitch + x + 1];
                        dst_data[y * dst_pitch + x + 2] = src_data[y * src_pitch + x + 0];
                        dst_data[y * dst_pitch + x + 3] = src_data[y * src_pitch + x + 3];
                    }
                }
            }
            hr = IDirect3DTexture9_UnlockRect(d3dr->texture, 0);
            if (FAILED(hr))
                msg_Err(vd, "Failed to unlock the texture");
        } else {
            msg_Err(vd, "Failed to lock the texture");
        }

        /* Map the subpicture to sys->sys.rect_dest */
        const RECT video = sys->sys.rect_dest;
        const float scale_w = (float)(video.right  - video.left) / subpicture->i_original_picture_width;
        const float scale_h = (float)(video.bottom - video.top)  / subpicture->i_original_picture_height;

        RECT dst;
        dst.left   = video.left + scale_w * r->i_x,
        dst.right  = dst.left + scale_w * r->fmt.i_visible_width,
        dst.top    = video.top  + scale_h * r->i_y,
        dst.bottom = dst.top  + scale_h * r->fmt.i_visible_height;

        RECT src;
        src.left = 0;
        src.right = r->fmt.i_width;
        src.top = 0;
        src.bottom = r->fmt.i_height;

        RECT src_clipped;
        src_clipped.left = r->fmt.i_x_offset;
        src_clipped.right = r->fmt.i_x_offset + r->fmt.i_visible_width;
        src_clipped.top = r->fmt.i_y_offset;
        src_clipped.bottom = r->fmt.i_y_offset + r->fmt.i_visible_height;

        Direct3D9SetupVertices(d3dr->vertex, &src, &src_clipped,
                              &dst, subpicture->i_alpha * r->i_alpha / 255, ORIENT_NORMAL);
    }
}

static int Direct3D9RenderRegion(vout_display_t *vd,
                                d3d_region_t *region,
                                bool use_pixel_shader)
{
    vout_display_sys_t *sys = vd->sys;

    LPDIRECT3DDEVICE9 d3ddev = vd->sys->d3dctx.dev;

    LPDIRECT3DVERTEXBUFFER9 d3dvtc = sys->d3dvtc;
    LPDIRECT3DTEXTURE9      d3dtex = region->texture;

    HRESULT hr;

    /* Import vertices */
    void *vertex;
    hr = IDirect3DVertexBuffer9_Lock(d3dvtc, 0, 0, &vertex, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DVertexBuffer9_Lock: 0x%0lx", hr);
        return -1;
    }
    memcpy(vertex, region->vertex, sizeof(region->vertex));
    hr = IDirect3DVertexBuffer9_Unlock(d3dvtc);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DVertexBuffer9_Unlock: 0x%0lx", hr);
        return -1;
    }

    // Setup our texture. Using textures introduces the texture stage states,
    // which govern how textures get blended together (in the case of multiple
    // textures) and lighting information. In this case, we are modulating
    // (blending) our texture with the diffuse color of the vertices.
    hr = IDirect3DDevice9_SetTexture(d3ddev, 0, (LPDIRECT3DBASETEXTURE9)d3dtex);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_SetTexture: 0x%0lx", hr);
        return -1;
    }

    if (sys->d3dx_shader) {
        if (use_pixel_shader)
        {
            hr = IDirect3DDevice9_SetPixelShader(d3ddev, sys->d3dx_shader);
            float shader_data[4] = { region->width, region->height, 0, 0 };
            hr = IDirect3DDevice9_SetPixelShaderConstantF(d3ddev, 0, shader_data, 1);
            if (FAILED(hr)) {
                msg_Dbg(vd, "Failed IDirect3DDevice9_SetPixelShaderConstantF: 0x%0lx", hr);
                return -1;
            }
        }
        else /* Disable any existing pixel shader. */
            hr = IDirect3DDevice9_SetPixelShader(d3ddev, NULL);
        if (FAILED(hr)) {
            msg_Dbg(vd, "Failed IDirect3DDevice9_SetPixelShader: 0x%0lx", hr);
            return -1;
        }
    }

    // Render the vertex buffer contents
    hr = IDirect3DDevice9_SetStreamSource(d3ddev, 0, d3dvtc, 0, sizeof(CUSTOMVERTEX));
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_SetStreamSource: 0x%0lx", hr);
        return -1;
    }

    // we use FVF instead of vertex shader
    hr = IDirect3DDevice9_SetFVF(d3ddev, D3DFVF_CUSTOMVERTEX);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_SetFVF: 0x%0lx", hr);
        return -1;
    }

    // draw rectangle
    hr = IDirect3DDevice9_DrawPrimitive(d3ddev, D3DPT_TRIANGLEFAN, 0, 2);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_DrawPrimitive: 0x%0lx", hr);
        return -1;
    }
    return 0;
}

/**
 * It renders the scene.
 *
 * This function is intented for higher end 3D cards, with pixel shader support
 * and at least 64 MiB of video RAM.
 */
static void Direct3D9RenderScene(vout_display_t *vd,
                                d3d_region_t *picture,
                                int subpicture_count,
                                d3d_region_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    LPDIRECT3DDEVICE9 d3ddev = sys->d3dctx.dev;
    HRESULT hr;

    if (sys->clear_scene) {
        /* Clear the backbuffer and the zbuffer */
        hr = IDirect3DDevice9_Clear(d3ddev, 0, NULL, D3DCLEAR_TARGET,
                                  D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (FAILED(hr)) {
            msg_Dbg(vd, "Failed IDirect3DDevice9_Clear: 0x%0lx", hr);
            return;
        }
        sys->clear_scene = false;
    }

    // Begin the scene
    hr = IDirect3DDevice9_BeginScene(d3ddev);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_BeginScene: 0x%0lx", hr);
        return;
    }

    Direct3D9RenderRegion(vd, picture, true);

    if (subpicture_count > 0)
        IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHABLENDENABLE, TRUE);
    for (int i = 0; i < subpicture_count; i++) {
        d3d_region_t *r = &subpicture[i];
        if (r->texture)
            Direct3D9RenderRegion(vd, r, false);
    }
    if (subpicture_count > 0)
        IDirect3DDevice9_SetRenderState(d3ddev, D3DRS_ALPHABLENDENABLE, FALSE);

    // End the scene
    hr = IDirect3DDevice9_EndScene(d3ddev);
    if (FAILED(hr)) {
        msg_Dbg(vd, "Failed IDirect3DDevice9_EndScene: 0x%0lx", hr);
        return;
    }
}

/*****************************************************************************
 * DesktopCallback: desktop mode variable callback
 *****************************************************************************/
static int DesktopCallback(vlc_object_t *object, char const *psz_cmd,
                            vlc_value_t oldval, vlc_value_t newval,
                            void *p_data)
{
    vout_display_t *vd = (vout_display_t *)object;
    vout_display_sys_t *sys = vd->sys;
    VLC_UNUSED(psz_cmd);
    VLC_UNUSED(oldval);
    VLC_UNUSED(p_data);

    vlc_mutex_lock(&sys->lock);
    const bool ch_desktop = !sys->desktop_requested != !newval.b_bool;
    sys->ch_desktop |= ch_desktop;
    sys->desktop_requested = newval.b_bool;
    vlc_mutex_unlock(&sys->lock);
    return VLC_SUCCESS;
}

typedef struct
{
    char **values;
    char **descs;
    size_t count;
} enum_context_t;

static void ListShaders(enum_context_t *ctx)
{
    size_t num_shaders = BUILTIN_SHADERS_COUNT;
    ctx->values = xrealloc(ctx->values, (ctx->count + num_shaders + 1) * sizeof(char *));
    ctx->descs = xrealloc(ctx->descs, (ctx->count + num_shaders + 1) * sizeof(char *));
    for (size_t i = 0; i < num_shaders; ++i) {
        ctx->values[ctx->count] = strdup(builtin_shaders[i].name);
        ctx->descs[ctx->count] = strdup(builtin_shaders[i].name);
        ctx->count++;
    }
    ctx->values[ctx->count] = strdup(SELECTED_SHADER_FILE);
    ctx->descs[ctx->count] = strdup(SELECTED_SHADER_FILE);
    ctx->count++;
}

/* Populate the list of available shader techniques in the options */
static int FindShadersCallback(vlc_object_t *object, const char *name,
                               char ***values, char ***descs)
{
    VLC_UNUSED(object);
    VLC_UNUSED(name);

    enum_context_t ctx = { NULL, NULL, 0 };

    ListShaders(&ctx);

    *values = ctx.values;
    *descs = ctx.descs;
    return ctx.count;

}

#include "../opengl/converter.h"
#include <GL/wglew.h>

struct wgl_vt {
    PFNWGLDXSETRESOURCESHAREHANDLENVPROC DXSetResourceShareHandleNV;
    PFNWGLDXOPENDEVICENVPROC             DXOpenDeviceNV;
    PFNWGLDXCLOSEDEVICENVPROC            DXCloseDeviceNV;
    PFNWGLDXREGISTEROBJECTNVPROC         DXRegisterObjectNV;
    PFNWGLDXUNREGISTEROBJECTNVPROC       DXUnregisterObjectNV;
    PFNWGLDXLOCKOBJECTSNVPROC            DXLockObjectsNV;
    PFNWGLDXUNLOCKOBJECTSNVPROC          DXUnlockObjectsNV;
};
struct glpriv
{
    struct wgl_vt vt;
    struct d3dctx d3dctx;
    HANDLE gl_handle_d3d;
    HANDLE gl_render;
    IDirect3DSurface9 *dx_render;
};

static int
GLConvUpdate(const opengl_tex_converter_t *tc, GLuint *textures,
             const GLsizei *tex_width, const GLsizei *tex_height,
             picture_t *pic, const size_t *plane_offset)
{
    VLC_UNUSED(textures); VLC_UNUSED(tex_width); VLC_UNUSED(tex_height); VLC_UNUSED(plane_offset);
    struct glpriv *priv = tc->priv;
    HRESULT hr;

    picture_sys_t *picsys = ActivePictureSys(pic);
    if (!picsys)
        return VLC_EGENERIC;

    if (!priv->vt.DXUnlockObjectsNV(priv->gl_handle_d3d, 1, &priv->gl_render))
    {
        msg_Warn(tc->gl, "DXUnlockObjectsNV failed");
        return VLC_EGENERIC;
    }

    hr = IDirect3DDevice9Ex_StretchRect(priv->d3dctx.devex, picsys->surface,
                                        NULL, priv->dx_render, NULL, D3DTEXF_NONE);
    if (FAILED(hr))
    {
        msg_Warn(tc->gl, "IDirect3DDevice9Ex_StretchRect failed");
        return VLC_EGENERIC;
    }

    if (!priv->vt.DXLockObjectsNV(priv->gl_handle_d3d, 1, &priv->gl_render))
    {
        msg_Warn(tc->gl, "DXLockObjectsNV failed");
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static picture_pool_t *
GLConvGetPool(const opengl_tex_converter_t *tc, unsigned requested_count)
{
    struct glpriv *priv = tc->priv;
    return Direct3D9CreatePicturePool(VLC_OBJECT(tc->gl), &priv->d3dctx, NULL,
                                      &tc->fmt, requested_count);
}

static int
GLConvAllocateTextures(const opengl_tex_converter_t *tc, GLuint *textures,
                       const GLsizei *tex_width, const GLsizei *tex_height)
{
    VLC_UNUSED(tex_width); VLC_UNUSED(tex_height);
    struct glpriv *priv = tc->priv;
    tc->vt->GenTextures(1, textures);

    tc->vt->ActiveTexture(GL_TEXTURE0);
    tc->vt->BindTexture(tc->tex_target, textures[0]);
    tc->vt->TexParameteri(tc->tex_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    tc->vt->TexParameteri(tc->tex_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    tc->vt->TexParameterf(tc->tex_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    tc->vt->TexParameterf(tc->tex_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    priv->gl_render =
        priv->vt.DXRegisterObjectNV(priv->gl_handle_d3d, priv->dx_render,
                                    textures[0], GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!priv->gl_render)
    {
        msg_Warn(tc->gl, "DXRegisterObjectNV failed: %lu", GetLastError());
        return VLC_EGENERIC;
    }

    if (!priv->vt.DXLockObjectsNV(priv->gl_handle_d3d, 1, &priv->gl_render))
    {
        msg_Warn(tc->gl, "DXLockObjectsNV failed");
        priv->vt.DXUnregisterObjectNV(priv->gl_handle_d3d, priv->gl_render);
        priv->gl_render = NULL;
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void
GLConvClose(vlc_object_t *obj)
{
    opengl_tex_converter_t *tc = (void *)obj;
    struct glpriv *priv = tc->priv;

    if (priv->gl_handle_d3d)
    {
        if (priv->gl_render)
        {
            priv->vt.DXUnlockObjectsNV(priv->gl_handle_d3d, 1, &priv->gl_render);
            priv->vt.DXUnregisterObjectNV(priv->gl_handle_d3d, priv->gl_render);
        }

        priv->vt.DXCloseDeviceNV(priv->gl_handle_d3d);
    }

    if (priv->dx_render)
        IDirect3DSurface9_Release(priv->dx_render);

    Direct3D9DestroyDevice(obj, &priv->d3dctx);
    Direct3D9Destroy(obj, &priv->d3dctx);
    free(tc->priv);
}

static int
GLConvOpen(vlc_object_t *obj)
{
    opengl_tex_converter_t *tc = (void *) obj;

    if (tc->fmt.i_chroma != VLC_CODEC_D3D9_OPAQUE
     && tc->fmt.i_chroma != VLC_CODEC_D3D9_OPAQUE_10B)
        return VLC_EGENERIC;

    if (tc->gl->ext != VLC_GL_EXT_WGL || !tc->gl->wgl.getExtensionsString)
        return VLC_EGENERIC;

    const char *wglExt = tc->gl->wgl.getExtensionsString(tc->gl);

    if (wglExt == NULL || !HasExtension(wglExt, "WGL_NV_DX_interop"))
        return VLC_EGENERIC;

    struct wgl_vt vt;
#define LOAD_EXT(name, type) do { \
    vt.name = (type) vlc_gl_GetProcAddress(tc->gl, "wgl" #name); \
    if (!vt.name) { \
        msg_Warn(obj, "'wgl " #name "' could not be loaded"); \
        return VLC_EGENERIC; \
    } \
} while(0)

    LOAD_EXT(DXSetResourceShareHandleNV, PFNWGLDXSETRESOURCESHAREHANDLENVPROC);
    LOAD_EXT(DXOpenDeviceNV, PFNWGLDXOPENDEVICENVPROC);
    LOAD_EXT(DXCloseDeviceNV, PFNWGLDXCLOSEDEVICENVPROC);
    LOAD_EXT(DXRegisterObjectNV, PFNWGLDXREGISTEROBJECTNVPROC);
    LOAD_EXT(DXUnregisterObjectNV, PFNWGLDXUNREGISTEROBJECTNVPROC);
    LOAD_EXT(DXLockObjectsNV, PFNWGLDXLOCKOBJECTSNVPROC);
    LOAD_EXT(DXUnlockObjectsNV, PFNWGLDXUNLOCKOBJECTSNVPROC);

    struct glpriv *priv = calloc(1, sizeof(struct glpriv));
    if (!priv)
        return VLC_ENOMEM;
    tc->priv = priv;
    priv->vt = vt;

    priv->d3dctx = (struct d3dctx) { .hwnd = tc->gl->surface->handle.hwnd };
    if (Direct3D9Create(obj, &priv->d3dctx, &tc->fmt) != VLC_SUCCESS)
        goto error;

    if (!priv->d3dctx.use_ex)
    {
        msg_Warn(obj, "DX/GL interrop only working on d3d9x");
        goto error;
    }

    if (Direct3D9CreateDevice(obj, &priv->d3dctx, &tc->fmt) != VLC_SUCCESS)
        goto error;

    HRESULT hr;
    HANDLE shared_handle = NULL;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(priv->d3dctx.dev,
                                                      tc->fmt.i_width,
                                                      tc->fmt.i_height,
                                                      D3DFMT_X8R8G8B8,
                                                      D3DPOOL_DEFAULT,
                                                      &priv->dx_render,
                                                      &shared_handle);
    if (FAILED(hr))
    {
        msg_Warn(obj, "IDirect3DDevice9_CreateOffscreenPlainSurface failed");
        goto error;
    }

   if (shared_handle)
        priv->vt.DXSetResourceShareHandleNV(priv->dx_render, shared_handle);

    priv->gl_handle_d3d = priv->vt.DXOpenDeviceNV(priv->d3dctx.dev);
    if (!priv->gl_handle_d3d)
    {
        msg_Warn(obj, "DXOpenDeviceNV failed: %lu", GetLastError());
        goto error;
    }

    tc->pf_update  = GLConvUpdate;
    tc->pf_get_pool = GLConvGetPool;
    tc->pf_allocate_textures = GLConvAllocateTextures;

    tc->fshader = opengl_fragment_shader_init(tc, GL_TEXTURE_2D, VLC_CODEC_RGB32,
                                              COLOR_SPACE_UNDEF);
    if (tc->fshader == 0)
        goto error;

    return VLC_SUCCESS;

error:
    GLConvClose(obj);
    return VLC_EGENERIC;
}
