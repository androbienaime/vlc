/*****************************************************************************
 * es.c : Generic audio ES input module for vlc
 *****************************************************************************
 * Copyright (C) 2001-2008 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@videolan.org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_codec.h>
#include <vlc_codecs.h>
#include <vlc_input.h>

#include "../../codec/a52.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )
    set_description( N_("MPEG-I/II/4 / A52 / DTS / MLP audio" ) )
    set_capability( "demux", 155 )
    set_callbacks( Open, Close )

    add_shortcut( "mpga" )
    add_shortcut( "mp3" )

    add_shortcut( "m4a" )
    add_shortcut( "mp4a" )
    add_shortcut( "aac" )

    add_shortcut( "ac3" )
    add_shortcut( "a52" )

    add_shortcut( "eac3" )

    add_shortcut( "dts" )

    add_shortcut( "mlp" )
    add_shortcut( "thd" )
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Demux  ( demux_t * );
static int Control( demux_t *, int, va_list );

typedef struct
{
    vlc_fourcc_t i_codec;
    bool       b_use_word;
    const char *psz_name;
    int  (*pf_probe)( demux_t *p_demux, int64_t *pi_offset );
    int  (*pf_init)( demux_t *p_demux );
} codec_t;

struct demux_sys_t
{
    codec_t codec;

    es_out_id_t *p_es;

    bool  b_start;
    decoder_t   *p_packetizer;

    mtime_t     i_pts;
    mtime_t     i_time_offset;
    int64_t     i_bytes;

    bool        b_big_endian;
    bool        b_estimate_bitrate;
    int         i_bitrate_avg;  /* extracted from Xing header */

    bool b_initial_sync_failed;

    int i_packet_size;

    int64_t i_stream_offset;

    /* Mpga specific */
    struct
    {
        int i_frames;
        int i_bytes;
        int i_bitrate_avg;
        int i_frame_samples;
    } xing;
};

static int MpgaProbe( demux_t *p_demux, int64_t *pi_offset );
static int MpgaInit( demux_t *p_demux );

static int AacProbe( demux_t *p_demux, int64_t *pi_offset );
static int AacInit( demux_t *p_demux );

static int EA52Probe( demux_t *p_demux, int64_t *pi_offset );
static int A52Probe( demux_t *p_demux, int64_t *pi_offset );
static int A52Init( demux_t *p_demux );

static int DtsProbe( demux_t *p_demux, int64_t *pi_offset );
static int DtsInit( demux_t *p_demux );

static int MlpProbe( demux_t *p_demux, int64_t *pi_offset );
static int MlpInit( demux_t *p_demux );

static const codec_t p_codec[] = {
    { VLC_CODEC_MP4A, false, "mp4 audio",  AacProbe,  AacInit },
    { VLC_CODEC_MPGA, false, "mpeg audio", MpgaProbe, MpgaInit },
    { VLC_CODEC_A52, true,  "a52 audio",  A52Probe,  A52Init },
    { VLC_CODEC_EAC3, true,  "eac3 audio", EA52Probe, A52Init },
    { VLC_CODEC_DTS, false, "dts audio",  DtsProbe,  DtsInit },
    { VLC_CODEC_TRUEHD, false, "mlp audio",  MlpProbe,  MlpInit },

    { 0, false, NULL, NULL, NULL }
};

