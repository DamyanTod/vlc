/*****************************************************************************
 * transcode.c
 *****************************************************************************
 * Copyright (C) 2001, 2002 VideoLAN
 * $Id: transcode.c,v 1.31 2003/08/26 18:01:16 fenrir Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <string.h>

#include <vlc/vlc.h>
#include <vlc/input.h>
#include <vlc/sout.h>

/* ffmpeg header */
#ifdef HAVE_FFMPEG_AVCODEC_H
#   include <ffmpeg/avcodec.h>
#else
#   include <avcodec.h>
#endif

/* vorbis header */
#ifdef HAVE_VORBIS_VORBISENC_H
#   include <vorbis/vorbisenc.h>
#   ifndef OV_ECTL_RATEMANAGE_AVG
#       define OV_ECTL_RATEMANAGE_AVG 0x0
#   endif
#endif

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

static sout_stream_id_t *Add ( sout_stream_t *, sout_format_t * );
static int               Del ( sout_stream_t *, sout_stream_id_t * );
static int               Send( sout_stream_t *, sout_stream_id_t *, sout_buffer_t* );

static int  transcode_audio_ffmpeg_new    ( sout_stream_t *, sout_stream_id_t * );
static void transcode_audio_ffmpeg_close  ( sout_stream_t *, sout_stream_id_t * );
static int  transcode_audio_ffmpeg_process( sout_stream_t *, sout_stream_id_t *, sout_buffer_t *, sout_buffer_t ** );

static int  transcode_video_ffmpeg_new    ( sout_stream_t *, sout_stream_id_t * );
static void transcode_video_ffmpeg_close  ( sout_stream_t *, sout_stream_id_t * );
static int  transcode_video_ffmpeg_process( sout_stream_t *, sout_stream_id_t *, sout_buffer_t *, sout_buffer_t ** );

static int  transcode_video_ffmpeg_getframebuf( struct AVCodecContext *, AVFrame *);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();
    set_description( _("Transcode stream") );
    set_capability( "sout stream", 50 );
    add_shortcut( "transcode" );
    set_callbacks( Open, Close );
vlc_module_end();

struct sout_stream_sys_t
{
    sout_stream_t   *p_out;

    vlc_fourcc_t    i_acodec;   /* codec audio (0 if not transcode) */
    int             i_sample_rate;
    int             i_channels;
    int             i_abitrate;

    vlc_fourcc_t    i_vcodec;   /*    "   video  " "   "      " */
    int             i_vbitrate;
    int             i_vtolerance;
    int             i_width;
    int             i_height;
    int             i_b_frames;
    int             i_key_int;
    int             i_qmin;
    int             i_qmax;
    vlc_bool_t      i_hq;
    vlc_bool_t      b_deinterlace;

    int             i_crop_top;
    int             i_crop_bottom;
    int             i_crop_right;
    int             i_crop_left;

    mtime_t         i_input_pts;
    mtime_t         i_output_pts;
    mtime_t         i_last_ref_pts;

    mtime_t         i_buggy_pts_detect;
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t     *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys;
    char *codec;

    p_sys = malloc( sizeof( sout_stream_sys_t ) );
    p_sys->p_out = sout_stream_new( p_stream->p_sout, p_stream->psz_next );

    p_sys->i_acodec      = 0;
    p_sys->i_sample_rate = 0;
    p_sys->i_channels    = 0;
    p_sys->i_abitrate    = 0;

    p_sys->i_vcodec     = 0;
    p_sys->i_vbitrate   = 0;
    p_sys->i_vtolerance = -1;
    p_sys->i_width      = 0;
    p_sys->i_height     = 0;
    p_sys->i_key_int    = -1;
    p_sys->i_b_frames   = 0;
    p_sys->i_qmin       = 2;
    p_sys->i_qmax       = 31;
#if LIBAVCODEC_BUILD >= 4673
    p_sys->i_hq         = FF_MB_DECISION_SIMPLE;
#else
    p_sys->i_hq         = VLC_FALSE;
#endif
    p_sys->b_deinterlace= VLC_FALSE;

    p_sys->i_crop_top   = 0;
    p_sys->i_crop_bottom= 0;
    p_sys->i_crop_right = 0;
    p_sys->i_crop_left  = 0;

    if( ( codec = sout_cfg_find_value( p_stream->p_cfg, "acodec" ) ) )
    {
        char fcc[4] = "    ";
        char *val;

        memcpy( fcc, codec, strlen( codec ) );

        p_sys->i_acodec = VLC_FOURCC( fcc[0], fcc[1], fcc[2], fcc[3] );

        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "samplerate" ) ) )
        {
            p_sys->i_sample_rate = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "channels" ) ) )
        {
            p_sys->i_channels = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "ab" ) ) )
        {
            p_sys->i_abitrate = atoi( val );
            if( p_sys->i_abitrate < 4000 )
            {
                p_sys->i_abitrate *= 1000;
            }
        }

        msg_Dbg( p_stream, "codec audio=%4.4s %dHz %d channels %dKb/s", fcc,
                 p_sys->i_sample_rate, p_sys->i_channels,
                 p_sys->i_abitrate / 1024 );
    }

    if( ( codec = sout_cfg_find_value( p_stream->p_cfg, "vcodec" ) ) )
    {
        char fcc[4] = "    ";
        char *val;

        memcpy( fcc, codec, strlen( codec ) );

        p_sys->i_vcodec = VLC_FOURCC( fcc[0], fcc[1], fcc[2], fcc[3] );

        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "width" ) ) )
        {
            p_sys->i_width = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "height" ) ) )
        {
            p_sys->i_height = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "vb" ) ) )
        {
            p_sys->i_vbitrate = atoi( val );
            if( p_sys->i_vbitrate < 16000 )
            {
                p_sys->i_vbitrate *= 1000;
            }
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "vt" ) ) )
        {
            p_sys->i_vtolerance = atoi( val );
        }
        if( sout_cfg_find( p_stream->p_cfg, "deinterlace" ) )
        {
            p_sys->b_deinterlace = VLC_TRUE;
        }
        /* crop */
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "croptop" ) ) )
        {
            p_sys->i_crop_top = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "cropbottom" ) ) )
        {
            p_sys->i_crop_bottom = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "cropleft" ) ) )
        {
            p_sys->i_crop_left = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "cropright" ) ) )
        {
            p_sys->i_crop_right = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "keyint" ) ) )
        {
            p_sys->i_key_int    = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "bframes" ) ) )
        {
            p_sys->i_b_frames   = atoi( val );
        }
#if LIBAVCODEC_BUILD >= 4673
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "hq" ) ) )
        {
            if( !strcmp( val, "rd" ) )
            {
                p_sys->i_hq = FF_MB_DECISION_RD;
            }
            else if( !strcmp( val, "bits" ) )
            {
                p_sys->i_hq = FF_MB_DECISION_BITS;
            }
            else if( !strcmp( val, "simple" ) )
            {
                p_sys->i_hq = FF_MB_DECISION_SIMPLE;
            }
            else
            {
                p_sys->i_hq = FF_MB_DECISION_RD;
            }
        }
