//
// ZoneMinder Ffmpeg Camera Class Implementation, $Date: 2009-01-16 12:18:50 +0000 (Fri, 16 Jan 2009) $, $Revision: 2713 $
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// 

#include "zm.h"

#if HAVE_LIBAVFORMAT

#include "zm_ffmpeg_camera.h"

extern "C" {
#include "libavutil/time.h"
#if HAVE_AVUTIL_HWCONTEXT_H
	#include "libavutil/hwcontext.h"
	#include "libavutil/hwcontext_qsv.h"
#endif
}
#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

#ifdef SOLARIS
#include <sys/errno.h>  // for ESRCH
#include <signal.h>
#include <pthread.h>
#endif


#if HAVE_AVUTIL_HWCONTEXT_H
static AVPixelFormat get_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts) {
  while (*pix_fmts != AV_PIX_FMT_NONE) {
    if (*pix_fmts == AV_PIX_FMT_QSV) {
      DecodeContext *decode = (DecodeContext *)avctx->opaque;
      AVHWFramesContext  *frames_ctx;
      AVQSVFramesContext *frames_hwctx;
      int ret;

      /* create a pool of surfaces to be used by the decoder */
      avctx->hw_frames_ctx = av_hwframe_ctx_alloc(decode->hw_device_ref);
      if (!avctx->hw_frames_ctx)
        return AV_PIX_FMT_NONE;
      frames_ctx   = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
      frames_hwctx = (AVQSVFramesContext*)frames_ctx->hwctx;

      frames_ctx->format            = AV_PIX_FMT_QSV;
      frames_ctx->sw_format         = avctx->sw_pix_fmt;
      frames_ctx->width             = FFALIGN(avctx->coded_width,  32);
      frames_ctx->height            = FFALIGN(avctx->coded_height, 32);
      frames_ctx->initial_pool_size = 32;

      frames_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

      ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
      if (ret < 0)
        return AV_PIX_FMT_NONE;

      return AV_PIX_FMT_QSV;
    }

    pix_fmts++;
  }

  Error( "The QSV pixel format not offered in get_format()");

  return AV_PIX_FMT_NONE;
}
#endif

FfmpegCamera::FfmpegCamera( int p_id, const std::string &p_path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio ) :
  Camera( p_id, FFMPEG_SRC, p_width, p_height, p_colours, ZM_SUBPIX_ORDER_DEFAULT_FOR_COLOUR(p_colours), p_brightness, p_contrast, p_hue, p_colour, p_capture, p_record_audio ),
  mPath( p_path ),
  mMethod( p_method ),
  mOptions( p_options )
{
  if ( capture ) {
    Initialise();
  }

  hwaccel = false;
#if HAVE_AVUTIL_HWCONTEXT_H
  decode = { NULL };
  hwFrame = NULL;
#endif

  mFormatContext = NULL;
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  mVideoCodecContext = NULL;
  mAudioCodecContext = NULL;
  mVideoCodec = NULL;
  mAudioCodec = NULL;
  mRawFrame = NULL;
  mFrame = NULL;
  frameCount = 0;
  startTime = 0;
  mIsOpening = false;
  mCanCapture = false;
  mOpenStart = 0;
  mReopenThread = 0;

#if HAVE_LIBSWSCALE  
  mConvertContext = NULL;
#endif
  /* Has to be located inside the constructor so other components such as zma will receive correct colours and subpixel order */
  if ( colours == ZM_COLOUR_RGB32 ) {
    subpixelorder = ZM_SUBPIX_ORDER_RGBA;
    imagePixFormat = AV_PIX_FMT_RGBA;
  } else if ( colours == ZM_COLOUR_RGB24 ) {
    subpixelorder = ZM_SUBPIX_ORDER_RGB;
    imagePixFormat = AV_PIX_FMT_RGB24;
  } else if ( colours == ZM_COLOUR_GRAY8 ) {
    subpixelorder = ZM_SUBPIX_ORDER_NONE;
    imagePixFormat = AV_PIX_FMT_GRAY8;
  } else {
    Panic("Unexpected colours: %d",colours);
  }
} // end FFmpegCamera::FFmpegCamera