/*****************************************************************************
 * Open: initializes demux structures
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys;

    es_format_t fmt;
    int64_t i_offset;
    int i_index;

    for( i_index = 0; p_codec[i_index].i_codec != 0; i_index++ )
    {
        if( !p_codec[i_index].pf_probe( p_demux, &i_offset ) )
            break;
    }

    if( p_codec[i_index].i_codec == 0 )
        return VLC_EGENERIC;

    DEMUX_INIT_COMMON(); p_sys = p_demux->p_sys;
    memset( p_sys, 0, sizeof( demux_sys_t ) );
    p_sys->codec = p_codec[i_index];
    p_sys->p_es = NULL;
    p_sys->b_start = true;
    p_sys->i_stream_offset = i_offset;
    p_sys->b_estimate_bitrate = true;
    p_sys->i_bitrate_avg = 0;
    p_sys->b_big_endian = false;

    if( stream_Seek( p_demux->s, p_sys->i_stream_offset ) )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    if( p_sys->codec.pf_init( p_demux ) )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    msg_Dbg( p_demux, "detected format %4.4s", (const char*)&p_sys->codec.i_codec );

    /* Load the audio packetizer */
    es_format_Init( &fmt, AUDIO_ES, p_sys->codec.i_codec );
    p_sys->p_packetizer = demux_PacketizerNew( p_demux, &fmt, p_sys->codec.psz_name );
    if( !p_sys->p_packetizer )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Demux: reads and demuxes data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, 1 otherwise
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    block_t *p_block_in, *p_block_out;

    if( p_sys->codec.b_use_word )
    {
        /* Make sure we are word aligned */
        int64_t i_pos = stream_Tell( p_demux->s );
        if( i_pos % 2 )
            stream_Read( p_demux->s, NULL, 1 );
    }

    if( ( p_block_in = stream_Block( p_demux->s, p_sys->i_packet_size ) ) == NULL )
        return 0;

    if( p_sys->codec.b_use_word && !p_sys->b_big_endian && p_block_in->i_buffer > 0 )
    {
        /* Convert to big endian */
        swab( p_block_in->p_buffer, p_block_in->p_buffer, p_block_in->i_buffer );
    }

    p_block_in->i_pts = p_block_in->i_dts = p_sys->b_start || p_sys->b_initial_sync_failed ? 1 : 0;
    p_sys->b_initial_sync_failed = p_sys->b_start; /* Only try to resync once */

    while( ( p_block_out = p_sys->p_packetizer->pf_packetize( p_sys->p_packetizer, &p_block_in ) ) )
    {
        p_sys->b_initial_sync_failed = false;
        while( p_block_out )
        {
            block_t *p_next = p_block_out->p_next;

            if( !p_sys->p_es )
            {
                p_sys->p_packetizer->fmt_out.b_packetized = true;
                p_sys->p_es = es_out_Add( p_demux->out,
                                          &p_sys->p_packetizer->fmt_out);


                /* Try the xing header */
                if( p_sys->xing.i_bytes && p_sys->xing.i_frames &&
                    p_sys->xing.i_frame_samples )
                {
                    p_sys->i_bitrate_avg = p_sys->xing.i_bytes * INT64_C(8) *
                        p_sys->p_packetizer->fmt_out.audio.i_rate /
                        p_sys->xing.i_frames / p_sys->xing.i_frame_samples;

                    if( p_sys->i_bitrate_avg > 0 )
                        p_sys->b_estimate_bitrate = false;
                }
                /* Use the bitrate as initual value */
                if( p_sys->b_estimate_bitrate )
                    p_sys->i_bitrate_avg = p_sys->p_packetizer->fmt_out.i_bitrate;
            }

            p_sys->i_pts = p_block_out->i_pts;

            /* Re-estimate bitrate */
            if( p_sys->b_estimate_bitrate && p_sys->i_pts > 1 + INT64_C(500000) )
                p_sys->i_bitrate_avg = 8*INT64_C(1000000)*p_sys->i_bytes/(p_sys->i_pts-1);
            p_sys->i_bytes += p_block_out->i_buffer;

            /* Correct timestamp */
            p_block_out->i_pts += p_sys->i_time_offset;
            p_block_out->i_dts += p_sys->i_time_offset;

            es_out_Control( p_demux->out, ES_OUT_SET_PCR, p_block_out->i_dts );

            es_out_Send( p_demux->out, p_sys->p_es, p_block_out );

            p_block_out = p_next;
        }
    }

    if( p_sys->b_initial_sync_failed )
        msg_Dbg( p_demux, "did not sync on first block" );
    p_sys->b_start = false;
    return 1;
}

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;

    demux_PacketizerDestroy( p_sys->p_packetizer );
    free( p_sys );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys  = p_demux->p_sys;
    int64_t *pi64;
    bool *pb_bool;
    int i_ret;
    va_list args_save;

    va_copy ( args_save, args );

    switch( i_query )
    {
        case DEMUX_HAS_UNSUPPORTED_META:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            pi64 = (int64_t*)va_arg( args, int64_t * );
            *pi64 = p_sys->i_pts + p_sys->i_time_offset;
            return VLC_SUCCESS;

        case DEMUX_GET_LENGTH:
            i_ret = demux_vaControlHelper( p_demux->s, p_sys->i_stream_offset, -1,
                                            p_sys->i_bitrate_avg, 1, i_query,
                                            args );
            /* No bitrate, we can't have it precisely, but we can compute
             * a raw approximation with time/position */
            if( i_ret && !p_sys->i_bitrate_avg )
            {
                float f_pos = (double)(uint64_t)( stream_Tell( p_demux->s ) ) /
                              (double)(uint64_t)( stream_Size( p_demux->s ) );
                /* The first few seconds are guaranteed to be very whacky,
                 * don't bother trying ... Too bad */
                if( f_pos < 0.01 ||
                    (p_sys->i_pts + p_sys->i_time_offset) < 8000000 )
                    return VLC_EGENERIC;

                pi64 = (int64_t *)va_arg( args_save, int64_t * );
                *pi64 = (p_sys->i_pts + p_sys->i_time_offset) / f_pos;
                return VLC_SUCCESS;
            }
            va_end( args_save );
            return i_ret;

        case DEMUX_SET_TIME:
            /* FIXME TODO: implement a high precision seek (with mp3 parsing)
             * needed for multi-input */
        default:
            i_ret = demux_vaControlHelper( p_demux->s, p_sys->i_stream_offset, -1,
                                            p_sys->i_bitrate_avg, 1, i_query,
                                            args );
            if( !i_ret && p_sys->i_bitrate_avg > 0 &&
                (i_query == DEMUX_SET_POSITION || i_query == DEMUX_SET_TIME) )
            {
                int64_t i_time = INT64_C(8000000) * ( stream_Tell(p_demux->s) - p_sys->i_stream_offset ) /
                    p_sys->i_bitrate_avg;

                /* Fix time_offset */
                if( i_time >= 0 )
                    p_sys->i_time_offset = i_time - p_sys->i_pts;
            }
            return i_ret;
    }
}