#else
        if( sout_cfg_find( p_stream->p_cfg, "hq" ) )
        {
            p_sys->i_hq = VLC_TRUE;
        }
#endif
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "qmin" ) ) )
        {
            p_sys->i_qmin   = atoi( val );
        }
        if( ( val = sout_cfg_find_value( p_stream->p_cfg, "qmax" ) ) )
        {
            p_sys->i_qmax   = atoi( val );
        }

        msg_Dbg( p_stream, "codec video=%4.4s %dx%d %dkb/s",
                 fcc,
                 p_sys->i_width, p_sys->i_height,
                 p_sys->i_vbitrate / 1024 );
    }

    if( !p_sys->p_out )
    {
        msg_Err( p_stream, "cannot create chain" );
        free( p_sys );
        return VLC_EGENERIC;
    }
    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;

    p_stream->p_sys     = p_sys;

    avcodec_init();
    avcodec_register_all();

    /* ffmpeg needs some padding at the end of each buffer */
    p_stream->p_sout->i_padding += FF_INPUT_BUFFER_PADDING_SIZE;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/

static void Close( vlc_object_t * p_this )
{
    sout_stream_t       *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t   *p_sys = p_stream->p_sys;

    sout_stream_delete( p_sys->p_out );
    free( p_sys );
}

struct sout_stream_id_t
{
    vlc_fourcc_t  b_transcode;
    sout_format_t f_src;        /* only if transcoding */
    sout_format_t f_dst;        /*  "   "      " */

    /* id of the out stream */
    void *id;

    /* ffmpeg part */
    AVCodec         *ff_dec;
    AVCodecContext  *ff_dec_c;


    vlc_fourcc_t    b_enc_inited;
    AVCodec         *ff_enc;
    AVCodecContext  *ff_enc_c;

    mtime_t         i_dts;
    mtime_t         i_length;

    int             i_buffer_in;
    int             i_buffer_in_pos;
    uint8_t         *p_buffer_in;

    int             i_buffer;
    int             i_buffer_pos;
    uint8_t         *p_buffer;

    int             i_buffer_out;
    int             i_buffer_out_pos;
    uint8_t         *p_buffer_out;

    AVFrame         *p_ff_pic;
    AVFrame         *p_ff_pic_tmp0; /* to do deinterlace */
    AVFrame         *p_ff_pic_tmp1; /* to do pix conversion */
    AVFrame         *p_ff_pic_tmp2; /* to do resample */

    ImgReSampleContext *p_vresample;

#ifdef HAVE_VORBIS_VORBISENC_H

    /* Vorbis part */
    vorbis_info      *p_vi;
    vorbis_dsp_state *p_vd;
    vorbis_block     *p_vb;
    vorbis_comment   *p_vc;
    int              i_last_block_size;
    int              i_samples_delay;
    vlc_bool_t       b_headers_sent;
#endif
};


static sout_stream_id_t * Add( sout_stream_t *p_stream, sout_format_t *p_fmt )
{
    sout_stream_sys_t   *p_sys = p_stream->p_sys;
    sout_stream_id_t    *id;

    id = malloc( sizeof( sout_stream_id_t ) );
    id->i_dts = 0;
    id->id = NULL;
    if( p_fmt->i_cat == AUDIO_ES && p_sys->i_acodec != 0 )
    {
        msg_Dbg( p_stream,
                 "creating audio transcoding from fcc=`%4.4s' to fcc=`%4.4s'",
                 (char*)&p_fmt->i_fourcc,
                 (char*)&p_sys->i_acodec );

        /* src format */
        memcpy( &id->f_src, p_fmt, sizeof( sout_format_t ) );

        /* create dst format */
        id->f_dst.i_cat    = AUDIO_ES;
        id->f_dst.i_fourcc = p_sys->i_acodec;
        id->f_dst.i_sample_rate = p_sys->i_sample_rate  > 0 ? p_sys->i_sample_rate : id->f_src.i_sample_rate;
        id->f_dst.i_channels    = p_sys->i_channels > 0 ? p_sys->i_channels : id->f_src.i_channels;
        id->f_dst.i_bitrate     = p_sys->i_abitrate > 0 ? p_sys->i_abitrate : 64000;
        id->f_dst.i_block_align = 0;
        id->f_dst.i_extra_data  = 0;
        id->f_dst.p_extra_data  = NULL;

        /* build decoder -> filter -> encoder */
        if( transcode_audio_ffmpeg_new( p_stream, id ) )
        {
            msg_Err( p_stream, "cannot create audio chain" );
            free( id );
            return NULL;
        }

        /* open output stream */
        id->id = p_sys->p_out->pf_add( p_sys->p_out, &id->f_dst );
        id->b_transcode = VLC_TRUE;

        if( id->id == NULL )
        {
            free( id );
            return NULL;
        }
    }
    else if( p_fmt->i_cat == VIDEO_ES && p_sys->i_vcodec != 0 )
    {
        msg_Dbg( p_stream,
                 "creating video transcoding from fcc=`%4.4s' to fcc=`%4.4s'",
                 (char*)&p_fmt->i_fourcc,
                 (char*)&p_sys->i_vcodec );

        memcpy( &id->f_src, p_fmt, sizeof( sout_format_t ) );

        /* create dst format */
        id->f_dst.i_cat         = VIDEO_ES;
        id->f_dst.i_fourcc      = p_sys->i_vcodec;
        id->f_dst.i_width       = p_sys->i_width ; /* > 0 ? p_sys->i_width : id->f_src.i_width; */
        id->f_dst.i_height      = p_sys->i_height; /* > 0 ? p_sys->i_height: id->f_src.i_height; */
        id->f_dst.i_bitrate     = p_sys->i_vbitrate > 0 ? p_sys->i_vbitrate : 800*1000;
        id->f_dst.i_extra_data  = 0;
        id->f_dst.p_extra_data  = NULL;

        /* build decoder -> filter -> encoder */
        if( transcode_video_ffmpeg_new( p_stream, id ) )
        {
            msg_Err( p_stream, "cannot create video chain" );
            free( id );
            return NULL;
        }
#if 0
        /* open output stream */
        id->id = p_sys->p_out->pf_add( p_sys->p_out, &id->f_dst );
#endif
        id->b_transcode = VLC_TRUE;
    }
    else
    {
        msg_Dbg( p_stream, "not transcoding a stream (fcc=`%4.4s')", (char*)&p_fmt->i_fourcc );
        id->id = p_sys->p_out->pf_add( p_sys->p_out, p_fmt );
        id->b_transcode = VLC_FALSE;

        if( id->id == NULL )
        {
            free( id );
            return NULL;
        }
    }

    return id;
}

static int     Del      ( sout_stream_t *p_stream, sout_stream_id_t *id )
{
    sout_stream_sys_t   *p_sys = p_stream->p_sys;

    if( id->b_transcode )
    {
        if( id->f_src.i_cat == AUDIO_ES )
        {
            transcode_audio_ffmpeg_close( p_stream, id );
        }
        else if( id->f_src.i_cat == VIDEO_ES )
        {
            transcode_video_ffmpeg_close( p_stream, id );
        }
    }

    if( id->id ) p_sys->p_out->pf_del( p_sys->p_out, id->id );
    free( id );

    return VLC_SUCCESS;
}