FfmpegCamera::~FfmpegCamera() {

  CloseFfmpeg();

  if ( capture ) {
    Terminate();
  }
  avformat_network_deinit();
}

void FfmpegCamera::Initialise() {
  if ( logDebugging() )
    av_log_set_level( AV_LOG_DEBUG ); 
  else
    av_log_set_level( AV_LOG_QUIET ); 

  av_register_all();
  avformat_network_init();
}

void FfmpegCamera::Terminate() {
}

int FfmpegCamera::PrimeCapture() {
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  Info( "Priming capture from %s", mPath.c_str() );

#if THREAD
  if ( OpenFfmpeg() != 0 ) {
    ReopenFfmpeg();
  }
  return 0;
#else
  return OpenFfmpeg();
#endif
}

int FfmpegCamera::PreCapture() {
  // If Reopen was called, then ffmpeg is closed and we need to reopen it.
  if ( ! mCanCapture )
    return OpenFfmpeg();
  // Nothing to do here
  return( 0 );
}

int FfmpegCamera::Capture( ZMPacket &zm_packet ) {
  if ( ! mCanCapture ) {
    return -1;
  }

  int ret;

  // If the reopen thread has a value, but mCanCapture != 0, then we have just reopened the connection to the ffmpeg device, and we can clean up the thread.
  if ( mReopenThread != 0 ) {
    void *retval = 0;
    ret = pthread_join(mReopenThread, &retval);
    if ( ret != 0 ) {
      Error("Could not join reopen thread.");
    }
    Info( "Successfully reopened stream." );
    mReopenThread = 0;
  }

  ret = av_read_frame( mFormatContext, &packet );
  if ( ret < 0 ) {
    if (
        // Check if EOF.
        (ret == AVERROR_EOF || (mFormatContext->pb && mFormatContext->pb->eof_reached)) ||
        // Check for Connection failure.
        (ret == -110)
       ) {
      Info( "av_read_frame returned \"%s\". Reopening stream.", av_make_error_string(ret).c_str() );
      ReopenFfmpeg();
    }
    Error( "Unable to read packet from stream %d: error %d \"%s\".", packet.stream_index, ret, av_make_error_string(ret).c_str() );
    return -1;
  }
  Debug( 5, "Got packet from stream %d dts (%d) pts(%d)", packet.stream_index, packet.pts, packet.dts );

  zm_packet.set_packet( &packet );
  zm_av_packet_unref( &packet );
  return 1;
} // FfmpegCamera::Capture

int FfmpegCamera::PostCapture() {
  // Nothing to do here
  return( 0 );
}