/*****************************************************************************
 * Mpeg I/II Audio
 *****************************************************************************/
static int MpgaCheckSync( const uint8_t *p_peek )
{
    uint32_t h = GetDWBE( p_peek );

    if( ((( h >> 21 )&0x07FF) != 0x07FF )   /* header sync */
        || (((h >> 17)&0x03) == 0 )         /* valid layer ?*/
        || (((h >> 12)&0x0F) == 0x0F )
        || (((h >> 12)&0x0F) == 0x00 )      /* valid bitrate ? */
        || (((h >> 10) & 0x03) == 0x03 )    /* valide sampling freq ? */
        || ((h & 0x03) == 0x02 ))           /* valid emphasis ? */
    {
        return false;
    }
    return true;
}

#define MPGA_VERSION( h )   ( 1 - (((h)>>19)&0x01) )
#define MPGA_MODE(h)        (((h)>> 6)&0x03)

static int MpgaGetFrameSamples( uint32_t h )
{
    const int i_layer = 3 - (((h)>>17)&0x03);
    switch( i_layer )
    {
    case 0:
        return 384;
    case 1:
        return 1152;
    case 2:
        return MPGA_VERSION(h) ? 576 : 1152;
    default:
        return 0;
    }
}

static int MpgaProbe( demux_t *p_demux, int64_t *pi_offset )
{
    bool   b_forced;
    bool   b_forced_demux;
    int64_t i_offset;

    const uint8_t     *p_peek;

    b_forced = demux_IsPathExtension( p_demux, ".mp3" );
    b_forced_demux = demux_IsForced( p_demux, "mp3" ) ||
                     demux_IsForced( p_demux, "mpga" );

    i_offset = stream_Tell( p_demux->s );
    if( stream_Peek( p_demux->s, &p_peek, 4 ) < 4 )
        return VLC_EGENERIC;

    if( !MpgaCheckSync( p_peek ) )
    {
        bool b_ok = false;
        int i_peek;

        if( !b_forced_demux && !b_forced )
            return VLC_EGENERIC;

        i_peek = stream_Peek( p_demux->s, &p_peek, 8096 );
        while( i_peek > 4 )
        {
            if( MpgaCheckSync( p_peek ) )
            {
                b_ok = true;
                break;
            }
            p_peek += 1;
            i_peek -= 1;
            i_offset++;
        }
        if( !b_ok && !b_forced_demux )
            return VLC_EGENERIC;
    }
    *pi_offset = i_offset;
    return VLC_SUCCESS;
}

static void MpgaXingSkip( const uint8_t **pp_xing, int *pi_xing, int i_count )
{
    if(i_count > *pi_xing )
        i_count = *pi_xing;

    (*pp_xing) += i_count;
    (*pi_xing) -= i_count;
}