static int Send( sout_stream_t *p_stream, sout_stream_id_t *id,
                 sout_buffer_t *p_buffer )
{
    sout_stream_sys_t   *p_sys = p_stream->p_sys;

    if( id->b_transcode )
    {
        sout_buffer_t *p_buffer_out;
        if( id->f_src.i_cat == AUDIO_ES )
        {
            transcode_audio_ffmpeg_process( p_stream, id, p_buffer, &p_buffer_out );
        }
        else if( id->f_src.i_cat == VIDEO_ES )
        {
            transcode_video_ffmpeg_process( p_stream, id, p_buffer, &p_buffer_out );
        }
        sout_BufferDelete( p_stream->p_sout, p_buffer );

        if( p_buffer_out )
        {
            return p_sys->p_out->pf_send( p_sys->p_out, id->id, p_buffer_out );
        }
        return VLC_SUCCESS;
    }
    else if( id->id != NULL )
    {
        return p_sys->p_out->pf_send( p_sys->p_out, id->id, p_buffer );
    }
    else
    {
        sout_BufferDelete( p_stream->p_sout, p_buffer );
        return VLC_EGENERIC;
    }
}

/****************************************************************************
 * ffmpeg decoder reencocdr part
 ****************************************************************************/

static struct
{
    vlc_fourcc_t i_fcc;
    int          i_ff_codec;
} fourcc_to_ff_code[] =
{
    /* audio */
    { VLC_FOURCC( 'm', 'p', 'g', 'a' ), CODEC_ID_MP2 },
    { VLC_FOURCC( 'm', 'p', '3', ' ' ), CODEC_ID_MP3LAME },
    { VLC_FOURCC( 'a', '5', '2', ' ' ), CODEC_ID_AC3 },
    { VLC_FOURCC( 'a', 'c', '3', ' ' ), CODEC_ID_AC3 },
    { VLC_FOURCC( 'w', 'm', 'a', '1' ), CODEC_ID_WMAV1 },
    { VLC_FOURCC( 'w', 'm', 'a', '2' ), CODEC_ID_WMAV2 },
    { VLC_FOURCC( 'v', 'o', 'r', 'b' ), CODEC_ID_VORBIS },
    { VLC_FOURCC( 'a', 'l', 'a', 'w' ), CODEC_ID_PCM_ALAW },

    /* video */
    { VLC_FOURCC( 'm', 'p', 'g', 'v' ), CODEC_ID_MPEG1VIDEO },
    { VLC_FOURCC( 'm', 'p', '1', 'v' ), CODEC_ID_MPEG1VIDEO },
#if LIBAVCODEC_BUILD >= 4676
    { VLC_FOURCC( 'm', 'p', '2', 'v' ), CODEC_ID_MPEG2VIDEO },
#endif
    { VLC_FOURCC( 'm', 'p', '4', 'v'),  CODEC_ID_MPEG4 },
    { VLC_FOURCC( 'D', 'I', 'V', '1' ), CODEC_ID_MSMPEG4V1 },
    { VLC_FOURCC( 'D', 'I', 'V', '2' ), CODEC_ID_MSMPEG4V2 },
    { VLC_FOURCC( 'D', 'I', 'V', '3' ), CODEC_ID_MSMPEG4V3 },
    { VLC_FOURCC( 'H', '2', '6', '3' ), CODEC_ID_H263 },
    { VLC_FOURCC( 'I', '2', '6', '3' ), CODEC_ID_H263I },
    { VLC_FOURCC( 'h', 'u', 'f', 'f' ), CODEC_ID_HUFFYUV },
    { VLC_FOURCC( 'W', 'M', 'V', '1' ), CODEC_ID_WMV1 },
    { VLC_FOURCC( 'W', 'M', 'V', '2' ), CODEC_ID_WMV2 },
    { VLC_FOURCC( 'M', 'J', 'P', 'G' ), CODEC_ID_MJPEG },
    { VLC_FOURCC( 'm', 'j', 'p', 'b' ), CODEC_ID_MJPEGB },
    { VLC_FOURCC( 'd', 'v', 's', 'l' ), CODEC_ID_DVVIDEO },
    { VLC_FOURCC( 'S', 'V', 'Q', '1' ), CODEC_ID_SVQ1 },
#if LIBAVCODEC_BUILD >= 4666
    { VLC_FOURCC( 'S', 'V', 'Q', '3' ), CODEC_ID_SVQ3 },
#endif

    /* raw video code, only used for 'encoding' */
    { VLC_FOURCC( 'I', '4', '2', '0' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'I', '4', '2', '2' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'I', '4', '4', '4' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'R', 'V', '1', '5' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'R', 'V', '1', '6' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'R', 'V', '2', '4' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'R', 'V', '3', '2' ), CODEC_ID_RAWVIDEO },
    { VLC_FOURCC( 'Y', 'U', 'Y', '2' ), CODEC_ID_RAWVIDEO },

    { VLC_FOURCC(   0,   0,   0,   0 ), 0 }
};

static inline int get_ff_codec( vlc_fourcc_t i_fcc )
{
    int i;

    for( i = 0; fourcc_to_ff_code[i].i_fcc != 0; i++ )
    {
        if( fourcc_to_ff_code[i].i_fcc == i_fcc )
        {
            return fourcc_to_ff_code[i].i_ff_codec;
        }
    }

    return 0;
}

static inline int get_ff_chroma( vlc_fourcc_t i_chroma )
{
    switch( i_chroma )
    {
        case VLC_FOURCC( 'I', '4', '2', '0' ):
            return PIX_FMT_YUV420P;
        case VLC_FOURCC( 'I', '4', '2', '2' ):
            return PIX_FMT_YUV422P;
        case VLC_FOURCC( 'I', '4', '4', '4' ):
            return PIX_FMT_YUV444P;
        case VLC_FOURCC( 'R', 'V', '1', '5' ):
            return PIX_FMT_RGB555;
        case VLC_FOURCC( 'R', 'V', '1', '6' ):
            return PIX_FMT_RGB565;
        case VLC_FOURCC( 'R', 'V', '2', '4' ):
            return PIX_FMT_RGB24;
        case VLC_FOURCC( 'R', 'V', '3', '2' ):
            return PIX_FMT_RGBA32;
        case VLC_FOURCC( 'G', 'R', 'E', 'Y' ):
            return PIX_FMT_GRAY8;
        case VLC_FOURCC( 'Y', 'U', 'Y', '2' ):
            return PIX_FMT_YUV422;
        default:
            return 0;
    }
}