int FfmpegCamera::OpenFfmpeg() {

  Debug ( 2, "OpenFfmpeg called." );

  int ret;

  mOpenStart = time(NULL);
  mIsOpening = true;

  // Open the input, not necessarily a file
#if !LIBAVFORMAT_VERSION_CHECK(53, 2, 0, 4, 0)
  Debug ( 1, "Calling av_open_input_file" );
  if ( av_open_input_file( &mFormatContext, mPath.c_str(), NULL, 0, NULL ) != 0 )
#else
  // Handle options
  AVDictionary *opts = 0;
  ret = av_dict_parse_string(&opts, Options().c_str(), "=", ",", 0);
  if ( ret < 0 ) {
    Warning("Could not parse ffmpeg input options list '%s'\n", Options().c_str());
  }

  // Set transport method as specified by method field, rtpUni is default
  const std::string method = Method();
  if ( method == "rtpMulti" ) {
    ret = av_dict_set(&opts, "rtsp_transport", "udp_multicast", 0);
  } else if ( method == "rtpRtsp" ) {
    ret = av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  } else if ( method == "rtpRtspHttp" ) {
    ret = av_dict_set(&opts, "rtsp_transport", "http", 0);
  } else {
    Warning("Unknown method (%s)", method.c_str() );
  }

  if ( ret < 0 ) {
    Warning("Could not set rtsp_transport method '%s'\n", method.c_str());
  }

  Debug ( 1, "Calling avformat_open_input for %s", mPath.c_str() );

  mFormatContext = avformat_alloc_context( );
  //mFormatContext->interrupt_callback.callback = FfmpegInterruptCallback;
  //mFormatContext->interrupt_callback.opaque = this;
  // Speed up find_stream_info
  //FIXME can speed up initial analysis but need sensible parameters...
  //mFormatContext->probesize = 32;
  //mFormatContext->max_analyze_duration = 32;

  if ( avformat_open_input( &mFormatContext, mPath.c_str(), NULL, &opts ) != 0 )
#endif
  {
    mIsOpening = false;
    Error( "Unable to open input %s due to: %s", mPath.c_str(), strerror(errno) );
    return -1;
  }

  AVDictionaryEntry *e;
  if ( (e = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)) != NULL ) {
    Warning( "Option %s not recognized by ffmpeg", e->key);
  }

  mIsOpening = false;
  Debug ( 1, "Opened input" );

  Info( "Stream open %s", mPath.c_str() );

#if !LIBAVFORMAT_VERSION_CHECK(53, 6, 0, 6, 0)
  Debug ( 1, "Calling av_find_stream_info" );
  if ( av_find_stream_info( mFormatContext ) < 0 )
#else
    Debug ( 1, "Calling avformat_find_stream_info" );
  if ( avformat_find_stream_info( mFormatContext, 0 ) < 0 )