static uint32_t MpgaXingGetDWBE( const uint8_t **pp_xing, int *pi_xing, uint32_t i_default )
{
    if( *pi_xing < 4 )
        return i_default;

    uint32_t v = GetDWBE( *pp_xing );

    MpgaXingSkip( pp_xing, pi_xing, 4 );

    return v;
}

static int MpgaInit( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    const uint8_t *p_peek;
    int i_peek;

    /* */
    p_sys->i_packet_size = 1024;

    /* Load a potential xing header */
    i_peek = stream_Peek( p_demux->s, &p_peek, 4 + 1024 );
    if( i_peek < 4 + 21 )
        return VLC_SUCCESS;

    const uint32_t header = GetDWBE( p_peek );
    if( !MpgaCheckSync( p_peek ) )
        return VLC_SUCCESS;

    /* Xing header */
    const uint8_t *p_xing = p_peek;
    int i_xing = i_peek;
    int i_skip;

    if( MPGA_VERSION( header ) == 0 )
        i_skip = MPGA_MODE( header ) != 3 ? 36 : 21;
    else
        i_skip = MPGA_MODE( header ) != 3 ? 21 : 13;

    if( i_skip + 8 >= i_xing || memcmp( &p_xing[i_skip], "Xing", 4 ) )
        return VLC_SUCCESS;

    const uint32_t i_flags = GetDWBE( &p_xing[i_skip+4] );

    MpgaXingSkip( &p_xing, &i_xing, i_skip + 8 );

    if( i_flags&0x01 )
        p_sys->xing.i_frames = MpgaXingGetDWBE( &p_xing, &i_xing, 0 );
    if( i_flags&0x02 )
        p_sys->xing.i_bytes = MpgaXingGetDWBE( &p_xing, &i_xing, 0 );
    if( i_flags&0x04 ) /* TODO Support XING TOC to improve seeking accuracy */
        MpgaXingSkip( &p_xing, &i_xing, 100 );
    if( i_flags&0x08 )
    {
        /* FIXME: doesn't return the right bitrage average, at least
           with some MP3's */
        p_sys->xing.i_bitrate_avg = MpgaXingGetDWBE( &p_xing, &i_xing, 0 );
        msg_Dbg( p_demux, "xing vbr value present (%d)",
                 p_sys->xing.i_bitrate_avg );
    }

    if( p_sys->xing.i_frames > 0 && p_sys->xing.i_bytes > 0 )
    {
        p_sys->xing.i_frame_samples = MpgaGetFrameSamples( header );
        msg_Dbg( p_demux, "xing frames&bytes value present "
                 "(%d bytes, %d frames, %d samples/frame)",
                 p_sys->xing.i_bytes, p_sys->xing.i_frames,
                 p_sys->xing.i_frame_samples );
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * AAC
 *****************************************************************************/
static int AacProbe( demux_t *p_demux, int64_t *pi_offset )
{
    bool   b_forced;
    bool   b_forced_demux;

    int64_t i_offset;
    const uint8_t *p_peek;

    b_forced = demux_IsPathExtension( p_demux, ".aac" ) ||
               demux_IsPathExtension( p_demux, ".aacp" );
    b_forced_demux = demux_IsForced( p_demux, "m4a" ) ||
                     demux_IsForced( p_demux, "aac" ) ||
                     demux_IsForced( p_demux, "mp4a" );

    if( !b_forced_demux && !b_forced )
        return VLC_EGENERIC;

    i_offset = stream_Tell( p_demux->s );

    /* peek the begining (10 is for adts header) */
    if( stream_Peek( p_demux->s, &p_peek, 10 ) < 10 )
    {
        msg_Err( p_demux, "cannot peek" );
        return VLC_EGENERIC;
    }
    if( !strncmp( (char *)p_peek, "ADIF", 4 ) )
    {
        msg_Err( p_demux, "ADIF file. Not yet supported. (Please report)" );
        return VLC_EGENERIC;
    }

    *pi_offset = i_offset;
    return VLC_SUCCESS;
}
static int AacInit( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->i_packet_size = 4096;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Wav header skipper
 *****************************************************************************/
#define WAV_PROBE_SIZE (512*1024)
static int WavSkipHeader( demux_t *p_demux, int *pi_skip )
{
    const uint8_t *p_peek;
    int         i_peek = 0;

    /* */
    *pi_skip = 0;

    /* Check if we are dealing with a WAV file */
    if( stream_Peek( p_demux->s, &p_peek, 12+8 ) != 12 + 8 )
        return VLC_SUCCESS;

    if( memcmp( p_peek, "RIFF", 4 ) || memcmp( &p_peek[8], "WAVE", 4 ) )
        return VLC_SUCCESS;

    /* Find the wave format header */
    i_peek = 12 + 8;
    while( memcmp( p_peek + i_peek - 8, "fmt ", 4 ) )
    {
        uint32_t i_len = GetDWLE( p_peek + i_peek - 4 );
        if( i_len > WAV_PROBE_SIZE || i_peek + i_len > WAV_PROBE_SIZE )
            return VLC_EGENERIC;

        i_peek += i_len + 8;
        if( stream_Peek( p_demux->s, &p_peek, i_peek ) != i_peek )
            return VLC_EGENERIC;
    }

    /* Sanity check the wave format header */
    uint32_t i_len = GetDWLE( p_peek + i_peek - 4 );
    if( i_len > WAV_PROBE_SIZE )
        return VLC_EGENERIC;

    i_peek += i_len + 8;
    if( stream_Peek( p_demux->s, &p_peek, i_peek ) != i_peek )
        return VLC_EGENERIC;
    int i_format = GetWLE( p_peek + i_peek - i_len - 8 /* wFormatTag */ );
    if( i_format != WAVE_FORMAT_PCM &&
        i_format != WAVE_FORMAT_A52 &&
        i_format != WAVE_FORMAT_DTS )
        return VLC_EGENERIC;
    if( GetWLE( p_peek + i_peek - i_len - 6 /* nChannels */ ) != 2 )
        return VLC_EGENERIC;
    if( GetDWLE( p_peek + i_peek - i_len - 4 /* nSamplesPerSec */ ) !=
        44100 )
        return VLC_EGENERIC;

    /* Skip the wave header */
    while( memcmp( p_peek + i_peek - 8, "data", 4 ) )
    {
        uint32_t i_len = GetDWLE( p_peek + i_peek - 4 );
        if( i_len > WAV_PROBE_SIZE || i_peek + i_len > WAV_PROBE_SIZE )
            return VLC_EGENERIC;

        i_peek += i_len + 8;
        if( stream_Peek( p_demux->s, &p_peek, i_peek ) != i_peek )
            return VLC_EGENERIC;
    }
    *pi_skip = i_peek;
    return VLC_SUCCESS;
}

static int GenericProbe( demux_t *p_demux, int64_t *pi_offset,
                         const char * ppsz_name[],
                         int (*pf_check)( const uint8_t * ), int i_check_size )
{
    bool   b_forced_demux;

    int64_t i_offset;
    const uint8_t *p_peek;
    int i_skip;

    b_forced_demux = false;
    for( int i = 0; ppsz_name[i] != NULL; i++ )
    {
        b_forced_demux |= demux_IsForced( p_demux, ppsz_name[i] );
    }

    i_offset = stream_Tell( p_demux->s );

    if( WavSkipHeader( p_demux, &i_skip ) )
    {
        if( !b_forced_demux )
            return VLC_EGENERIC;
    }
    const bool b_wav = i_skip > 0;

    /* peek the begining
     * It is common that wav files have some sort of garbage at the begining */
    const int i_probe = i_skip + i_check_size + ( b_wav ? 16000 : 0);
    const int i_peek = stream_Peek( p_demux->s, &p_peek, i_probe );
    if( i_peek < i_skip + i_check_size )
    {
        msg_Err( p_demux, "cannot peek" );
        return VLC_EGENERIC;
    }
    for( ;; )
    {
        if( i_skip + i_check_size > i_peek )
        {
            if( !b_forced_demux )
                return VLC_EGENERIC;
            break;
        }
        const int i_size = pf_check( &p_peek[i_skip] );
        if( i_size >= 0 )
        {
            if( i_size == 0 || 1)
                break;

            /* If we have the frame size, check the next frame for
             * extra robustness */
            if( i_skip + i_check_size + i_size <= i_peek )
            {
                if( pf_check( &p_peek[i_skip+i_size] ) >= 0 )
                    break;
            }
        }
        i_skip++;
    }

    *pi_offset = i_offset + i_skip;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * A52
 *****************************************************************************/
static int A52CheckSync( const uint8_t *p_peek, bool *p_big_endian, bool b_eac3 )
{
    vlc_a52_header_t header;
    uint8_t p_tmp[VLC_A52_HEADER_SIZE];

    *p_big_endian =  p_peek[0] == 0x0b && p_peek[1] == 0x77;
    if( !*p_big_endian )
    {
        swab( p_peek, p_tmp, VLC_A52_HEADER_SIZE );
        p_peek = p_tmp;
    }

    if( vlc_a52_header_Parse( &header, p_peek, VLC_A52_HEADER_SIZE ) )
        return VLC_EGENERIC;

    if( !header.b_eac3 != !b_eac3 )
        return VLC_EGENERIC;
    return header.i_size;
}
static int EA52CheckSyncProbe( const uint8_t *p_peek )
{
    bool b_dummy;
    return A52CheckSync( p_peek, &b_dummy, true );
}

static int EA52Probe( demux_t *p_demux, int64_t *pi_offset )
{
    const char *ppsz_name[] = { "eac3", NULL };

    return GenericProbe( p_demux, pi_offset, ppsz_name, EA52CheckSyncProbe, VLC_A52_HEADER_SIZE );
}

static int A52CheckSyncProbe( const uint8_t *p_peek )
{
    bool b_dummy;
    return A52CheckSync( p_peek, &b_dummy, false );
}

static int A52Probe( demux_t *p_demux, int64_t *pi_offset )
{
    const char *ppsz_name[] = { "a52", "ac3", NULL };

    return GenericProbe( p_demux, pi_offset, ppsz_name, A52CheckSyncProbe, VLC_A52_HEADER_SIZE );
}

static int A52Init( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->b_big_endian = false;
    p_sys->i_packet_size = 1024;

    const uint8_t *p_peek;

    /* peek the begining */
    if( stream_Peek( p_demux->s, &p_peek, VLC_A52_HEADER_SIZE ) >= VLC_A52_HEADER_SIZE )
    {
        A52CheckSync( p_peek, &p_sys->b_big_endian, true );
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * DTS
 *****************************************************************************/
static int DtsCheckSync( const uint8_t *p_peek )
{
    /* TODO return frame size for robustness */

    /* 14 bits, little endian version of the bitstream */
    if( p_peek[0] == 0xff && p_peek[1] == 0x1f &&
        p_peek[2] == 0x00 && p_peek[3] == 0xe8 &&
        (p_peek[4] & 0xf0) == 0xf0 && p_peek[5] == 0x07 )
    {
        return 0;
    }
    /* 14 bits, big endian version of the bitstream */
    else if( p_peek[0] == 0x1f && p_peek[1] == 0xff &&
             p_peek[2] == 0xe8 && p_peek[3] == 0x00 &&
             p_peek[4] == 0x07 && (p_peek[5] & 0xf0) == 0xf0)
    {
        return 0;
    }
    /* 16 bits, big endian version of the bitstream */
    else if( p_peek[0] == 0x7f && p_peek[1] == 0xfe &&
             p_peek[2] == 0x80 && p_peek[3] == 0x01 )
    {
        return 0;
    }
    /* 16 bits, little endian version of the bitstream */
    else if( p_peek[0] == 0xfe && p_peek[1] == 0x7f &&
             p_peek[2] == 0x01 && p_peek[3] == 0x80 )
    {
        return 0;
    }

    return VLC_EGENERIC;
}

static int DtsProbe( demux_t *p_demux, int64_t *pi_offset )
{
    const char *ppsz_name[] = { "dts", NULL };

    return GenericProbe( p_demux, pi_offset, ppsz_name, DtsCheckSync, 11 );
}
static int DtsInit( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->i_packet_size = 16384;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * MLP
 *****************************************************************************/
static int MlpCheckSync( const uint8_t *p_peek )
{
    if( p_peek[4+0] != 0xf8 || p_peek[4+1] != 0x72 || p_peek[4+2] != 0x6f )
        return -1;

    if( p_peek[4+3] != 0xba && p_peek[4+3] != 0xbb )
        return -1;

    /* TODO checksum and real size for robustness */
    return 0;
}
static int MlpProbe( demux_t *p_demux, int64_t *pi_offset )
{
    const char *ppsz_name[] = { "mlp", "thd", NULL };

    return GenericProbe( p_demux, pi_offset, ppsz_name, MlpCheckSync, 4+28+16*4 );
}
static int MlpInit( demux_t *p_demux )

{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->i_packet_size = 4096;

    return VLC_SUCCESS;
}