static int transcode_audio_ffmpeg_new( sout_stream_t *p_stream,
                                       sout_stream_id_t *id )
{
    int i_ff_codec;

    if( id->f_src.i_fourcc == VLC_FOURCC('s','1','6','l') ||
        id->f_src.i_fourcc == VLC_FOURCC('s','1','6','b') ||
        id->f_src.i_fourcc == VLC_FOURCC('s','8',' ',' ') ||
        id->f_src.i_fourcc == VLC_FOURCC('u','8',' ',' ') )
    {
        id->ff_dec = NULL;

        id->ff_dec_c = avcodec_alloc_context();
        id->ff_dec_c->sample_rate = id->f_src.i_sample_rate;
        id->ff_dec_c->channels    = id->f_src.i_channels;
        id->ff_dec_c->block_align = id->f_src.i_block_align;
        id->ff_dec_c->bit_rate    = id->f_src.i_bitrate;
    }
    else
    {
        /* find decoder */
        i_ff_codec = get_ff_codec( id->f_src.i_fourcc );
        if( i_ff_codec == 0 )
        {
            msg_Err( p_stream, "cannot find decoder id" );
            return VLC_EGENERIC;
        }

        id->ff_dec = avcodec_find_decoder( i_ff_codec );
        if( !id->ff_dec )
        {
            msg_Err( p_stream, "cannot find decoder (avcodec)" );
            return VLC_EGENERIC;
        }

        id->ff_dec_c = avcodec_alloc_context();
        id->ff_dec_c->sample_rate = id->f_src.i_sample_rate;
        id->ff_dec_c->channels    = id->f_src.i_channels;
        id->ff_dec_c->block_align = id->f_src.i_block_align;
        id->ff_dec_c->bit_rate    = id->f_src.i_bitrate;

        id->ff_dec_c->extradata_size = id->f_src.i_extra_data;
        id->ff_dec_c->extradata      = id->f_src.p_extra_data;
        if( avcodec_open( id->ff_dec_c, id->ff_dec ) )
        {
            msg_Err( p_stream, "cannot open decoder" );
            return VLC_EGENERIC;
        }
    }

    /* find encoder */
    id->i_buffer_in      = 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
    id->i_buffer_in_pos = 0;
    id->p_buffer_in      = malloc( id->i_buffer_in );

    id->i_buffer     = 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
    id->i_buffer_pos = 0;
    id->p_buffer     = malloc( id->i_buffer );

    id->i_buffer_out     = 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE;
    id->i_buffer_out_pos = 0;
    id->p_buffer_out     = malloc( id->i_buffer_out );

    /* Sanity check for audio channels */
    id->f_dst.i_channels = __MIN( id->f_dst.i_channels, id->f_src.i_channels );

#ifdef HAVE_VORBIS_VORBISENC_H
    if( id->f_dst.i_fourcc == VLC_FOURCC('v','o','r','b') )
    {
        id->p_vi = (vorbis_info *)malloc( sizeof(vorbis_info) );
        id->p_vd = (vorbis_dsp_state *)malloc( sizeof(vorbis_dsp_state) );
        id->p_vb = (vorbis_block *)malloc( sizeof(vorbis_block) );
        id->p_vc = (vorbis_comment *)malloc( sizeof(vorbis_comment) );

        vorbis_info_init( id->p_vi );

        if( vorbis_encode_setup_managed( id->p_vi, id->f_dst.i_channels,
              id->f_dst.i_sample_rate, -1, id->f_dst.i_bitrate, -1 ) ||
            vorbis_encode_ctl( id->p_vi, OV_ECTL_RATEMANAGE_AVG, NULL ) ||
             vorbis_encode_setup_init( id->p_vi ) ){}

        /* add a comment */
        vorbis_comment_init( id->p_vc);
        vorbis_comment_add_tag( id->p_vc, "ENCODER", "VLC media player");

        /* set up the analysis state and auxiliary encoding storage */
        vorbis_analysis_init( id->p_vd, id->p_vi );
        vorbis_block_init( id->p_vd, id->p_vb );

        id->b_headers_sent = VLC_FALSE;
        id->i_last_block_size = 0;
        id->i_samples_delay = 0;

        return VLC_SUCCESS;
    }
#endif

    i_ff_codec = get_ff_codec( id->f_dst.i_fourcc );
    if( i_ff_codec == 0 )
    {
        msg_Err( p_stream, "cannot find encoder id" );
        return VLC_EGENERIC;
    }

    id->ff_enc = avcodec_find_encoder( i_ff_codec );
    if( !id->ff_enc )
    {
        msg_Err( p_stream, "cannot find encoder (avcodec)" );
        return VLC_EGENERIC;
    }

    /* Hack for mp3 transcoding support */
    if( id->f_dst.i_fourcc == VLC_FOURCC( 'm','p','3',' ' ) )
    {
        id->f_dst.i_fourcc = VLC_FOURCC( 'm','p','g','a' );
    }

    id->ff_enc_c = avcodec_alloc_context();
    id->ff_enc_c->bit_rate    = id->f_dst.i_bitrate;
    id->ff_enc_c->sample_rate = id->f_dst.i_sample_rate;
    id->ff_enc_c->channels    = id->f_dst.i_channels;

    if( avcodec_open( id->ff_enc_c, id->ff_enc ) )
    {
        if( id->ff_enc_c->channels > 2 )
        {
            id->ff_enc_c->channels = 2;
            id->f_dst.i_channels   = 2;
            if( avcodec_open( id->ff_enc_c, id->ff_enc ) )
            {
                msg_Err( p_stream, "cannot open encoder" );
                return VLC_EGENERIC;
            }
            msg_Warn( p_stream, "stereo mode selected (codec limitation)" );
        }
        else
        {
            msg_Err( p_stream, "cannot open encoder" );
            return VLC_EGENERIC;
        }
    }

    return VLC_SUCCESS;
}

static void transcode_audio_ffmpeg_close( sout_stream_t *p_stream,
                                          sout_stream_id_t *id )
{
    if( id->ff_dec )
    {
        avcodec_close( id->ff_dec_c );
    }

#ifdef HAVE_VORBIS_VORBISENC_H
    if( id->f_dst.i_fourcc == VLC_FOURCC('v','o','r','b') )
    {
        vorbis_block_clear( id->p_vb );
        vorbis_dsp_clear( id->p_vd );
        vorbis_comment_clear( id->p_vc );
        vorbis_info_clear( id->p_vi );  /* must be called last */

        free( id->p_vi );
        free( id->p_vd );
        free( id->p_vb );
        free( id->p_vc );
    }
    else
#endif
        avcodec_close( id->ff_enc_c );

    free( id->ff_dec_c );
    if( id->f_dst.i_fourcc != VLC_FOURCC('v','o','r','b') )
        free( id->ff_enc_c );

    free( id->p_buffer_in );
    free( id->p_buffer );
    free( id->p_buffer_out );
}