#endif
    Fatal( "Unable to find stream info from %s due to: %s", mPath.c_str(), strerror(errno) );

  startTime = av_gettime();//FIXME here or after find_Stream_info
  Debug ( 1, "Got stream info" );

  // Find first video stream present
  // The one we want Might not be the first
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  for ( unsigned int i=0; i < mFormatContext->nb_streams; i++ ) {
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    if ( mFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ) {
#else
#if (LIBAVCODEC_VERSION_CHECK(52, 64, 0, 64, 0) || LIBAVUTIL_VERSION_CHECK(50, 14, 0, 14, 0))
    if ( mFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
#else
    if ( mFormatContext->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO ) {
#endif
#endif
      if ( mVideoStreamId == -1 ) {
        mVideoStreamId = i;
        // if we break, then we won't find the audio stream
        continue;
      } else {
        Debug(2, "Have another video stream." );
      }
    }
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    if ( mFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
#else
#if (LIBAVCODEC_VERSION_CHECK(52, 64, 0, 64, 0) || LIBAVUTIL_VERSION_CHECK(50, 14, 0, 14, 0))
    if ( mFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO ) {
#else
    if ( mFormatContext->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO ) {
#endif
#endif
      if ( mAudioStreamId == -1 ) {
        mAudioStreamId = i;
      } else {
        Debug(2, "Have another audio stream." );
      }
    }
  } // end foreach stream
  if ( mVideoStreamId == -1 )
    Fatal( "Unable to locate video stream in %s", mPath.c_str() );
  if ( mAudioStreamId == -1 )
    Debug( 3, "Unable to locate audio stream in %s", mPath.c_str() );

  Debug ( 3, "Found video stream at index %d", mVideoStreamId );
  Debug ( 3, "Found audio stream at index %d", mAudioStreamId );

#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
  mVideoCodecContext = avcodec_alloc_context3( NULL );
  avcodec_parameters_to_context( mVideoCodecContext, mFormatContext->streams[mVideoStreamId]->codecpar );
#else
  mVideoCodecContext = mFormatContext->streams[mVideoStreamId]->codec;
#endif
	// STolen from ispy
	//this fixes issues with rtsp streams!! woot.
	//mVideoCodecContext->flags2 |= CODEC_FLAG2_FAST | CODEC_FLAG2_CHUNKS | CODEC_FLAG_LOW_DELAY;  // Enable faster H264 decode.
#ifdef CODEC_FLAG2_FAST
	mVideoCodecContext->flags2 |= CODEC_FLAG2_FAST | CODEC_FLAG_LOW_DELAY;
#endif

#if HAVE_AVUTIL_HWCONTEXT_H
  if ( mVideoCodecContext->codec_id == AV_CODEC_ID_H264 ) {

    //vaapi_decoder = new VAAPIDecoder();
    //mVideoCodecContext->opaque = vaapi_decoder;
    //mVideoCodec = vaapi_decoder->openCodec( mVideoCodecContext );

    if ( ! mVideoCodec ) {
      // Try to open an hwaccel codec.
      if ( (mVideoCodec = avcodec_find_decoder_by_name("h264_vaapi")) == NULL ) { 
        Debug(1, "Failed to find decoder (h264_vaapi)" );
      } else {
        Debug(1, "Success finding decoder (h264_vaapi)" );
      }
    }
    if ( ! mVideoCodec ) {
      // Try to open an hwaccel codec.
      if ( (mVideoCodec = avcodec_find_decoder_by_name("h264_qsv")) == NULL ) { 
        Debug(1, "Failed to find decoder (h264_qsv)" );
      } else {
        Debug(1, "Success finding decoder (h264_qsv)" );
        /* open the hardware device */
        ret = av_hwdevice_ctx_create(&decode.hw_device_ref, AV_HWDEVICE_TYPE_QSV,
            "auto", NULL, 0);
        if (ret < 0) {
          Error("Failed to open the hardware device");
          mVideoCodec = NULL;
        } else {
          mVideoCodecContext->opaque      = &decode;
          mVideoCodecContext->get_format  = get_format;
          hwaccel = true;
          hwFrame = zm_av_frame_alloc();
        }
      }
    }
  } else {
#ifdef AV_CODEC_ID_H265
    if ( mVideoCodecContext->codec_id == AV_CODEC_ID_H265 ) {
      Debug( 1, "Input stream appears to be h265.  The stored event file may not be viewable in browser." );
    } else {
#endif
      Error( "Input stream is not h264.  The stored event file may not be viewable in browser." );
#ifdef AV_CODEC_ID_H265
    }
#endif
  } // end if h264
#endif

  if ( (!mVideoCodec) and ( (mVideoCodec = avcodec_find_decoder(mVideoCodecContext->codec_id)) == NULL ) ) {
  // Try and get the codec from the codec context
    Fatal("Can't find codec for video stream from %s", mPath.c_str());
  } else {
    Debug(1, "Video Found decoder");
    zm_dump_stream_format(mFormatContext, mVideoStreamId, 0, 0);
  // Open the codec
#if !LIBAVFORMAT_VERSION_CHECK(53, 8, 0, 8, 0)
  Debug ( 1, "Calling avcodec_open" );
  if ( avcodec_open(mVideoCodecContext, mVideoCodec) < 0 ){
#else
    Debug ( 1, "Calling avcodec_open2" );
  if ( avcodec_open2(mVideoCodecContext, mVideoCodec, &opts) < 0 ) {
#endif
    AVDictionaryEntry *e;
    if ( (e = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)) != NULL ) {
      Warning( "Option %s not recognized by ffmpeg", e->key);
    }
    Fatal( "Unable to open codec for video stream from %s", mPath.c_str() );
  } else {

    AVDictionaryEntry *e;
    if ( (e = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)) != NULL ) {
      Warning( "Option %s not recognized by ffmpeg", e->key);
    }
  }
  }

  if ( mVideoCodecContext->codec_id != AV_CODEC_ID_H264 ) {
#ifdef AV_CODEC_ID_H265
    if ( mVideoCodecContext->codec_id == AV_CODEC_ID_H265 ) {
      Debug( 1, "Input stream appears to be h265.  The stored event file may not be viewable in browser." );
    } else {
#endif
      Warning( "Input stream is not h264.  The stored event file may not be viewable in browser." );
#ifdef AV_CODEC_ID_H265
    }
#endif
  }

  if (mVideoCodecContext->hwaccel != NULL) {
    Debug(1, "HWACCEL in use");
  } else {
    Debug(1, "HWACCEL not in use");
  }
  if ( mAudioStreamId >= 0 ) {
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    mAudioCodecContext = avcodec_alloc_context3( NULL );
    avcodec_parameters_to_context( mAudioCodecContext, mFormatContext->streams[mAudioStreamId]->codecpar );
#else
    mAudioCodecContext = mFormatContext->streams[mAudioStreamId]->codec;
#endif
    if ( (mAudioCodec = avcodec_find_decoder(mAudioCodecContext->codec_id)) == NULL ) {
      Debug(1, "Can't find codec for audio stream from %s", mPath.c_str());
    } else {
      Debug(1, "Audio Found decoder");
      zm_dump_stream_format(mFormatContext, mAudioStreamId, 0, 0);
  // Open the codec
#if !LIBAVFORMAT_VERSION_CHECK(53, 8, 0, 8, 0)
  Debug ( 1, "Calling avcodec_open" );
  if ( avcodec_open(mAudioCodecContext, mAudioCodec) < 0 )
#else
    Debug ( 1, "Calling avcodec_open2" );
  if ( avcodec_open2(mAudioCodecContext, mAudioCodec, 0) < 0 )
#endif
    Fatal( "Unable to open codec for video stream from %s", mPath.c_str() );
    }
  }

  Debug ( 1, "Opened codec" );

  // Allocate space for the native video frame
  mRawFrame = zm_av_frame_alloc();

  // Allocate space for the converted video frame
  mFrame = zm_av_frame_alloc();

  if ( mRawFrame == NULL || mFrame == NULL )
    Fatal( "Unable to allocate frame for %s", mPath.c_str() );

  Debug ( 1, "Allocated frames" );

#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
  int pSize = av_image_get_buffer_size( imagePixFormat, width, height,1 );
#else
  int pSize = avpicture_get_size( imagePixFormat, width, height );
#endif

  if ( (unsigned int)pSize != imagesize ) {
    Fatal("Image size mismatch. Required: %d Available: %d",pSize,imagesize);
  }

  Debug ( 1, "Validated imagesize" );

#if HAVE_LIBSWSCALE
  Debug ( 1, "Calling sws_isSupportedInput" );
  if ( !sws_isSupportedInput(mVideoCodecContext->pix_fmt) ) {
    Fatal("swscale does not support the codec format: %c%c%c%c", (mVideoCodecContext->pix_fmt)&0xff, ((mVideoCodecContext->pix_fmt >> 8)&0xff), ((mVideoCodecContext->pix_fmt >> 16)&0xff), ((mVideoCodecContext->pix_fmt >> 24)&0xff));
  }

  if ( !sws_isSupportedOutput(imagePixFormat) ) {
    Fatal("swscale does not support the target format: %c%c%c%c",(imagePixFormat)&0xff,((imagePixFormat>>8)&0xff),((imagePixFormat>>16)&0xff),((imagePixFormat>>24)&0xff));
  }

  mConvertContext = sws_getContext(mVideoCodecContext->width,
      mVideoCodecContext->height,
      mVideoCodecContext->pix_fmt,
      width, height,
      imagePixFormat, SWS_BICUBIC, NULL,
      NULL, NULL);
  if ( mConvertContext == NULL )
    Fatal( "Unable to create conversion context for %s", mPath.c_str() );
#else // HAVE_LIBSWSCALE
  Fatal( "You must compile ffmpeg with the --enable-swscale option to use ffmpeg cameras" );
#endif // HAVE_LIBSWSCALE

  if ( (unsigned int)mVideoCodecContext->width != width || (unsigned int)mVideoCodecContext->height != height ) {
    Warning( "Monitor dimensions are %dx%d but camera is sending %dx%d", width, height, mVideoCodecContext->width, mVideoCodecContext->height );
  }

  mCanCapture = true;

  return 0;
} // int FfmpegCamera::OpenFfmpeg()

int FfmpegCamera::ReopenFfmpeg() {

  Debug(2, "ReopenFfmpeg called.");

#if THREAD 
  mCanCapture = false;
  if ( pthread_create( &mReopenThread, NULL, ReopenFfmpegThreadCallback, (void*) this) != 0 ) {
    // Log a fatal error and exit the process.
    Fatal( "ReopenFfmpeg failed to create worker thread." );
  }
#else
  CloseFfmpeg();
  OpenFfmpeg();

#endif

  return 0;
}

int FfmpegCamera::CloseFfmpeg() {

  Debug(2, "CloseFfmpeg called.");

  mCanCapture = false;

  if ( mFrame ) {
    av_frame_free( &mFrame );
    mFrame = NULL;
  }
  if ( mRawFrame ) {
    av_frame_free( &mRawFrame );
    mRawFrame = NULL;
  }

#if HAVE_LIBSWSCALE
  if ( mConvertContext ) {
    sws_freeContext( mConvertContext );
    mConvertContext = NULL;
  }
#endif

  if ( mVideoCodecContext ) {
    avcodec_close(mVideoCodecContext);
    //av_free(mVideoCodecContext);
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    avcodec_free_context(&mVideoCodecContext);
#endif
    mVideoCodecContext = NULL; // Freed by av_close_input_file
  }
  if ( mAudioCodecContext ) {
    avcodec_close(mAudioCodecContext);
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    avcodec_free_context(&mAudioCodecContext);
#endif
    //av_free(mAudioCodecContext);
    mAudioCodecContext = NULL; // Freed by av_close_input_file
  }

  if ( mFormatContext ) {
#if !LIBAVFORMAT_VERSION_CHECK(53, 17, 0, 25, 0)
    av_close_input_file( mFormatContext );
#else
    avformat_close_input( &mFormatContext );
#endif
    mFormatContext = NULL;
  }

  return 0;
}

int FfmpegCamera::FfmpegInterruptCallback(void *ctx) { 
  Debug(3,"FfmpegInteruptCallback");
  FfmpegCamera* camera = reinterpret_cast<FfmpegCamera*>(ctx);
  if ( camera->mIsOpening ) {
    int now = time(NULL);
    if ( (now - camera->mOpenStart) > config.ffmpeg_open_timeout ) {
      Error( "Open video took more than %d seconds.", config.ffmpeg_open_timeout );
      return 1;
    }
  }

  return 0;
}

void *FfmpegCamera::ReopenFfmpegThreadCallback(void *ctx){
  Debug(3,"FfmpegReopenThreadtCallback");
  if ( ctx == NULL ) return NULL;

  FfmpegCamera* camera = reinterpret_cast<FfmpegCamera*>(ctx);

  while (1) {
    // Close current stream.
    camera->CloseFfmpeg();

    // Sleep if necessary to not reconnect too fast.
    int wait = config.ffmpeg_open_timeout - (time(NULL) - camera->mOpenStart);
    wait = wait < 0 ? 0 : wait;
    if ( wait > 0 ) {
      Debug( 1, "Sleeping %d seconds before reopening stream.", wait );
      sleep(wait);
    }

    if ( camera->OpenFfmpeg() == 0 ) {
      return NULL;
    }
  }
}

#endif // HAVE_LIBAVFORMAT