static int transcode_audio_ffmpeg_process( sout_stream_t *p_stream,
                                           sout_stream_id_t *id,
                                           sout_buffer_t *in,
                                           sout_buffer_t **out )
{
    vlc_bool_t b_again = VLC_FALSE;

    *out = NULL;

    /* gather data into p_buffer_in */
#ifdef HAVE_VORBIS_VORBISENC_H
    if( id->f_dst.i_fourcc == VLC_FOURCC( 'v', 'o', 'r', 'b' ) )
    id->i_dts = in->i_dts -
                (mtime_t)1000000 * (mtime_t)id->i_samples_delay /
                (mtime_t)id->f_dst.i_sample_rate;
    else
#endif
    id->i_dts = in->i_dts -
                (mtime_t)1000000 *
                (mtime_t)(id->i_buffer_pos / 2 / id->ff_dec_c->channels )/
                (mtime_t)id->ff_dec_c->sample_rate;

    if( id->i_buffer_in_pos + (int)in->i_size > id->i_buffer_in )
    {
        /* extend buffer_in */
        id->i_buffer_in = id->i_buffer_in_pos + in->i_size + 1024;
        id->p_buffer_in = realloc( id->p_buffer_in, id->i_buffer_in );
    }
    memcpy( &id->p_buffer_in[id->i_buffer_in_pos],
            in->p_buffer, in->i_size );
    id->i_buffer_in_pos += in->i_size;

    do
    {
        int i_buffer_pos;

        /* decode as much data as possible */
        if( id->ff_dec )
        {
            for( ;; )
            {
                int i_buffer_size;
                int i_used;

                i_buffer_size = id->i_buffer - id->i_buffer_pos;

                i_used = avcodec_decode_audio( id->ff_dec_c,
                         (int16_t*)&id->p_buffer[id->i_buffer_pos],
                         &i_buffer_size, id->p_buffer_in,
                         id->i_buffer_in_pos );

                /* msg_Warn( p_stream, "avcodec_decode_audio: %d used",
                             i_used ); */
                id->i_buffer_pos += i_buffer_size;

                if( i_used < 0 )
                {
                    msg_Warn( p_stream, "error");
                    id->i_buffer_in_pos = 0;
                    break;
                }
                else if( i_used < id->i_buffer_in_pos )
                {
                    memmove( id->p_buffer_in,
                             &id->p_buffer_in[i_used],
                             id->i_buffer_in - i_used );
                    id->i_buffer_in_pos -= i_used;
                }
                else
                {
                    id->i_buffer_in_pos = 0;
                    break;
                }

                if( id->i_buffer_pos >= AVCODEC_MAX_AUDIO_FRAME_SIZE )
                {
                    /* buffer full */
                    b_again = VLC_TRUE;
                    break;
                }
            }
        }
        else
        {
            int16_t *sout  = (int16_t*)&id->p_buffer[id->i_buffer_pos];
            int     i_used = 0;

            if( id->f_src.i_fourcc == VLC_FOURCC( 's', '8', ' ', ' ' ) )
            {
                int8_t *sin = (int8_t*)id->p_buffer_in;
                int     i_samples = __MIN( ( id->i_buffer - id->i_buffer_pos )
                                           / 2, id->i_buffer_in_pos );
                i_used = i_samples;
                while( i_samples > 0 )
                {
                    *sout++ = ( *sin++ ) << 8;
                    i_samples--;
                }
            }
            else if( id->f_src.i_fourcc == VLC_FOURCC( 'u', '8', ' ', ' ' ) )
            {
                int8_t *sin = (int8_t*)id->p_buffer_in;
                int     i_samples = __MIN( ( id->i_buffer - id->i_buffer_pos )
                                           / 2, id->i_buffer_in_pos );
                i_used = i_samples;
                while( i_samples > 0 )
                {
                    *sout++ = ( *sin++ - 128 ) << 8;
                    i_samples--;
                }
            }
            else if( id->f_src.i_fourcc == VLC_FOURCC( 's', '1', '6', 'l' ) )
            {
                int     i_samples = __MIN( ( id->i_buffer - id->i_buffer_pos )
                                           / 2, id->i_buffer_in_pos / 2);
#ifdef WORDS_BIGENDIAN
                uint8_t *sin = (uint8_t*)id->p_buffer_in;
                i_used = i_samples * 2;
                while( i_samples > 0 )
                {
                    uint8_t tmp[2];

                    tmp[1] = *sin++;
                    tmp[0] = *sin++;
                    *sout++ = *(int16_t*)tmp;
                    i_samples--;
                }

#else
                memcpy( sout, id->p_buffer_in, i_samples * 2 );
                sout += i_samples;
                i_used = i_samples * 2;
#endif
            }
            else if( id->f_src.i_fourcc == VLC_FOURCC( 's', '1', '6', 'b' ) )
            {
                int     i_samples = __MIN( ( id->i_buffer - id->i_buffer_pos )
                                           / 2, id->i_buffer_in_pos / 2);
#ifdef WORDS_BIGENDIAN
                memcpy( sout, id->p_buffer_in, i_samples * 2 );
                sout += i_samples;
                i_used = i_samples * 2;
#else
                uint8_t *sin = (uint8_t*)id->p_buffer_in;
                i_used = i_samples * 2;
                while( i_samples > 0 )
                {
                    uint8_t tmp[2];

                    tmp[1] = *sin++;
                    tmp[0] = *sin++;
                    *sout++ = *(int16_t*)tmp;
                    i_samples--;
                }
#endif
            }

            id->i_buffer_pos = (uint8_t*)sout - id->p_buffer;
            if( i_used < id->i_buffer_in_pos )
            {
                memmove( id->p_buffer_in,
                         &id->p_buffer_in[i_used],
                         id->i_buffer_in - i_used );
            }
            id->i_buffer_in_pos -= i_used;
        }

        i_buffer_pos = id->i_buffer_pos;

        /* encode as much data as possible */

#ifdef HAVE_VORBIS_VORBISENC_H
        if( id->i_buffer_pos == 0 );
        else if( id->f_dst.i_fourcc == VLC_FOURCC( 'v', 'o', 'r', 'b' ) )
        {
            float **buffer;
            int i, j, i_samples;
            sout_buffer_t *p_out;
            ogg_packet op;

            if( !id->b_headers_sent )
            {
                ogg_packet header[3];
                vorbis_analysis_headerout( id->p_vd, id->p_vc,
                                           &header[0], &header[1], &header[2]);
                for( i = 0; i < 3; i++ )
                {
                    p_out = sout_BufferNew( p_stream->p_sout, header[i].bytes);
                    memcpy( p_out->p_buffer, header[i].packet,
                            header[i].bytes );

                    p_out->i_size = header[i].bytes;
                    p_out->i_length = 0;

                    p_out->i_dts = p_out->i_pts = 0;

                    sout_BufferChain( out, p_out );
                }
                id->b_headers_sent = VLC_TRUE;
            }

            i_samples = id->i_buffer_pos / id->f_src.i_channels / 2;
            id->i_samples_delay += i_samples;
            id->i_buffer_pos = 0;

            buffer = vorbis_analysis_buffer( id->p_vd, i_samples );

            /* convert samples to float and uninterleave */
            for( i = 0; i < id->f_dst.i_channels; i++ )
            {
                for( j = 0 ; j < i_samples ; j++ )
                {
                    buffer[i][j]= ((float)( ((int16_t *)id->p_buffer)
                                  [j*id->f_src.i_channels + i ] ))/ 32768.f;
                }
            }

            vorbis_analysis_wrote( id->p_vd, i_samples );

            while( vorbis_analysis_blockout( id->p_vd, id->p_vb ) == 1 )
            {
                vorbis_analysis( id->p_vb, NULL );
                vorbis_bitrate_addblock( id->p_vb );

                while( vorbis_bitrate_flushpacket( id->p_vd, &op ) )
                {
                    int i_block_size;
                    p_out = sout_BufferNew( p_stream->p_sout, op.bytes );
                    memcpy( p_out->p_buffer, op.packet, op.bytes );

                    i_block_size = vorbis_packet_blocksize( id->p_vi, &op );

                    if( i_block_size < 0 ) i_block_size = 0;
                    i_samples = ( id->i_last_block_size +
                                  i_block_size ) >> 2;
                    id->i_last_block_size = i_block_size;

                    p_out->i_size = op.bytes;
                    p_out->i_length = (mtime_t)1000000 *
                      (mtime_t)i_samples /
                      (mtime_t)id->f_dst.i_sample_rate;

                    //msg_Err( p_stream, "i_dts: %lld", id->i_dts );

                    /* FIXME */
                    p_out->i_dts = id->i_dts;
                    p_out->i_pts = id->i_dts;

                    id->i_samples_delay -= i_samples;

                    /* update dts */
                    id->i_dts += p_out->i_length;
                    sout_BufferChain( out, p_out );

                }
            }
        }
        else
#endif

        for( ;; )
        {
            int i_frame_size = id->ff_enc_c->frame_size * 2 *
                                 id->ff_dec_c->channels;
            int i_out_size, i, j;
            sout_buffer_t *p_out;
            int16_t *p_buffer = (int16_t *)(id->p_buffer + i_buffer_pos -
                                            id->i_buffer_pos);

            if( id->i_buffer_pos < i_frame_size )
            {
                break;
            }

            if( id->ff_dec_c->channels != id->ff_enc_c->channels )
            {
                /* dumb downmixing */
                for( i = 0; i < id->ff_enc_c->frame_size; i++ )
                {
                    for( j = 0 ; j < id->f_dst.i_channels; j++ )
                    {
                        p_buffer[i*id->f_dst.i_channels+j] = p_buffer[i*id->f_src.i_channels+j];
                    }
                }
            }

            /* msg_Warn( p_stream, "avcodec_encode_audio: frame size%d",
                         i_frame_size); */
            i_out_size = avcodec_encode_audio( id->ff_enc_c,
                           id->p_buffer_out, id->i_buffer_out, p_buffer );

            if( i_out_size <= 0 )
            {
                break;
            }

            id->i_buffer_pos -= i_frame_size;

            p_out = sout_BufferNew( p_stream->p_sout, i_out_size );
            memcpy( p_out->p_buffer, id->p_buffer_out, i_out_size );
            p_out->i_size = i_out_size;
            p_out->i_length = (mtime_t)1000000 *
                              (mtime_t)id->ff_enc_c->frame_size /
                              (mtime_t)id->ff_enc_c->sample_rate;

            /* FIXME */
            p_out->i_dts = id->i_dts;
            p_out->i_pts = id->i_dts;

            /* update dts */
            id->i_dts += p_out->i_length;

           /* msg_Warn( p_stream, "frame dts=%lld len %lld out=%d",
                        p_out->i_dts, p_out->i_length, i_out_size ); */

            sout_BufferChain( out, p_out );
        }

        /* Copy the remaining raw samples */
        if( id->i_buffer_pos != 0 )
        {
            memmove( id->p_buffer,
                     &id->p_buffer[i_buffer_pos - id->i_buffer_pos],
                     id->i_buffer_pos );
        }

    } while( b_again );

    return VLC_SUCCESS;
}


/*
 * video
 */
static int transcode_video_ffmpeg_new( sout_stream_t *p_stream,
                                       sout_stream_id_t *id )
{
    sout_stream_sys_t   *p_sys = p_stream->p_sys;

    int i_ff_codec;

    if( id->f_src.i_fourcc == VLC_FOURCC( 'I', '4', '2', '0' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'I', '4', '2', '2' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'I', '4', '4', '4' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'Y', 'U', 'Y', '2' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'R', 'V', '1', '5' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'R', 'V', '1', '6' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'R', 'V', '2', '4' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'R', 'V', '3', '2' ) ||
        id->f_src.i_fourcc == VLC_FOURCC( 'G', 'R', 'E', 'Y' ) )
    {
        id->ff_dec   = NULL;
        id->ff_dec_c            = avcodec_alloc_context();
        id->ff_dec_c->width     = id->f_src.i_width;
        id->ff_dec_c->height    = id->f_src.i_height;
        id->ff_dec_c->pix_fmt   = get_ff_chroma( id->f_src.i_fourcc );
    }
    else
    {
        /* find decoder */
        i_ff_codec = get_ff_codec( id->f_src.i_fourcc );
        if( i_ff_codec == 0 )
        {
            msg_Err( p_stream, "cannot find decoder" );
            return VLC_EGENERIC;
        }

        id->ff_dec = avcodec_find_decoder( i_ff_codec );
        if( !id->ff_dec )
        {
            msg_Err( p_stream, "cannot find decoder" );
            return VLC_EGENERIC;
        }

        id->ff_dec_c = avcodec_alloc_context();
        id->ff_dec_c->width         = id->f_src.i_width;
        id->ff_dec_c->height        = id->f_src.i_height;
        /* id->ff_dec_c->bit_rate      = id->f_src.i_bitrate; */
        id->ff_dec_c->extradata_size= id->f_src.i_extra_data;
        id->ff_dec_c->extradata     = id->f_src.p_extra_data;
        id->ff_dec_c->workaround_bugs = FF_BUG_AUTODETECT;
        id->ff_dec_c->error_resilience= -1;
        id->ff_dec_c->get_buffer    = transcode_video_ffmpeg_getframebuf;
        id->ff_dec_c->opaque        = p_sys;

        if( avcodec_open( id->ff_dec_c, id->ff_dec ) < 0 )
        {
            msg_Err( p_stream, "cannot open decoder" );
            return VLC_EGENERIC;
        }

        if( i_ff_codec == CODEC_ID_MPEG4 && id->ff_dec_c->extradata_size > 0 )
        {
            int b_gotpicture;
            AVFrame frame;
            uint8_t *p_vol = malloc( id->ff_dec_c->extradata_size +
                                     FF_INPUT_BUFFER_PADDING_SIZE );

            memcpy( p_vol, id->ff_dec_c->extradata,
                    id->ff_dec_c->extradata_size );
            memset( p_vol + id->ff_dec_c->extradata_size, 0,
                    FF_INPUT_BUFFER_PADDING_SIZE );

            avcodec_decode_video( id->ff_dec_c, &frame, &b_gotpicture,
                                  id->ff_dec_c->extradata,
                                  id->ff_dec_c->extradata_size );
            free( p_vol );
        }
    }


    /* find encoder */
    i_ff_codec = get_ff_codec( id->f_dst.i_fourcc );
    if( i_ff_codec == 0 )
    {
        msg_Err( p_stream, "cannot find encoder" );
        return VLC_EGENERIC;
    }

    id->ff_enc = avcodec_find_encoder( i_ff_codec );
    if( !id->ff_enc )
    {
        msg_Err( p_stream, "cannot find encoder" );
        return VLC_EGENERIC;
    }

    if( id->f_dst.i_fourcc == VLC_FOURCC( 'm','p','1','v' )||
        id->f_dst.i_fourcc == VLC_FOURCC( 'm','p','2','v' ) )
    {
        id->f_dst.i_fourcc = VLC_FOURCC( 'm','p','g','v' );
    }

    id->ff_enc_c = avcodec_alloc_context();
    id->ff_enc_c->width          = id->f_dst.i_width;
    id->ff_enc_c->height         = id->f_dst.i_height;
    id->ff_enc_c->bit_rate       = id->f_dst.i_bitrate;

    if( id->ff_dec )
    {
        id->ff_enc_c->frame_rate     = id->ff_dec_c->frame_rate;
#if LIBAVCODEC_BUILD >= 4662
        id->ff_enc_c->frame_rate_base= id->ff_dec_c->frame_rate_base;
#endif
    }
    else
    {
#if LIBAVCODEC_BUILD >= 4662
        id->ff_enc_c->frame_rate     = 25 ; /* FIXME as it break mpeg */
        id->ff_enc_c->frame_rate_base= 1;
#else
        id->ff_enc_c->frame_rate     = 25 * FRAME_RATE_BASE;
#endif
    }

    id->ff_enc_c->gop_size       = p_sys->i_key_int >= 0 ? p_sys->i_key_int : 50;
    id->ff_enc_c->max_b_frames   = __MIN( p_sys->i_b_frames, FF_MAX_B_FRAMES );
    id->ff_enc_c->b_frame_strategy = 0;
    id->ff_enc_c->b_quant_factor = 2.0;

    if( p_sys->i_vtolerance >= 0 )
    {
        id->ff_enc_c->bit_rate_tolerance = p_sys->i_vtolerance;
    }
    id->ff_enc_c->qmin           = p_sys->i_qmin;
    id->ff_enc_c->qmax           = p_sys->i_qmax;

#if LIBAVCODEC_BUILD >= 4673
    id->ff_enc_c->mb_decision = p_sys->i_hq;
#else
    if( p_sys->i_hq )
    {
        id->ff_enc_c->flags      |= CODEC_FLAG_HQ;
    }
#endif

    if( i_ff_codec == CODEC_ID_RAWVIDEO )
    {
        id->ff_enc_c->pix_fmt = get_ff_chroma( id->f_dst.i_fourcc );
    }
    /* XXX open it only when we have the first frame */
    id->b_enc_inited     = VLC_FALSE;
    id->i_buffer_in      = 0;
    id->i_buffer_in_pos  = 0;
    id->p_buffer_in      = NULL;

    id->i_buffer     = 3*1024*1024;
    id->i_buffer_pos = 0;
    id->p_buffer     = malloc( id->i_buffer );

    id->i_buffer_out     = 0;
    id->i_buffer_out_pos = 0;
    id->p_buffer_out     = NULL;

    id->p_ff_pic         = avcodec_alloc_frame();
    id->p_ff_pic_tmp0    = NULL;
    id->p_ff_pic_tmp1    = NULL;
    id->p_ff_pic_tmp2    = NULL;
    id->p_vresample      = NULL;

    p_sys->i_last_ref_pts = 0;
    p_sys->i_buggy_pts_detect = 0;

    return VLC_SUCCESS;
}

static void transcode_video_ffmpeg_close ( sout_stream_t *p_stream, sout_stream_id_t *id )
{
    if( id->ff_dec )
    {
        avcodec_close( id->ff_dec_c );
    }
    if( id->b_enc_inited )
    {
        avcodec_close( id->ff_enc_c );
    }

    if( id->p_ff_pic)
    {
        free( id->p_ff_pic );
    }

    if( id->p_ff_pic_tmp0 )
    {
        free( id->p_ff_pic_tmp0->data[0] );
        free( id->p_ff_pic_tmp0 );
    }
    if( id->p_ff_pic_tmp1)
    {
        free( id->p_ff_pic_tmp1->data[0] );
        free( id->p_ff_pic_tmp1 );
    }
    if( id->p_ff_pic_tmp2)
    {
        free( id->p_ff_pic_tmp2->data[0] );
        free( id->p_ff_pic_tmp2 );
    }
    if( id->p_vresample )
    {
        free( id->p_vresample );
    }

    free( id->ff_dec_c );
    free( id->ff_enc_c );

    free( id->p_buffer );
}

static int transcode_video_ffmpeg_process( sout_stream_t *p_stream,
               sout_stream_id_t *id, sout_buffer_t *in, sout_buffer_t **out )
{
    sout_stream_sys_t   *p_sys = p_stream->p_sys;
    int     i_used;
    int     i_out;
    int     b_gotpicture;
    AVFrame *frame;

    int     i_data;
    uint8_t *p_data;

    *out = NULL;

    i_data = in->i_size;
    p_data = in->p_buffer;

    for( ;; )
    {
        /* decode frame */
        frame = id->p_ff_pic;
        p_sys->i_input_pts = in->i_pts;
        if( id->ff_dec )
        {
            i_used = avcodec_decode_video( id->ff_dec_c, frame,
                                           &b_gotpicture,
                                           p_data, i_data );
        }
        else
        {
            /* raw video */
            avpicture_fill( (AVPicture*)frame,
                            p_data,
                            id->ff_dec_c->pix_fmt,
                            id->ff_dec_c->width, id->ff_dec_c->height );
            i_used = i_data;
            b_gotpicture = 1;

            /* Set PTS */
            frame->pts = p_sys->i_input_pts;
        }

        if( i_used < 0 )
        {
            msg_Warn( p_stream, "error");
            return VLC_EGENERIC;
        }
        i_data -= i_used;
        p_data += i_used;

        if( !b_gotpicture )
        {
            return VLC_SUCCESS;
        }

        /* Get the pts of the decoded frame if any, otherwise keep the
         * interpolated one */
        if( frame->pts > 0 )
        {
            p_sys->i_output_pts = frame->pts;
        }

        if( !id->b_enc_inited )
        {
            /* XXX hack because of copy packetizer and mpeg4video that can failed
               detecting size */
            if( id->ff_enc_c->width <= 0 )
            {
                id->ff_enc_c->width    =
                    id->f_dst.i_width  = id->ff_dec_c->width - p_sys->i_crop_left - p_sys->i_crop_right;
            }
            if( id->ff_enc_c->height <= 0 )
            {
                id->ff_enc_c->height   =
                    id->f_dst.i_height = id->ff_dec_c->height - p_sys->i_crop_top - p_sys->i_crop_bottom;
            }

            if( avcodec_open( id->ff_enc_c, id->ff_enc ) )
            {
                msg_Err( p_stream, "cannot open encoder" );
                return VLC_EGENERIC;
            }

            if( !( id->id = p_stream->p_sys->p_out->pf_add( p_stream->p_sys->p_out, &id->f_dst ) ) )
            {
                msg_Err( p_stream, "cannot add this stream" );
                transcode_video_ffmpeg_close( p_stream, id );
                id->b_transcode = VLC_FALSE;
                return VLC_EGENERIC;
            }
            id->b_enc_inited = VLC_TRUE;
        }


        /* deinterlace */
        if( p_stream->p_sys->b_deinterlace )
        {
            if( id->p_ff_pic_tmp0 == NULL )
            {
                int     i_size;
                uint8_t *buf;
                id->p_ff_pic_tmp0 = avcodec_alloc_frame();
                i_size = avpicture_get_size( id->ff_dec_c->pix_fmt,
                                             id->ff_dec_c->width, id->ff_dec_c->height );

                buf = malloc( i_size );

                avpicture_fill( (AVPicture*)id->p_ff_pic_tmp0, buf,
                                id->ff_enc_c->pix_fmt,
                                id->ff_dec_c->width, id->ff_dec_c->height );
            }

            avpicture_deinterlace( (AVPicture*)id->p_ff_pic_tmp0, (AVPicture*)frame,
                                   id->ff_dec_c->pix_fmt,
                                   id->ff_dec_c->width, id->ff_dec_c->height );

            frame = id->p_ff_pic_tmp0;
        }

        /* convert pix format */
        if( id->ff_dec_c->pix_fmt != id->ff_enc_c->pix_fmt )
        {
            if( id->p_ff_pic_tmp1 == NULL )
            {
                int     i_size;
                uint8_t *buf;
                id->p_ff_pic_tmp1 = avcodec_alloc_frame();
                i_size = avpicture_get_size( id->ff_enc_c->pix_fmt,
                                             id->ff_dec_c->width, id->ff_dec_c->height );

                buf = malloc( i_size );

                avpicture_fill( (AVPicture*)id->p_ff_pic_tmp1, buf,
                                id->ff_enc_c->pix_fmt,
                                id->ff_dec_c->width, id->ff_dec_c->height );
            }

            img_convert( (AVPicture*)id->p_ff_pic_tmp1, id->ff_enc_c->pix_fmt,
                         (AVPicture*)frame,             id->ff_dec_c->pix_fmt,
                         id->ff_dec_c->width, id->ff_dec_c->height );

            frame = id->p_ff_pic_tmp1;
        }

        /* convert size and crop */
        if( id->ff_dec_c->width  != id->ff_enc_c->width ||
            id->ff_dec_c->height != id->ff_enc_c->height ||
            p_sys->i_crop_top > 0 || p_sys->i_crop_bottom > 0 ||
            p_sys->i_crop_left > 0 || p_sys->i_crop_right )
        {
            if( id->p_ff_pic_tmp2 == NULL )
            {
                int     i_size;
                uint8_t *buf;
                id->p_ff_pic_tmp2 = avcodec_alloc_frame();
                i_size = avpicture_get_size( id->ff_enc_c->pix_fmt,
                                             id->ff_enc_c->width, id->ff_enc_c->height );

                buf = malloc( i_size );

                avpicture_fill( (AVPicture*)id->p_ff_pic_tmp2, buf,
                                id->ff_enc_c->pix_fmt,
                                id->ff_enc_c->width, id->ff_enc_c->height );

                id->p_vresample =
                    img_resample_full_init( id->ff_enc_c->width, id->ff_enc_c->height,
                                            id->ff_dec_c->width, id->ff_dec_c->height,
                                            p_stream->p_sys->i_crop_top,
                                            p_stream->p_sys->i_crop_bottom,
                                            p_stream->p_sys->i_crop_left,
                                            p_stream->p_sys->i_crop_right );
            }

            img_resample( id->p_vresample, (AVPicture*)id->p_ff_pic_tmp2,
                          (AVPicture*)frame );

            frame = id->p_ff_pic_tmp2;
        }

        /* Set the pts of the frame being encoded (segfaults with mpeg4!)*/
        if( id->f_dst.i_fourcc == VLC_FOURCC( 'm', 'p', 'g', 'v' ) )
            frame->pts = p_sys->i_output_pts;
        else
            frame->pts = 0;

        /* Interpolate the next PTS
         * (needed by the mpeg video packetizer which can send pts <= 0 ) */
        if( id->ff_dec_c && id->ff_dec_c->frame_rate > 0 )
        {
            p_sys->i_output_pts += I64C(1000000) * (2 + frame->repeat_pict) *
              id->ff_dec_c->frame_rate_base / (2 * id->ff_dec_c->frame_rate);
        }

        /* Let ffmpeg select the frame type */
        frame->pict_type = 0;

        i_out = avcodec_encode_video( id->ff_enc_c, id->p_buffer,
                                      id->i_buffer, frame );
        if( i_out > 0 )
        {
            sout_buffer_t *p_out;
            p_out = sout_BufferNew( p_stream->p_sout, i_out );

            memcpy( p_out->p_buffer, id->p_buffer, i_out );

            p_out->i_size   = i_out;

            if( id->ff_enc_c->coded_frame->pts != 0 &&
                p_sys->i_buggy_pts_detect != id->ff_enc_c->coded_frame->pts )
            {
                p_sys->i_buggy_pts_detect = id->ff_enc_c->coded_frame->pts;

                /* FIXME, 3-2 pulldown is not handled correctly */
                p_out->i_length = in->i_length;
                p_out->i_pts    = id->ff_enc_c->coded_frame->pts;

                if( !id->ff_enc_c->delay ||
                    ( id->ff_enc_c->coded_frame->pict_type != FF_I_TYPE &&
                      id->ff_enc_c->coded_frame->pict_type != FF_P_TYPE ) )
                {
                    p_out->i_dts    = p_out->i_pts;
                }
                else
                {
                    if( p_sys->i_last_ref_pts )
                    {
                        p_out->i_dts = p_sys->i_last_ref_pts;
                    }
                    else
                    {
                        /* Let's put something sensible */
                        p_out->i_dts = p_out->i_pts;
                    }

                    p_sys->i_last_ref_pts = p_out->i_pts;
                }
            }
            else
            {
                /* Buggy libavcodec which doesn't update coded_frame->pts
                 * correctly */
                p_out->i_length = in->i_length;
                p_out->i_dts    = in->i_dts;
                p_out->i_pts    = in->i_dts;
            }

            sout_BufferChain( out, p_out );
        }

        if( i_data <= 0 )
        {
            return VLC_SUCCESS;
        }
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * transcode_video_ffmpeg_getframebuf:
 *
 * Callback used by ffmpeg to get a frame buffer.
 * We use it to get the right PTS for each decoded picture.
 *****************************************************************************/
static int transcode_video_ffmpeg_getframebuf(struct AVCodecContext *p_context,
                                              AVFrame *p_frame)
{
    sout_stream_sys_t *p_sys = (sout_stream_sys_t *)p_context->opaque;

    /* Set PTS */
    p_frame->pts = p_sys->i_input_pts;

    return avcodec_default_get_buffer( p_context, p_frame );
}
