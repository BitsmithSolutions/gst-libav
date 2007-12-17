/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <string.h>

#ifdef HAVE_FFMPEG_UNINSTALLED
#include <avcodec.h>
#else
#include <ffmpeg/avcodec.h>
#endif

#include <gst/gst.h>

#include "gstffmpeg.h"
#include "gstffmpegcodecmap.h"

//#define FORCE_OUR_GET_BUFFER

typedef struct _GstFFMpegDec GstFFMpegDec;

struct _GstFFMpegDec
{
  GstElement element;

  /* We need to keep track of our pads, so we do so here. */
  GstPad *srcpad;
  GstPad *sinkpad;

  /* decoding */
  AVCodecContext *context;
  AVFrame *picture;
  gboolean opened;
  union
  {
    struct
    {
      gint width, height;
      gint clip_width, clip_height;
      gint fps_n, fps_d;
      gint old_fps_n, old_fps_d;

      enum PixelFormat pix_fmt;
    } video;
    struct
    {
      gint channels;
      gint samplerate;
    } audio;
  } format;
  gboolean waiting_for_key;
  gboolean discont;
  guint64 next_ts;

  /* parsing */
  AVCodecParserContext *pctx;
  GstBuffer *pcache;

  GstBuffer *last_buffer;

  GValue *par;                  /* pixel aspect ratio of incoming data */

  gint hurry_up, lowres;

  /* QoS stuff *//* with LOCK */
  gdouble proportion;
  GstClockTime earliest_time;

  /* clipping segment */
  GstSegment segment;

  /* out-of-order incoming buffer special handling */
  gboolean outoforder;
  GstClockTime tstamp1, tstamp2;
  GstClockTime dur1, dur2;

  gboolean is_realvideo;
};

typedef struct _GstFFMpegDecClass GstFFMpegDecClass;

struct _GstFFMpegDecClass
{
  GstElementClass parent_class;

  AVCodec *in_plugin;
  GstPadTemplate *srctempl, *sinktempl;
};

typedef struct _GstFFMpegDecClassParams GstFFMpegDecClassParams;

struct _GstFFMpegDecClassParams
{
  AVCodec *in_plugin;
  GstCaps *srccaps, *sinkcaps;
};

#define GST_TYPE_FFMPEGDEC \
  (gst_ffmpegdec_get_type())
#define GST_FFMPEGDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FFMPEGDEC,GstFFMpegDec))
#define GST_FFMPEGDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FFMPEGDEC,GstFFMpegDecClass))
#define GST_IS_FFMPEGDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FFMPEGDEC))
#define GST_IS_FFMPEGDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FFMPEGDEC))

enum
{
  ARG_0,
  ARG_LOWRES,
  ARG_SKIPFRAME
};

/* A number of function prototypes are given so we can refer to them later. */
static void gst_ffmpegdec_base_init (GstFFMpegDecClass * klass);
static void gst_ffmpegdec_class_init (GstFFMpegDecClass * klass);
static void gst_ffmpegdec_init (GstFFMpegDec * ffmpegdec);
static void gst_ffmpegdec_finalize (GObject * object);

static gboolean gst_ffmpegdec_query (GstPad * pad, GstQuery * query);
static gboolean gst_ffmpegdec_src_event (GstPad * pad, GstEvent * event);

static gboolean gst_ffmpegdec_setcaps (GstPad * pad, GstCaps * caps);
static gboolean gst_ffmpegdec_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_ffmpegdec_chain (GstPad * pad, GstBuffer * buf);

static GstStateChangeReturn gst_ffmpegdec_change_state (GstElement * element,
    GstStateChange transition);

static void gst_ffmpegdec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_ffmpegdec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_ffmpegdec_negotiate (GstFFMpegDec * ffmpegdec);

/* some sort of bufferpool handling, but different */
static int gst_ffmpegdec_get_buffer (AVCodecContext * context,
    AVFrame * picture);
static void gst_ffmpegdec_release_buffer (AVCodecContext * context,
    AVFrame * picture);

#define GST_FFDEC_PARAMS_QDATA g_quark_from_static_string("ffdec-params")

static GstElementClass *parent_class = NULL;

#define GST_FFMPEGDEC_TYPE_LOWRES (gst_ffmpegdec_lowres_get_type())
static GType
gst_ffmpegdec_lowres_get_type (void)
{
  static GType ffmpegdec_lowres_type = 0;

  if (!ffmpegdec_lowres_type) {
    static const GEnumValue ffmpegdec_lowres[] = {
      {0, "0", "full"},
      {1, "1", "1/2-size"},
      {2, "2", "1/4-size"},
      {0, NULL, NULL},
    };

    ffmpegdec_lowres_type =
        g_enum_register_static ("GstFFMpegDecLowres", ffmpegdec_lowres);
  }

  return ffmpegdec_lowres_type;
}

#define GST_FFMPEGDEC_TYPE_SKIPFRAME (gst_ffmpegdec_skipframe_get_type())
static GType
gst_ffmpegdec_skipframe_get_type (void)
{
  static GType ffmpegdec_skipframe_type = 0;

  if (!ffmpegdec_skipframe_type) {
    static const GEnumValue ffmpegdec_skipframe[] = {
      {0, "0", "Skip nothing"},
      {1, "1", "Skip B-frames"},
      {2, "2", "Skip IDCT/Dequantization"},
      {5, "5", "Skip everything"},
      {0, NULL, NULL},
    };

    ffmpegdec_skipframe_type =
        g_enum_register_static ("GstFFMpegDecSkipFrame", ffmpegdec_skipframe);
  }

  return ffmpegdec_skipframe_type;
}

static void
gst_ffmpegdec_base_init (GstFFMpegDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstFFMpegDecClassParams *params;
  GstElementDetails details;
  GstPadTemplate *sinktempl, *srctempl;

  params =
      (GstFFMpegDecClassParams *) g_type_get_qdata (G_OBJECT_CLASS_TYPE (klass),
      GST_FFDEC_PARAMS_QDATA);
  g_assert (params != NULL);

  /* construct the element details struct */
  details.longname = g_strdup_printf ("FFMPEG %s decoder",
      gst_ffmpeg_get_codecid_longname (params->in_plugin->id));
  details.klass = g_strdup_printf ("Codec/Decoder/%s",
      (params->in_plugin->type == CODEC_TYPE_VIDEO) ? "Video" : "Audio");
  details.description = g_strdup_printf ("FFMPEG %s decoder",
      params->in_plugin->name);
  details.author = "Wim Taymans <wim@fluendo.com>, "
      "Ronald Bultje <rbultje@ronald.bitfreak.net>, "
      "Edward Hervey <bilboed@bilboed.com>";
  gst_element_class_set_details (element_class, &details);
  g_free (details.longname);
  g_free (details.klass);
  g_free (details.description);

  /* pad templates */
  sinktempl = gst_pad_template_new ("sink", GST_PAD_SINK,
      GST_PAD_ALWAYS, params->sinkcaps);
  srctempl = gst_pad_template_new ("src", GST_PAD_SRC,
      GST_PAD_ALWAYS, params->srccaps);

  gst_element_class_add_pad_template (element_class, srctempl);
  gst_element_class_add_pad_template (element_class, sinktempl);

  klass->in_plugin = params->in_plugin;
  klass->srctempl = srctempl;
  klass->sinktempl = sinktempl;
}

static void
gst_ffmpegdec_class_init (GstFFMpegDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_ffmpegdec_finalize;

  gobject_class->set_property = gst_ffmpegdec_set_property;
  gobject_class->get_property = gst_ffmpegdec_get_property;

  if (klass->in_plugin->type == CODEC_TYPE_VIDEO) {
    g_object_class_install_property (gobject_class, ARG_SKIPFRAME,
        g_param_spec_enum ("skip-frame", "Skip frames",
            "Which types of frames to skip during decoding",
            GST_FFMPEGDEC_TYPE_SKIPFRAME, 0, G_PARAM_READWRITE));
    g_object_class_install_property (gobject_class, ARG_LOWRES,
        g_param_spec_enum ("lowres", "Low resolution",
            "At which resolution to decode images",
            GST_FFMPEGDEC_TYPE_LOWRES, 0, G_PARAM_READWRITE));
  }

  gstelement_class->change_state = gst_ffmpegdec_change_state;
}

static void
gst_ffmpegdec_init (GstFFMpegDec * ffmpegdec)
{
  GstFFMpegDecClass *oclass;

  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  /* setup pads */
  ffmpegdec->sinkpad = gst_pad_new_from_template (oclass->sinktempl, "sink");
  gst_pad_set_setcaps_function (ffmpegdec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_setcaps));
  gst_pad_set_event_function (ffmpegdec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_sink_event));
  gst_pad_set_chain_function (ffmpegdec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_chain));
  gst_element_add_pad (GST_ELEMENT (ffmpegdec), ffmpegdec->sinkpad);

  ffmpegdec->srcpad = gst_pad_new_from_template (oclass->srctempl, "src");
  gst_pad_use_fixed_caps (ffmpegdec->srcpad);
  gst_pad_set_event_function (ffmpegdec->srcpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_src_event));
  gst_pad_set_query_function (ffmpegdec->srcpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_query));
  gst_element_add_pad (GST_ELEMENT (ffmpegdec), ffmpegdec->srcpad);

  /* some ffmpeg data */
  ffmpegdec->context = avcodec_alloc_context ();
  ffmpegdec->picture = avcodec_alloc_frame ();
  ffmpegdec->pctx = NULL;
  ffmpegdec->pcache = NULL;
  ffmpegdec->par = NULL;
  ffmpegdec->opened = FALSE;
  ffmpegdec->waiting_for_key = TRUE;
  ffmpegdec->hurry_up = ffmpegdec->lowres = 0;

  ffmpegdec->last_buffer = NULL;

  ffmpegdec->format.video.fps_n = -1;
  ffmpegdec->format.video.old_fps_n = -1;
  gst_segment_init (&ffmpegdec->segment, GST_FORMAT_TIME);

  ffmpegdec->tstamp1 = ffmpegdec->tstamp2 = GST_CLOCK_TIME_NONE;
  ffmpegdec->dur1 = ffmpegdec->dur2 = GST_CLOCK_TIME_NONE;
}

static void
gst_ffmpegdec_finalize (GObject * object)
{
  GstFFMpegDec *ffmpegdec = (GstFFMpegDec *) object;

  g_assert (!ffmpegdec->opened);

  /* clean up remaining allocated data */
  av_free (ffmpegdec->context);
  av_free (ffmpegdec->picture);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_ffmpegdec_query (GstPad * pad, GstQuery * query)
{
  GstFFMpegDec *ffmpegdec;
  GstPad *peer;
  gboolean res;

  ffmpegdec = (GstFFMpegDec *) gst_pad_get_parent (pad);

  res = FALSE;

  if ((peer = gst_pad_get_peer (ffmpegdec->sinkpad))) {
    /* just forward to peer */
    res = gst_pad_query (peer, query);
    gst_object_unref (peer);
  }
#if 0
  {
    GstFormat bfmt;

    bfmt = GST_FORMAT_BYTES;

    /* ok, do bitrate calc... */
    if ((type != GST_QUERY_POSITION && type != GST_QUERY_TOTAL) ||
        *fmt != GST_FORMAT_TIME || ffmpegdec->context->bit_rate == 0 ||
        !gst_pad_query (peer, type, &bfmt, value))
      return FALSE;

    if (ffmpegdec->pcache && type == GST_QUERY_POSITION)
      *value -= GST_BUFFER_SIZE (ffmpegdec->pcache);
    *value *= GST_SECOND / ffmpegdec->context->bit_rate;
  }
#endif

  gst_object_unref (ffmpegdec);

  return res;
}

static void
gst_ffmpegdec_update_qos (GstFFMpegDec * ffmpegdec, gdouble proportion,
    GstClockTime time)
{
  GST_OBJECT_LOCK (ffmpegdec);
  ffmpegdec->proportion = proportion;
  ffmpegdec->earliest_time = time;
  GST_OBJECT_UNLOCK (ffmpegdec);
}

static void
gst_ffmpegdec_reset_qos (GstFFMpegDec * ffmpegdec)
{
  gst_ffmpegdec_update_qos (ffmpegdec, 0.5, GST_CLOCK_TIME_NONE);
}

static void
gst_ffmpegdec_read_qos (GstFFMpegDec * ffmpegdec, gdouble * proportion,
    GstClockTime * time)
{
  GST_OBJECT_LOCK (ffmpegdec);
  *proportion = ffmpegdec->proportion;
  *time = ffmpegdec->earliest_time;
  GST_OBJECT_UNLOCK (ffmpegdec);
}

static gboolean
gst_ffmpegdec_src_event (GstPad * pad, GstEvent * event)
{
  GstFFMpegDec *ffmpegdec;
  gboolean res;

  ffmpegdec = (GstFFMpegDec *) gst_pad_get_parent (pad);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      gdouble proportion;
      GstClockTimeDiff diff;
      GstClockTime timestamp;

      gst_event_parse_qos (event, &proportion, &diff, &timestamp);

      /* update our QoS values */
      gst_ffmpegdec_update_qos (ffmpegdec, proportion, timestamp + diff);

      /* forward upstream */
      res = gst_pad_push_event (ffmpegdec->sinkpad, event);
      break;
    }
    default:
      /* forward upstream */
      res = gst_pad_push_event (ffmpegdec->sinkpad, event);
      break;
  }

  gst_object_unref (ffmpegdec);

  return res;
}

/* with LOCK */
static void
gst_ffmpegdec_close (GstFFMpegDec * ffmpegdec)
{
  if (!ffmpegdec->opened)
    return;

  if (ffmpegdec->par) {
    g_free (ffmpegdec->par);
    ffmpegdec->par = NULL;
  }

  if (ffmpegdec->context->priv_data)
    gst_ffmpeg_avcodec_close (ffmpegdec->context);
  ffmpegdec->opened = FALSE;

  if (ffmpegdec->context->palctrl) {
    av_free (ffmpegdec->context->palctrl);
    ffmpegdec->context->palctrl = NULL;
  }

  if (ffmpegdec->context->extradata) {
    av_free (ffmpegdec->context->extradata);
    ffmpegdec->context->extradata = NULL;
  }

  if (ffmpegdec->pctx) {
    if (ffmpegdec->pcache) {
      gst_buffer_unref (ffmpegdec->pcache);
      ffmpegdec->pcache = NULL;
    }
    av_parser_close (ffmpegdec->pctx);
    ffmpegdec->pctx = NULL;
  }

  ffmpegdec->format.video.fps_n = -1;
  ffmpegdec->format.video.old_fps_n = -1;
}

/* with LOCK */
static gboolean
gst_ffmpegdec_open (GstFFMpegDec * ffmpegdec)
{
  GstFFMpegDecClass *oclass;

  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  if (gst_ffmpeg_avcodec_open (ffmpegdec->context, oclass->in_plugin) < 0)
    goto could_not_open;

  ffmpegdec->opened = TRUE;
  ffmpegdec->is_realvideo = FALSE;

  GST_LOG_OBJECT (ffmpegdec, "Opened ffmpeg codec %s, id %d",
      oclass->in_plugin->name, oclass->in_plugin->id);

  /* open a parser if we can */
  switch (oclass->in_plugin->id) {
    case CODEC_ID_MPEG4:
    case CODEC_ID_MJPEG:
    case CODEC_ID_MP3:
  case CODEC_ID_VC1:
      GST_LOG_OBJECT (ffmpegdec, "not using parser, blacklisted codec");
      ffmpegdec->pctx = NULL;
      break;
    case CODEC_ID_H264:
      /* For H264, only use a parser if there is no context data, if there is, 
       * we're talking AVC */
      if (ffmpegdec->context->extradata_size == 0) {
        GST_LOG_OBJECT (ffmpegdec, "H264 with no extradata, creating parser");
        ffmpegdec->pctx = av_parser_init (oclass->in_plugin->id);
      } else {
        GST_LOG_OBJECT (ffmpegdec,
            "H264 with extradata implies framed data - not using parser");
        ffmpegdec->pctx = NULL;
      }
      break;
    case CODEC_ID_RV10:
    case CODEC_ID_RV30:
    case CODEC_ID_RV20:
    case CODEC_ID_RV40:
      ffmpegdec->is_realvideo = TRUE;
      break;
    default:
      ffmpegdec->pctx = av_parser_init (oclass->in_plugin->id);
      if (ffmpegdec->pctx)
        GST_LOG_OBJECT (ffmpegdec, "Using parser %p", ffmpegdec->pctx);
      else
        GST_LOG_OBJECT (ffmpegdec, "No parser for codec");
      break;
  }

  switch (oclass->in_plugin->type) {
    case CODEC_TYPE_VIDEO:
      ffmpegdec->format.video.width = 0;
      ffmpegdec->format.video.height = 0;
      ffmpegdec->format.video.clip_width = -1;
      ffmpegdec->format.video.clip_height = -1;
      ffmpegdec->format.video.pix_fmt = PIX_FMT_NB;
      break;
    case CODEC_TYPE_AUDIO:
      ffmpegdec->format.audio.samplerate = 0;
      ffmpegdec->format.audio.channels = 0;
      break;
    default:
      break;
  }

  /* out-of-order incoming buffer handling */
  if ((oclass->in_plugin->id == CODEC_ID_H264)
      && (ffmpegdec->context->extradata_size != 0))
    ffmpegdec->outoforder = TRUE;

  ffmpegdec->next_ts = GST_CLOCK_TIME_NONE;
  ffmpegdec->last_buffer = NULL;
  /* FIXME, reset_qos holds the LOCK */
  ffmpegdec->proportion = 0.0;
  ffmpegdec->earliest_time = -1;

  return TRUE;

  /* ERRORS */
could_not_open:
  {
    gst_ffmpegdec_close (ffmpegdec);
    GST_DEBUG_OBJECT (ffmpegdec, "ffdec_%s: Failed to open FFMPEG codec",
        oclass->in_plugin->name);
    return FALSE;
  }
}

static gboolean
gst_ffmpegdec_setcaps (GstPad * pad, GstCaps * caps)
{
  GstFFMpegDec *ffmpegdec;
  GstFFMpegDecClass *oclass;
  GstStructure *structure;
  const GValue *par;
  const GValue *fps;
  gboolean ret = TRUE;

  ffmpegdec = (GstFFMpegDec *) (gst_pad_get_parent (pad));
  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  GST_DEBUG_OBJECT (pad, "setcaps called");

  GST_OBJECT_LOCK (ffmpegdec);

  /* close old session */
  gst_ffmpegdec_close (ffmpegdec);

  /* set defaults */
  avcodec_get_context_defaults (ffmpegdec->context);

  /* set buffer functions */
  ffmpegdec->context->get_buffer = gst_ffmpegdec_get_buffer;
  ffmpegdec->context->release_buffer = gst_ffmpegdec_release_buffer;

  /* get size and so */
  gst_ffmpeg_caps_with_codecid (oclass->in_plugin->id,
      oclass->in_plugin->type, caps, ffmpegdec->context);

  if (!ffmpegdec->context->time_base.den || !ffmpegdec->context->time_base.num) {
    GST_DEBUG_OBJECT (ffmpegdec, "forcing 25/1 framerate");
    ffmpegdec->context->time_base.num = 1;
    ffmpegdec->context->time_base.den = 25;
  }

  /* get pixel aspect ratio if it's set */
  structure = gst_caps_get_structure (caps, 0);

  par = gst_structure_get_value (structure, "pixel-aspect-ratio");
  if (par) {
    GST_DEBUG_OBJECT (ffmpegdec, "sink caps have pixel-aspect-ratio of %d:%d",
        gst_value_get_fraction_numerator (par),
        gst_value_get_fraction_denominator (par));
    /* should be NULL */
    if (ffmpegdec->par)
      g_free (ffmpegdec->par);
    ffmpegdec->par = g_new0 (GValue, 1);
    gst_value_init_and_copy (ffmpegdec->par, par);
  }

  /* get the framerate from incomming caps. fps_n is set to -1 when
   * there is no valid framerate */
  fps = gst_structure_get_value (structure, "framerate");
  if (fps != NULL && GST_VALUE_HOLDS_FRACTION (fps)) {
    ffmpegdec->format.video.fps_n = gst_value_get_fraction_numerator (fps);
    ffmpegdec->format.video.fps_d = gst_value_get_fraction_denominator (fps);
    GST_DEBUG_OBJECT (ffmpegdec, "Using framerate %d/%d from incoming caps",
        ffmpegdec->format.video.fps_n, ffmpegdec->format.video.fps_d);
  } else {
    ffmpegdec->format.video.fps_n = -1;
    GST_DEBUG_OBJECT (ffmpegdec, "Using framerate from codec");
  }

  if (oclass->in_plugin->id != CODEC_ID_H264) {
    /* do *not* draw edges */
    ffmpegdec->context->flags |= CODEC_FLAG_EMU_EDGE;
  }

  /* workaround encoder bugs */
  ffmpegdec->context->workaround_bugs |= FF_BUG_AUTODETECT;

  ffmpegdec->context->error_resilience = 1;

  /* for slow cpus */
  ffmpegdec->context->lowres = ffmpegdec->lowres;
  ffmpegdec->context->hurry_up = ffmpegdec->hurry_up;

  /* open codec - we don't select an output pix_fmt yet,
   * simply because we don't know! We only get it
   * during playback... */
  if (!gst_ffmpegdec_open (ffmpegdec))
    goto open_failed;

  /* clipping region */
  gst_structure_get_int (structure, "width",
      &ffmpegdec->format.video.clip_width);
  gst_structure_get_int (structure, "height",
      &ffmpegdec->format.video.clip_height);

done:
  GST_OBJECT_UNLOCK (ffmpegdec);

  gst_object_unref (ffmpegdec);

  return ret;

  /* ERRORS */
open_failed:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "Failed to open");
    if (ffmpegdec->par) {
      g_free (ffmpegdec->par);
      ffmpegdec->par = NULL;
    }
    ret = FALSE;
    goto done;
  }
}

static int
gst_ffmpegdec_get_buffer (AVCodecContext * context, AVFrame * picture)
{
  GstBuffer *buf = NULL;
  gulong bufsize = 0;
  GstFFMpegDec *ffmpegdec;
  int width;
  int height;

  ffmpegdec = (GstFFMpegDec *) context->opaque;

  width = context->width;
  height = context->height;

  switch (context->codec_type) {
    case CODEC_TYPE_VIDEO:
    /* some ffmpeg video plugins don't see the point in setting codec_type ... */
    case CODEC_TYPE_UNKNOWN:
      avcodec_align_dimensions (context, &width, &height);

      bufsize = avpicture_get_size (context->pix_fmt, width, height);

      if ((width != context->width) || (height != context->height) || 1) {
#ifdef FORCE_OUR_GET_BUFFER
        context->width = width;
        context->height = height;
#else
        /* revert to ffmpeg's default functions */
        ffmpegdec->context->get_buffer = avcodec_default_get_buffer;
        ffmpegdec->context->release_buffer = avcodec_default_release_buffer;

        return avcodec_default_get_buffer (context, picture);
#endif
      }

      if (!gst_ffmpegdec_negotiate (ffmpegdec)) {
        GST_ELEMENT_ERROR (ffmpegdec, CORE, NEGOTIATION, (NULL),
            ("Failed to link ffmpeg decoder to next element"));
        return avcodec_default_get_buffer (context, picture);
      }

      if (gst_pad_alloc_buffer_and_set_caps (ffmpegdec->srcpad,
              GST_BUFFER_OFFSET_NONE, bufsize, GST_PAD_CAPS (ffmpegdec->srcpad),
              &buf) != GST_FLOW_OK)
        return -1;
      ffmpegdec->last_buffer = buf;

      gst_ffmpeg_avpicture_fill ((AVPicture *) picture,
          GST_BUFFER_DATA (buf),
          context->pix_fmt, context->width, context->height);
      break;
    case CODEC_TYPE_AUDIO:
    default:
      g_assert_not_reached ();
      break;
  }

  /* tell ffmpeg we own this buffer
   *
   * we also use an evil hack (keep buffer in base[0])
   * to keep a reference to the buffer in release_buffer(),
   * so that we can ref() it here and unref() it there
   * so that we don't need to copy data */
  picture->type = FF_BUFFER_TYPE_USER;
  picture->age = G_MAXINT;
  gst_buffer_ref (buf);
  picture->opaque = buf;

  GST_LOG_OBJECT (ffmpegdec, "END");

  return 0;
}

static void
gst_ffmpegdec_release_buffer (AVCodecContext * context, AVFrame * picture)
{
  gint i;
  GstBuffer *buf;
  GstFFMpegDec *ffmpegdec;

  g_return_if_fail (picture->type == FF_BUFFER_TYPE_USER);

  buf = GST_BUFFER (picture->opaque);
  g_return_if_fail (buf != NULL);

  ffmpegdec = (GstFFMpegDec *) context->opaque;

  if (buf == ffmpegdec->last_buffer)
    ffmpegdec->last_buffer = NULL;
  gst_buffer_unref (buf);

  picture->opaque = NULL;

  /* zero out the reference in ffmpeg */
  for (i = 0; i < 4; i++) {
    picture->data[i] = NULL;
    picture->linesize[i] = 0;
  }
}

static void
gst_ffmpegdec_add_pixel_aspect_ratio (GstFFMpegDec * ffmpegdec,
    GstStructure * s)
{
  gboolean demuxer_par_set = FALSE;
  gboolean decoder_par_set = FALSE;
  gint demuxer_num = 1, demuxer_denom = 1;
  gint decoder_num = 1, decoder_denom = 1;

  GST_OBJECT_LOCK (ffmpegdec);

  if (ffmpegdec->par) {
    demuxer_num = gst_value_get_fraction_numerator (ffmpegdec->par);
    demuxer_denom = gst_value_get_fraction_denominator (ffmpegdec->par);
    demuxer_par_set = TRUE;
    GST_DEBUG_OBJECT (ffmpegdec, "Demuxer PAR: %d:%d", demuxer_num,
        demuxer_denom);
  }

  if (ffmpegdec->context->sample_aspect_ratio.num &&
      ffmpegdec->context->sample_aspect_ratio.den) {
    decoder_num = ffmpegdec->context->sample_aspect_ratio.num;
    decoder_denom = ffmpegdec->context->sample_aspect_ratio.den;
    decoder_par_set = TRUE;
    GST_DEBUG_OBJECT (ffmpegdec, "Decoder PAR: %d:%d", decoder_num,
        decoder_denom);
  }

  GST_OBJECT_UNLOCK (ffmpegdec);

  if (!demuxer_par_set && !decoder_par_set)
    goto no_par;

  if (demuxer_par_set && !decoder_par_set)
    goto use_demuxer_par;

  if (decoder_par_set && !demuxer_par_set)
    goto use_decoder_par;

  /* Both the demuxer and the decoder provide a PAR. If one of
   * the two PARs is 1:1 and the other one is not, use the one
   * that is not 1:1. If both are non-1:1, use the pixel aspect
   * ratio provided by the codec */
  if (demuxer_num == demuxer_denom && decoder_num != decoder_denom)
    goto use_decoder_par;

  if (decoder_num == decoder_denom && demuxer_num != demuxer_denom)
    goto use_demuxer_par;

  /* fall through and use decoder pixel aspect ratio */
use_decoder_par:
  {
    GST_DEBUG_OBJECT (ffmpegdec,
        "Setting decoder provided pixel-aspect-ratio of %u:%u", decoder_num,
        decoder_denom);
    gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, decoder_num,
        decoder_denom, NULL);
    return;
  }

use_demuxer_par:
  {
    GST_DEBUG_OBJECT (ffmpegdec,
        "Setting demuxer provided pixel-aspect-ratio of %u:%u", demuxer_num,
        demuxer_denom);
    gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, demuxer_num,
        demuxer_denom, NULL);
    return;
  }
no_par:
  {
    GST_DEBUG_OBJECT (ffmpegdec,
        "Neither demuxer nor codec provide a pixel-aspect-ratio");
    return;
  }
}

static void
gst_ffmpegdec_save_incoming_values (GstFFMpegDec * ffmpegdec,
    GstClockTime timestamp, GstClockTime duration)
{
  GST_LOG_OBJECT (ffmpegdec,
      "BEFORE timestamp:%" GST_TIME_FORMAT "/%" GST_TIME_FORMAT " duration:%"
      GST_TIME_FORMAT "/%" GST_TIME_FORMAT, GST_TIME_ARGS (ffmpegdec->tstamp1),
      GST_TIME_ARGS (ffmpegdec->tstamp2), GST_TIME_ARGS (ffmpegdec->dur1),
      GST_TIME_ARGS (ffmpegdec->dur2));

  /* shift previous new values to oldest */
  if (ffmpegdec->tstamp2 != GST_CLOCK_TIME_NONE)
    ffmpegdec->tstamp1 = ffmpegdec->tstamp2;
  ffmpegdec->dur1 = ffmpegdec->dur2;

  /* store new values */
  ffmpegdec->tstamp2 = timestamp;
  ffmpegdec->dur2 = duration;

  GST_LOG_OBJECT (ffmpegdec,
      "AFTER timestamp:%" GST_TIME_FORMAT "/%" GST_TIME_FORMAT " duration:%"
      GST_TIME_FORMAT "/%" GST_TIME_FORMAT, GST_TIME_ARGS (ffmpegdec->tstamp1),
      GST_TIME_ARGS (ffmpegdec->tstamp2), GST_TIME_ARGS (ffmpegdec->dur1),
      GST_TIME_ARGS (ffmpegdec->dur2));

}

static void
gst_ffmpegdec_get_best_values (GstFFMpegDec * ffmpegdec,
    GstClockTime * timestamp, GstClockTime * duration)
{
  /* Best timestamp is the smallest valid timestamp */
  if (ffmpegdec->tstamp1 == GST_CLOCK_TIME_NONE) {
    *timestamp = ffmpegdec->tstamp2;
    ffmpegdec->tstamp2 = GST_CLOCK_TIME_NONE;
  } else if (ffmpegdec->tstamp2 == GST_CLOCK_TIME_NONE) {
    *timestamp = ffmpegdec->tstamp1;
    ffmpegdec->tstamp1 = GST_CLOCK_TIME_NONE;
  } else if (ffmpegdec->tstamp1 < ffmpegdec->tstamp2) {
    *timestamp = ffmpegdec->tstamp1;
    ffmpegdec->tstamp1 = GST_CLOCK_TIME_NONE;
  } else {
    *timestamp = ffmpegdec->tstamp2;
    ffmpegdec->tstamp2 = GST_CLOCK_TIME_NONE;
  }

  /* Best duration is the oldest valid one */
  if (ffmpegdec->dur1 == GST_CLOCK_TIME_NONE) {
    *duration = ffmpegdec->dur2;
    ffmpegdec->dur2 = GST_CLOCK_TIME_NONE;
  } else {
    *duration = ffmpegdec->dur1;
    ffmpegdec->dur1 = GST_CLOCK_TIME_NONE;
  }

  GST_LOG_OBJECT (ffmpegdec,
      "Returning timestamp:%" GST_TIME_FORMAT " duration:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (*timestamp), GST_TIME_ARGS (*duration));
}

static gboolean
gst_ffmpegdec_negotiate (GstFFMpegDec * ffmpegdec)
{
  GstFFMpegDecClass *oclass;
  GstCaps *caps;

  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  switch (oclass->in_plugin->type) {
    case CODEC_TYPE_VIDEO:
      if (ffmpegdec->format.video.width == ffmpegdec->context->width &&
          ffmpegdec->format.video.height == ffmpegdec->context->height &&
          ffmpegdec->format.video.fps_n == ffmpegdec->format.video.old_fps_n &&
          ffmpegdec->format.video.fps_d == ffmpegdec->format.video.old_fps_d &&
          ffmpegdec->format.video.pix_fmt == ffmpegdec->context->pix_fmt)
        return TRUE;
      GST_DEBUG_OBJECT (ffmpegdec,
          "Renegotiating video from %dx%d@ %d/%d fps to %dx%d@ %d/%d fps",
          ffmpegdec->format.video.width, ffmpegdec->format.video.height,
          ffmpegdec->format.video.old_fps_n, ffmpegdec->format.video.old_fps_n,
          ffmpegdec->context->width, ffmpegdec->context->height,
          ffmpegdec->format.video.fps_n, ffmpegdec->format.video.fps_d);
      ffmpegdec->format.video.width = ffmpegdec->context->width;
      ffmpegdec->format.video.height = ffmpegdec->context->height;
      ffmpegdec->format.video.old_fps_n = ffmpegdec->format.video.fps_n;
      ffmpegdec->format.video.old_fps_d = ffmpegdec->format.video.fps_d;
      ffmpegdec->format.video.pix_fmt = ffmpegdec->context->pix_fmt;
      break;
    case CODEC_TYPE_AUDIO:
      if (ffmpegdec->format.audio.samplerate ==
          ffmpegdec->context->sample_rate &&
          ffmpegdec->format.audio.channels == ffmpegdec->context->channels)
        return TRUE;
      GST_DEBUG_OBJECT (ffmpegdec,
          "Renegotiating audio from %dHz@%dchannels to %dHz@%dchannels",
          ffmpegdec->format.audio.samplerate, ffmpegdec->format.audio.channels,
          ffmpegdec->context->sample_rate, ffmpegdec->context->channels);
      ffmpegdec->format.audio.samplerate = ffmpegdec->context->sample_rate;
      ffmpegdec->format.audio.channels = ffmpegdec->context->channels;
      break;
    default:
      break;
  }

  caps = gst_ffmpeg_codectype_to_caps (oclass->in_plugin->type,
      ffmpegdec->context, oclass->in_plugin->id);

  if (caps == NULL)
    goto no_caps;

  switch (oclass->in_plugin->type) {
    case CODEC_TYPE_VIDEO:
    {
      gint width, height;

      width = ffmpegdec->format.video.clip_width;
      height = ffmpegdec->format.video.clip_height;

      if (width != -1 && height != -1) {
        gst_caps_set_simple (caps,
            "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, NULL);
      }
      /* If a demuxer provided a framerate then use it (#313970) */
      if (ffmpegdec->format.video.fps_n != -1) {
        gst_caps_set_simple (caps, "framerate",
            GST_TYPE_FRACTION, ffmpegdec->format.video.fps_n,
            ffmpegdec->format.video.fps_d, NULL);
      }
      gst_ffmpegdec_add_pixel_aspect_ratio (ffmpegdec,
          gst_caps_get_structure (caps, 0));
      break;
    }
    case CODEC_TYPE_AUDIO:
    {
      break;
    }
    default:
      break;
  }

  if (!gst_pad_set_caps (ffmpegdec->srcpad, caps))
    goto caps_failed;

  gst_caps_unref (caps);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_ELEMENT_ERROR (ffmpegdec, CORE, NEGOTIATION, (NULL),
        ("could not find caps for codec (%s), unknown type",
            oclass->in_plugin->name));
    return FALSE;
  }
caps_failed:
  {
    GST_ELEMENT_ERROR (ffmpegdec, CORE, NEGOTIATION, (NULL),
        ("Could not set caps for ffmpeg decoder (%s), not fixed?",
            oclass->in_plugin->name));
    gst_caps_unref (caps);

    return FALSE;
  }
}

/* perform qos calculations before decoding the next frame.
 *
 * Sets the hurry_up flag and if things are really bad, skips to the next
 * keyframe.
 * 
 * Returns TRUE if the frame should be decoded, FALSE if the frame can be dropped
 * entirely.
 */
static gboolean
gst_ffmpegdec_do_qos (GstFFMpegDec * ffmpegdec, GstClockTime timestamp,
    gboolean * mode_switch)
{
  GstClockTimeDiff diff;
  gdouble proportion;
  GstClockTime qostime, earliest_time;

  *mode_switch = FALSE;

  /* no timestamp, can't do QoS */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (timestamp)))
    goto no_qos;

  /* get latest QoS observation values */
  gst_ffmpegdec_read_qos (ffmpegdec, &proportion, &earliest_time);

  /* skip qos if we have no observation (yet) */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (earliest_time))) {
    /* no hurry_up initialy */
    ffmpegdec->context->hurry_up = 0;
    goto no_qos;
  }

  /* qos is done on running time */
  qostime = gst_segment_to_running_time (&ffmpegdec->segment, GST_FORMAT_TIME,
      timestamp);

  /* see how our next timestamp relates to the latest qos timestamp. negative
   * values mean we are early, positive values mean we are too late. */
  diff = GST_CLOCK_DIFF (qostime, earliest_time);

  GST_DEBUG_OBJECT (ffmpegdec, "QOS: qostime %" GST_TIME_FORMAT
      ", earliest %" GST_TIME_FORMAT, GST_TIME_ARGS (qostime),
      GST_TIME_ARGS (earliest_time));

  /* if we using less than 40% of the available time, we can try to
   * speed up again when we were slow. */
  if (proportion < 0.4 && diff < 0) {
    goto normal_mode;
  } else {
    /* if we're more than two seconds late, switch to the next keyframe */
    /* FIXME, let the demuxer decide what's the best since we might be dropping
     * a lot of frames when the keyframe is far away or we even might not get a new
     * keyframe at all.. */
    if (diff > ((GstClockTimeDiff) GST_SECOND * 2)
        && !ffmpegdec->waiting_for_key) {
      goto skip_to_keyframe;
    } else if (diff >= 0) {
      /* we're too slow, try to speed up */
      if (ffmpegdec->waiting_for_key) {
        /* we were waiting for a keyframe, that's ok */
        goto skipping;
      }
      /* switch to hurry_up mode */
      goto hurry_up;
    }
  }

no_qos:
  return TRUE;

skipping:
  {
    return FALSE;
  }
normal_mode:
  {
    if (ffmpegdec->context->hurry_up != 0) {
      ffmpegdec->context->hurry_up = 0;
      *mode_switch = TRUE;
      GST_DEBUG_OBJECT (ffmpegdec, "QOS: normal mode %g < 0.4", proportion);
    }
    return TRUE;
  }
skip_to_keyframe:
  {
    ffmpegdec->context->hurry_up = 1;
    ffmpegdec->waiting_for_key = TRUE;
    *mode_switch = TRUE;
    GST_DEBUG_OBJECT (ffmpegdec,
        "QOS: keyframe, %" G_GINT64_FORMAT " > GST_SECOND/2", diff);
    /* we can skip the current frame */
    return FALSE;
  }
hurry_up:
  {
    if (ffmpegdec->context->hurry_up != 1) {
      ffmpegdec->context->hurry_up = 1;
      *mode_switch = TRUE;
      GST_DEBUG_OBJECT (ffmpegdec,
          "QOS: hurry up, diff %" G_GINT64_FORMAT " >= 0", diff);
    }
    return TRUE;
  }
}

/* returns TRUE if buffer is within segment, else FALSE.
 * if Buffer is on segment border, it's timestamp and duration will be clipped */
static gboolean
clip_video_buffer (GstFFMpegDec * dec, GstBuffer * buf, GstClockTime in_ts,
    GstClockTime in_dur)
{
  gboolean res = TRUE;
  gint64 cstart, cstop;
  GstClockTime stop;

  GST_LOG_OBJECT (dec,
      "timestamp:%" GST_TIME_FORMAT " , duration:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (in_ts), GST_TIME_ARGS (in_dur));

  /* can't clip without TIME segment */
  if (G_UNLIKELY (dec->segment.format != GST_FORMAT_TIME))
    goto beach;

  /* we need a start time */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (in_ts)))
    goto beach;

  /* generate valid stop, if duration unknown, we have unknown stop */
  stop =
      GST_CLOCK_TIME_IS_VALID (in_dur) ? (in_ts + in_dur) : GST_CLOCK_TIME_NONE;

  /* now clip */
  res =
      gst_segment_clip (&dec->segment, GST_FORMAT_TIME, in_ts, stop, &cstart,
      &cstop);
  if (G_UNLIKELY (!res))
    goto beach;

  /* we're pretty sure the duration of this buffer is not till the end of this
   * segment (which _clip will assume when the stop is -1) */
  if (stop == GST_CLOCK_TIME_NONE)
    cstop = GST_CLOCK_TIME_NONE;

  /* update timestamp and possibly duration if the clipped stop time is
   * valid */
  GST_BUFFER_TIMESTAMP (buf) = cstart;
  if (GST_CLOCK_TIME_IS_VALID (cstop))
    GST_BUFFER_DURATION (buf) = cstop - cstart;

  GST_LOG_OBJECT (dec,
      "clipped timestamp:%" GST_TIME_FORMAT " , duration:%" GST_TIME_FORMAT,
      GST_TIME_ARGS (cstart), GST_TIME_ARGS (GST_BUFFER_DURATION (buf)));

beach:
  GST_LOG_OBJECT (dec, "%sdropping", (res ? "not " : ""));
  return res;
}


/* figure out if the current picture is a keyframe, return TRUE if that is
 * the case. */
static gboolean
check_keyframe (GstFFMpegDec * ffmpegdec)
{
  GstFFMpegDecClass *oclass;
  gboolean is_itype = FALSE;
  gboolean is_reference = FALSE;
  gboolean iskeyframe;

  /* figure out if we are dealing with a keyframe */
  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  is_itype = (ffmpegdec->picture->pict_type == FF_I_TYPE);
  is_reference = (ffmpegdec->picture->reference == 1);

  iskeyframe = (is_itype || is_reference || ffmpegdec->picture->key_frame)
      || (oclass->in_plugin->id == CODEC_ID_INDEO3)
      || (oclass->in_plugin->id == CODEC_ID_MSZH)
      || (oclass->in_plugin->id == CODEC_ID_ZLIB)
      || (oclass->in_plugin->id == CODEC_ID_VP3)
      || (oclass->in_plugin->id == CODEC_ID_HUFFYUV);

  GST_LOG_OBJECT (ffmpegdec,
      "current picture: type: %d, is_keyframe:%d, is_itype:%d, is_reference:%d",
      ffmpegdec->picture->pict_type, iskeyframe, is_itype, is_reference);

  return iskeyframe;
}

/* get an outbuf buffer with the current picture */
static GstFlowReturn
get_output_buffer (GstFFMpegDec * ffmpegdec, GstBuffer ** outbuf)
{
  GstFlowReturn ret;

  ret = GST_FLOW_ERROR;
  *outbuf = NULL;

  /* libavcodec constantly crashes on stupid buffer allocation
   * errors inside. This drives me crazy, so we let it allocate
   * its own buffers and copy to our own buffer afterwards... */
  /* BUFFER CREATION */
  if (ffmpegdec->picture->opaque != NULL) {
    *outbuf = (GstBuffer *) ffmpegdec->picture->opaque;
    if (*outbuf == ffmpegdec->last_buffer)
      ffmpegdec->last_buffer = NULL;
    if (*outbuf != NULL)
      ret = GST_FLOW_OK;
  } else {
    AVPicture pic;
    gint fsize;
    gint width, height;

    /* see if we need renegotiation */
    if (G_UNLIKELY (!gst_ffmpegdec_negotiate (ffmpegdec)))
      goto negotiate_failed;

    /* figure out size of output buffer */
    if ((width = ffmpegdec->format.video.clip_width) == -1)
      width = ffmpegdec->context->width;
    if ((height = ffmpegdec->format.video.clip_height) == -1)
      height = ffmpegdec->context->height;

    fsize = gst_ffmpeg_avpicture_get_size (ffmpegdec->context->pix_fmt,
        width, height);

    if (!ffmpegdec->context->palctrl) {
      ret = gst_pad_alloc_buffer_and_set_caps (ffmpegdec->srcpad,
          GST_BUFFER_OFFSET_NONE, fsize,
          GST_PAD_CAPS (ffmpegdec->srcpad), outbuf);
      if (G_UNLIKELY (ret != GST_FLOW_OK))
        goto alloc_failed;

    } else {
      /* for paletted data we can't use pad_alloc_buffer(), because
       * fsize contains the size of the palette, so the overall size
       * is bigger than ffmpegcolorspace's unit size, which will
       * prompt GstBaseTransform to complain endlessly ... */
      *outbuf = gst_buffer_new_and_alloc (fsize);
      gst_buffer_set_caps (*outbuf, GST_PAD_CAPS (ffmpegdec->srcpad));
      ret = GST_FLOW_OK;
    }

    /* original ffmpeg code does not handle odd sizes correctly.
     * This patched up version does */
    gst_ffmpeg_avpicture_fill (&pic, GST_BUFFER_DATA (*outbuf),
        ffmpegdec->context->pix_fmt, width, height);

    /* the original convert function did not do the right thing, this
     * is a patched up version that adjust widht/height so that the
     * ffmpeg one works correctly. */
    gst_ffmpeg_img_convert (&pic, ffmpegdec->context->pix_fmt,
        (AVPicture *) ffmpegdec->picture,
        ffmpegdec->context->pix_fmt, width, height);
  }
  return ret;

  /* special cases */
negotiate_failed:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "negotiate failed");
    return GST_FLOW_NOT_NEGOTIATED;
  }
alloc_failed:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "pad_alloc failed");
    return ret;
  }
}

/* gst_ffmpegdec_[video|audio]_frame:
 * ffmpegdec:
 * data: pointer to the data to decode
 * size: size of data in bytes
 * in_timestamp: incoming timestamp.
 * in_duration: incoming duration.
 * outbuf: outgoing buffer. Different from NULL ONLY if it contains decoded data.
 * ret: Return flow.
 *
 * Returns: number of bytes used in decoding. The check for successful decode is
 *   outbuf being non-NULL.
 */

static gint
gst_ffmpegdec_video_frame (GstFFMpegDec * ffmpegdec,
    guint8 * data, guint size,
    GstClockTime in_timestamp, GstClockTime in_duration,
    GstBuffer ** outbuf, GstFlowReturn * ret)
{
  gint len = -1;
  gint have_data;
  gboolean iskeyframe;
  gboolean mode_switch;

  *ret = GST_FLOW_OK;
  *outbuf = NULL;

  ffmpegdec->context->opaque = ffmpegdec;

  /* incoming out-of-order buffer timestamp buffering */
  if (ffmpegdec->outoforder)
    gst_ffmpegdec_save_incoming_values (ffmpegdec, in_timestamp, in_duration);

  /* run QoS code, returns FALSE if we can skip decoding this
   * frame entirely. */
  if (G_UNLIKELY (!gst_ffmpegdec_do_qos (ffmpegdec, in_timestamp,
              &mode_switch)))
    goto drop_qos;

  /* in case we skip frames */
  ffmpegdec->picture->pict_type = -1;

  if (ffmpegdec->is_realvideo && data != NULL) {
    gint slice_count;
    gint i;

    /* setup the slice table for realvideo */
    if (ffmpegdec->context->slice_offset == NULL)
      ffmpegdec->context->slice_offset = g_malloc (sizeof (guint32) * 1000);

    slice_count = (*data++) + 1;
    ffmpegdec->context->slice_count = slice_count;

    for (i = 0; i < slice_count; i++) {
      data += 4;
      ffmpegdec->context->slice_offset[i] = GST_READ_UINT32_LE (data);
      data += 4;
    }
  }

  /* now decode the frame */
  len = avcodec_decode_video (ffmpegdec->context,
      ffmpegdec->picture, &have_data, data, size);

  GST_DEBUG_OBJECT (ffmpegdec, "after decode: len %d, have_data %d",
      len, have_data);

  /* when we are in hurry_up mode, don't complain when ffmpeg returned
   * no data because we told it to skip stuff. */
  if (len < 0 && (mode_switch || ffmpegdec->context->hurry_up))
    len = 0;

  /* no data, we're done */
  if (len < 0 || have_data <= 0)
    goto beach;

  GST_DEBUG_OBJECT (ffmpegdec, "picture: pts %" G_GUINT64_FORMAT,
      ffmpegdec->picture->pts);
  GST_DEBUG_OBJECT (ffmpegdec, "picture: num %d",
      ffmpegdec->picture->coded_picture_number);
  GST_DEBUG_OBJECT (ffmpegdec, "picture: display %d",
      ffmpegdec->picture->display_picture_number);

  /* check if we are dealing with a keyframe here */
  iskeyframe = check_keyframe (ffmpegdec);

  /* when we're waiting for a keyframe, see if we have one or drop the current
   * non-keyframe */
  if (G_UNLIKELY (ffmpegdec->waiting_for_key)) {
    if (G_LIKELY (!iskeyframe))
      goto drop_non_keyframe;

    /* we have a keyframe, we can stop waiting for one */
    ffmpegdec->waiting_for_key = FALSE;
  }

  /* get a handle to the output buffer */
  *ret = get_output_buffer (ffmpegdec, outbuf);
  if (G_UNLIKELY (*ret != GST_FLOW_OK))
    goto no_output;

  /* Special handling for out-of-order incoming buffers */
  if (ffmpegdec->outoforder)
    gst_ffmpegdec_get_best_values (ffmpegdec, &in_timestamp, &in_duration);

  /*
   * Timestamps:
   *
   *  1) Copy parse context timestamp if present and valid (FIXME)
   *  2) Copy input timestamp if valid
   *  3) else interpolate from previous input timestamp
   */
#if 0
  /* this does not work reliably, for some files this works fine, for other
   * files it returns the same timestamp twice. Leaving the code here for when
   * the parsers are improved in ffmpeg. */
  if (ffmpegdec->pctx) {
    GST_DEBUG_OBJECT (ffmpegdec, "picture: ffpts %" G_GUINT64_FORMAT,
        ffmpegdec->pctx->pts);
    if (ffmpegdec->pctx->pts != AV_NOPTS_VALUE) {
      in_timestamp = gst_ffmpeg_time_ff_to_gst (ffmpegdec->pctx->pts,
          ffmpegdec->context->time_base);
    }
  }
#endif
  if (!GST_CLOCK_TIME_IS_VALID (in_timestamp)) {
    GST_LOG_OBJECT (ffmpegdec, "using timestamp returned by ffmpeg");
    /* Get (interpolated) timestamp from FFMPEG */
    in_timestamp = gst_ffmpeg_time_ff_to_gst ((guint64) ffmpegdec->picture->pts,
        ffmpegdec->context->time_base);
  }
  GST_BUFFER_TIMESTAMP (*outbuf) = in_timestamp;

  /*
   * Duration:
   *
   *  1) Copy input duration if valid
   *  2) else use input framerate
   *  3) else use ffmpeg framerate
   */
  if (!GST_CLOCK_TIME_IS_VALID (in_duration)) {
    /* if we have an input framerate, use that */
    if (ffmpegdec->format.video.fps_n != -1 &&
        (ffmpegdec->format.video.fps_n != 1000 &&
            ffmpegdec->format.video.fps_d != 1)) {
      GST_LOG_OBJECT (ffmpegdec, "using input framerate for duration");
      in_duration = gst_util_uint64_scale_int (GST_SECOND,
          ffmpegdec->format.video.fps_d, ffmpegdec->format.video.fps_n);
    } else {
      /* don't try to use the decoder's framerate when it seems a bit abnormal,
       * which we assume when den >= 1000... */
      if (ffmpegdec->context->time_base.num != 0 &&
          (ffmpegdec->context->time_base.den > 0 &&
              ffmpegdec->context->time_base.den < 1000)) {
        GST_LOG_OBJECT (ffmpegdec, "using decoder's framerate for duration");
        in_duration = gst_util_uint64_scale_int (GST_SECOND,
            ffmpegdec->context->time_base.num,
            ffmpegdec->context->time_base.den);
      } else {
        GST_LOG_OBJECT (ffmpegdec, "no valid duration found");
      }
    }
  } else {
    GST_LOG_OBJECT (ffmpegdec, "using in_duration");
  }

  /* Take repeat_pict into account */
  if (GST_CLOCK_TIME_IS_VALID (in_duration)) {
    in_duration += in_duration * ffmpegdec->picture->repeat_pict / 2;
  }
  GST_BUFFER_DURATION (*outbuf) = in_duration;

  /* palette is not part of raw video frame in gst and the size
   * of the outgoing buffer needs to be adjusted accordingly */
  if (ffmpegdec->context->palctrl != NULL)
    GST_BUFFER_SIZE (*outbuf) -= AVPALETTE_SIZE;

  /* now see if we need to clip the buffer against the segment boundaries. */
  if (G_UNLIKELY (!clip_video_buffer (ffmpegdec, *outbuf, in_timestamp,
              in_duration)))
    goto clipped;

  /* mark as keyframe or delta unit */
  if (!iskeyframe)
    GST_BUFFER_FLAG_SET (*outbuf, GST_BUFFER_FLAG_DELTA_UNIT);

beach:
  GST_DEBUG_OBJECT (ffmpegdec, "return flow %d, out %p, len %d",
      *ret, *outbuf, len);
  return len;

  /* special cases */
drop_qos:
  {
    GST_WARNING_OBJECT (ffmpegdec, "Dropping frame because of QoS");
    /* drop a frame, set discont on next buffer, pretend we decoded the complete
     * buffer */
    ffmpegdec->discont = TRUE;
    len = size;
    goto beach;
  }
drop_non_keyframe:
  {
    GST_WARNING_OBJECT (ffmpegdec, "Dropping non-keyframe (seek/init)");
    goto beach;
  }
no_output:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "no output buffer");
    len = -1;
    goto beach;
  }
clipped:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "buffer clipped");
    gst_buffer_unref (*outbuf);
    *outbuf = NULL;
    goto beach;
  }
}

/* returns TRUE if buffer is within segment, else FALSE.
 * if Buffer is on segment border, it's timestamp and duration will be clipped */
static gboolean
clip_audio_buffer (GstFFMpegDec * dec, GstBuffer * buf, GstClockTime in_ts,
    GstClockTime in_dur)
{
  GstClockTime stop;
  gint64 diff, ctime, cstop;
  gboolean res = TRUE;

  GST_LOG_OBJECT (dec,
      "timestamp:%" GST_TIME_FORMAT ", duration:%" GST_TIME_FORMAT
      ", size %u", GST_TIME_ARGS (in_ts), GST_TIME_ARGS (in_dur),
      GST_BUFFER_SIZE (buf));

  /* can't clip without TIME segment */
  if (G_UNLIKELY (dec->segment.format != GST_FORMAT_TIME))
    goto beach;

  /* we need a start time */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (in_ts)))
    goto beach;

  /* trust duration */
  stop = in_ts + in_dur;

  res = gst_segment_clip (&dec->segment, GST_FORMAT_TIME, in_ts, stop, &ctime,
      &cstop);
  if (G_UNLIKELY (!res))
    goto out_of_segment;

  /* see if some clipping happened */
  if (G_UNLIKELY ((diff = ctime - in_ts) > 0)) {
    /* bring clipped time to bytes */
    diff =
        gst_util_uint64_scale_int (diff, dec->format.audio.samplerate,
        GST_SECOND) * (2 * dec->format.audio.channels);

    GST_DEBUG_OBJECT (dec, "clipping start to %" GST_TIME_FORMAT " %"
        G_GINT64_FORMAT " bytes", GST_TIME_ARGS (ctime), diff);

    GST_BUFFER_SIZE (buf) -= diff;
    GST_BUFFER_DATA (buf) += diff;
  }
  if (G_UNLIKELY ((diff = stop - cstop) > 0)) {
    /* bring clipped time to bytes */
    diff =
        gst_util_uint64_scale_int (diff, dec->format.audio.samplerate,
        GST_SECOND) * (2 * dec->format.audio.channels);

    GST_DEBUG_OBJECT (dec, "clipping stop to %" GST_TIME_FORMAT " %"
        G_GINT64_FORMAT " bytes", GST_TIME_ARGS (cstop), diff);

    GST_BUFFER_SIZE (buf) -= diff;
  }
  GST_BUFFER_TIMESTAMP (buf) = ctime;
  GST_BUFFER_DURATION (buf) = cstop - ctime;

beach:
  GST_LOG_OBJECT (dec, "%sdropping", (res ? "not " : ""));
  return res;

  /* ERRORS */
out_of_segment:
  {
    GST_LOG_OBJECT (dec, "out of segment");
    goto beach;
  }
}

static gint
gst_ffmpegdec_audio_frame (GstFFMpegDec * ffmpegdec,
    guint8 * data, guint size,
    GstClockTime in_timestamp, GstClockTime in_duration,
    GstBuffer ** outbuf, GstFlowReturn * ret)
{
  gint len = -1;
  gint have_data = AVCODEC_MAX_AUDIO_FRAME_SIZE;

  GST_DEBUG_OBJECT (ffmpegdec,
      "size:%d, ts:%" GST_TIME_FORMAT ", dur:%" GST_TIME_FORMAT
      ", ffmpegdec->next_ts:%" GST_TIME_FORMAT, size,
      GST_TIME_ARGS (in_timestamp), GST_TIME_ARGS (in_duration),
      GST_TIME_ARGS (ffmpegdec->next_ts));

  /* outgoing buffer */
  if (!ffmpegdec->last_buffer)
    *outbuf = gst_buffer_new_and_alloc (AVCODEC_MAX_AUDIO_FRAME_SIZE);
  else {
    *outbuf = ffmpegdec->last_buffer;
    ffmpegdec->last_buffer = NULL;
  }

  len = avcodec_decode_audio2 (ffmpegdec->context,
      (int16_t *) GST_BUFFER_DATA (*outbuf), &have_data, data, size);
  GST_DEBUG_OBJECT (ffmpegdec,
      "Decode audio: len=%d, have_data=%d", len, have_data);

  if (len >= 0 && have_data > 0) {
    GST_DEBUG_OBJECT (ffmpegdec, "Creating output buffer");
    if (!gst_ffmpegdec_negotiate (ffmpegdec)) {
      gst_buffer_unref (*outbuf);
      *outbuf = NULL;
      len = -1;
      goto beach;
    }

    /* Buffer size */
    GST_BUFFER_SIZE (*outbuf) = have_data;

    /*
     * Timestamps:
     *
     *  1) Copy input timestamp if valid
     *  2) else interpolate from previous input timestamp
     */
    /* always take timestamps from the input buffer if any */
    if (!GST_CLOCK_TIME_IS_VALID (in_timestamp)) {
      in_timestamp = ffmpegdec->next_ts;
    }

    /*
     * Duration:
     *
     *  1) calculate based on number of samples
     */
    in_duration = gst_util_uint64_scale_int (have_data, GST_SECOND,
        2 * ffmpegdec->context->channels * ffmpegdec->context->sample_rate);

    GST_DEBUG_OBJECT (ffmpegdec,
        "Buffer created. Size:%d , timestamp:%" GST_TIME_FORMAT " , duration:%"
        GST_TIME_FORMAT, have_data,
        GST_TIME_ARGS (in_timestamp), GST_TIME_ARGS (in_duration));

    GST_BUFFER_TIMESTAMP (*outbuf) = in_timestamp;
    GST_BUFFER_DURATION (*outbuf) = in_duration;

    /* the next timestamp we'll use when interpolating */
    ffmpegdec->next_ts = in_timestamp + in_duration;

    /* now see if we need to clip the buffer against the segment boundaries. */
    if (G_UNLIKELY (!clip_audio_buffer (ffmpegdec, *outbuf, in_timestamp,
                in_duration)))
      goto clipped;

  } else if (len > 0 && have_data == 0) {
    /* cache output, because it may be used for caching (in-place) */
    ffmpegdec->last_buffer = *outbuf;
    *outbuf = NULL;
  } else {
    gst_buffer_unref (*outbuf);
    *outbuf = NULL;
  }

beach:
  GST_DEBUG_OBJECT (ffmpegdec, "return flow %d, out %p, len %d",
      *ret, *outbuf, len);
  return len;

  /* ERRORS */
clipped:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "buffer clipped");
    gst_buffer_unref (*outbuf);
    *outbuf = NULL;
    goto beach;
  }
}


/* gst_ffmpegdec_frame:
 * ffmpegdec:
 * data: pointer to the data to decode
 * size: size of data in bytes
 * got_data: 0 if no data was decoded, != 0 otherwise.
 * in_time: timestamp of data
 * in_duration: duration of data
 * ret: GstFlowReturn to return in the chain function
 *
 * Decode the given frame and pushes it downstream.
 *
 * Returns: Number of bytes used in decoding, -1 on error/failure.
 */

static gint
gst_ffmpegdec_frame (GstFFMpegDec * ffmpegdec,
    guint8 * data, guint size, gint * got_data,
    GstClockTime in_timestamp, GstClockTime in_duration, GstFlowReturn * ret)
{
  GstFFMpegDecClass *oclass;
  GstBuffer *outbuf = NULL;
  gint have_data = 0, len = 0;

  if (G_UNLIKELY (ffmpegdec->context->codec == NULL))
    goto no_codec;

  GST_LOG_OBJECT (ffmpegdec,
      "data:%p, size:%d, ts:%" GST_TIME_FORMAT ", dur:%" GST_TIME_FORMAT,
      data, size, GST_TIME_ARGS (in_timestamp), GST_TIME_ARGS (in_duration));

  *ret = GST_FLOW_OK;
  ffmpegdec->context->frame_number++;

  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  switch (oclass->in_plugin->type) {
    case CODEC_TYPE_VIDEO:
      len =
          gst_ffmpegdec_video_frame (ffmpegdec, data, size, in_timestamp,
          in_duration, &outbuf, ret);
      break;
    case CODEC_TYPE_AUDIO:
      len =
          gst_ffmpegdec_audio_frame (ffmpegdec, data, size, in_timestamp,
          in_duration, &outbuf, ret);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (outbuf)
    have_data = 1;

  if (len < 0 || have_data < 0) {
    GST_WARNING_OBJECT (ffmpegdec,
        "ffdec_%s: decoding error (len: %d, have_data: %d)",
        oclass->in_plugin->name, len, have_data);
    *got_data = 0;
    goto beach;
  } else if (len == 0 && have_data == 0) {
    *got_data = 0;
    goto beach;
  } else {
    /* this is where I lost my last clue on ffmpeg... */
    *got_data = 1;
  }

  if (outbuf) {
    GST_LOG_OBJECT (ffmpegdec,
        "Decoded data, now pushing buffer with timestamp %" GST_TIME_FORMAT
        " and duration %" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)));

    /* mark pending discont */
    if (ffmpegdec->discont) {
      GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DISCONT);
      ffmpegdec->discont = FALSE;
    }
    /* set caps */
    gst_buffer_set_caps (outbuf, GST_PAD_CAPS (ffmpegdec->srcpad));
    /* and off we go */
    *ret = gst_pad_push (ffmpegdec->srcpad, outbuf);
  } else {
    GST_DEBUG_OBJECT (ffmpegdec, "We didn't get a decoded buffer");
  }

beach:
  return len;

  /* ERRORS */
no_codec:
  {
    GST_ERROR_OBJECT (ffmpegdec, "no codec context");
    return -1;
  }
}

static void
gst_ffmpegdec_flush_pcache (GstFFMpegDec * ffmpegdec)
{
  if (ffmpegdec->pcache) {
    gst_buffer_unref (ffmpegdec->pcache);
    ffmpegdec->pcache = NULL;
  }
  if (ffmpegdec->pctx) {
    GstFFMpegDecClass *oclass;

    oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

    av_parser_close (ffmpegdec->pctx);
    ffmpegdec->pctx = av_parser_init (oclass->in_plugin->id);
  }
}

static gboolean
gst_ffmpegdec_sink_event (GstPad * pad, GstEvent * event)
{
  GstFFMpegDec *ffmpegdec;
  GstFFMpegDecClass *oclass;
  gboolean ret = FALSE;

  ffmpegdec = (GstFFMpegDec *) gst_pad_get_parent (pad);
  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  GST_DEBUG_OBJECT (ffmpegdec, "Handling %s event",
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:{
      if (oclass->in_plugin->capabilities & CODEC_CAP_DELAY) {
        gint have_data, len, try = 0;

        GST_LOG_OBJECT (ffmpegdec,
            "codec has delay capabilities, calling until ffmpeg has drained everything");

        do {
          GstFlowReturn ret;

          len = gst_ffmpegdec_frame (ffmpegdec, NULL, 0, &have_data,
              GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, &ret);
          if (len < 0 || have_data == 0)
            break;
        } while (try++ < 10);
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:{
      if (ffmpegdec->opened) {
        avcodec_flush_buffers (ffmpegdec->context);
      }
      ffmpegdec->next_ts = GST_CLOCK_TIME_NONE;
      gst_ffmpegdec_reset_qos (ffmpegdec);
      gst_ffmpegdec_flush_pcache (ffmpegdec);
      ffmpegdec->waiting_for_key = TRUE;
      gst_segment_init (&ffmpegdec->segment, GST_FORMAT_TIME);
      ffmpegdec->tstamp1 = ffmpegdec->tstamp2 = GST_CLOCK_TIME_NONE;
      ffmpegdec->dur1 = ffmpegdec->dur2 = GST_CLOCK_TIME_NONE;
      break;
    }
    case GST_EVENT_NEWSEGMENT:{
      gboolean update;
      GstFormat fmt;
      gint64 start, stop, time;
      gdouble rate, arate;

      gst_event_parse_new_segment_full (event, &update, &rate, &arate, &fmt,
          &start, &stop, &time);

      /* no negative rates for now */
      if (rate <= 0.0)
        goto newseg_wrong_rate;

      switch (fmt) {
        case GST_FORMAT_TIME:
          /* fine, our native segment format */
          break;
        case GST_FORMAT_BYTES:
        {
          gint bit_rate;

          bit_rate = ffmpegdec->context->bit_rate;

          /* convert to time or fail */
          if (!bit_rate)
            goto no_bitrate;

          GST_DEBUG_OBJECT (ffmpegdec, "bitrate: %d", bit_rate);

          /* convert values to TIME */
          if (start != -1)
            start = gst_util_uint64_scale_int (start, GST_SECOND, bit_rate);
          if (stop != -1)
            stop = gst_util_uint64_scale_int (stop, GST_SECOND, bit_rate);
          if (time != -1)
            time = gst_util_uint64_scale_int (time, GST_SECOND, bit_rate);

          /* unref old event */
          gst_event_unref (event);

          /* create new converted time segment */
          fmt = GST_FORMAT_TIME;
          /* FIXME, bitrate is not good enough too find a good stop, let's
           * hope start and time were 0... meh. */
          stop = -1;
          event = gst_event_new_new_segment (update, rate, fmt,
              start, stop, time);
          break;
        }
        default:
          /* invalid format */
          goto invalid_format;
      }

      GST_DEBUG_OBJECT (ffmpegdec,
          "NEWSEGMENT in time start %" GST_TIME_FORMAT " -- stop %"
          GST_TIME_FORMAT, GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

      /* and store the values */
      gst_segment_set_newsegment_full (&ffmpegdec->segment, update,
          rate, arate, fmt, start, stop, time);
      break;
    }
    default:
      break;
  }

  /* and push segment downstream */
  ret = gst_pad_push_event (ffmpegdec->srcpad, event);

done:
  gst_object_unref (ffmpegdec);

  return ret;

  /* ERRORS */
newseg_wrong_rate:
  {
    GST_WARNING_OBJECT (ffmpegdec, "negative rates not supported yet");
    gst_event_unref (event);
    goto done;
  }
no_bitrate:
  {
    GST_WARNING_OBJECT (ffmpegdec, "no bitrate to convert BYTES to TIME");
    gst_event_unref (event);
    goto done;
  }
invalid_format:
  {
    GST_WARNING_OBJECT (ffmpegdec, "unknown format received in NEWSEGMENT");
    gst_event_unref (event);
    goto done;
  }
}

static GstFlowReturn
gst_ffmpegdec_chain (GstPad * pad, GstBuffer * inbuf)
{
  GstFFMpegDec *ffmpegdec;
  GstFFMpegDecClass *oclass;
  guint8 *data, *bdata;
  gint size, bsize, len, have_data;
  GstFlowReturn ret = GST_FLOW_OK;
  guint left;
  GstClockTime in_timestamp, in_duration;
  GstClockTime next_timestamp, next_duration;
  GstClockTime pending_timestamp, pending_duration;

  ffmpegdec = (GstFFMpegDec *) (GST_PAD_PARENT (pad));

  if (G_UNLIKELY (!ffmpegdec->opened))
    goto not_negotiated;

  /* The discont flags marks a buffer that is not continuous with the previous
   * buffer. This means we need to clear whatever data we currently have. We
   * currently also wait for a new keyframe, which might be suboptimal in the
   * case of a network error, better show the errors than to drop all data.. */
  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_DISCONT))) {
    GST_DEBUG_OBJECT (ffmpegdec, "received DISCONT");
    gst_ffmpegdec_flush_pcache (ffmpegdec);
    avcodec_flush_buffers (ffmpegdec->context);
    ffmpegdec->waiting_for_key = TRUE;
    ffmpegdec->discont = TRUE;
    ffmpegdec->next_ts = GST_CLOCK_TIME_NONE;
  }

  oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));

  /* do early keyframe check pretty bad to rely on the keyframe flag in the
   * source for this as it might not even be parsed (UDP/file/..).  */
  if (G_UNLIKELY (ffmpegdec->waiting_for_key)) {
    if (GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_DELTA_UNIT) &&
        oclass->in_plugin->type != CODEC_TYPE_AUDIO)
      goto skip_keyframe;

    GST_DEBUG_OBJECT (ffmpegdec, "got keyframe");
    ffmpegdec->waiting_for_key = FALSE;
  }

  pending_timestamp = GST_BUFFER_TIMESTAMP (inbuf);
  pending_duration = GST_BUFFER_DURATION (inbuf);

  GST_LOG_OBJECT (ffmpegdec,
      "Received new data of size %d, ts:%" GST_TIME_FORMAT ", dur:%"
      GST_TIME_FORMAT, GST_BUFFER_SIZE (inbuf),
      GST_TIME_ARGS (pending_timestamp), GST_TIME_ARGS (pending_duration));

  /* parse cache joining. If there is cached data, its timestamp will be what we
   * send to the parse. */
  if (ffmpegdec->pcache) {
    /* keep track of how many bytes to consume before we can use the incomming
     * timestamp, which we have stored in pending_timestamp. */
    left = GST_BUFFER_SIZE (ffmpegdec->pcache);

    /* use timestamp and duration of what is in the cache */
    in_timestamp = GST_BUFFER_TIMESTAMP (ffmpegdec->pcache);
    in_duration = GST_BUFFER_DURATION (ffmpegdec->pcache);

    /* join with previous data */
    inbuf = gst_buffer_join (ffmpegdec->pcache, inbuf);

    GST_LOG_OBJECT (ffmpegdec,
        "joined parse cache, inbuf now has ts:%" GST_TIME_FORMAT,
        GST_TIME_ARGS (in_timestamp));

    /* no more cached data, we assume we can consume the complete cache */
    ffmpegdec->pcache = NULL;
  } else {
    /* no cache, input timestamp matches the buffer we try to decode */
    left = 0;
    in_timestamp = pending_timestamp;
    in_duration = pending_duration;
  }

  /* workarounds, functions write to buffers:
   *  libavcodec/svq1.c:svq1_decode_frame writes to the given buffer.
   *  libavcodec/svq3.c:svq3_decode_slice_header too.
   * ffmpeg devs know about it and will fix it (they said). */
  if (oclass->in_plugin->id == CODEC_ID_SVQ1 ||
      oclass->in_plugin->id == CODEC_ID_SVQ3) {
    inbuf = gst_buffer_make_writable (inbuf);
  }

  bdata = GST_BUFFER_DATA (inbuf);
  bsize = GST_BUFFER_SIZE (inbuf);

  do {
    /* parse, if at all possible */
    if (ffmpegdec->pctx) {
      gint res;
      gint64 ffpts;

      /* convert timestamp to ffmpeg timestamp */
      ffpts =
          gst_ffmpeg_time_gst_to_ff (in_timestamp,
          ffmpegdec->context->time_base);

      GST_LOG_OBJECT (ffmpegdec,
          "Calling av_parser_parse with ts:%" GST_TIME_FORMAT ", ffpts:%"
          G_GINT64_FORMAT, GST_TIME_ARGS (in_timestamp), ffpts);

      /* feed the parser */
      res = av_parser_parse (ffmpegdec->pctx, ffmpegdec->context,
          &data, &size, bdata, bsize, ffpts, ffpts);

      GST_LOG_OBJECT (ffmpegdec,
          "parser returned res %d and size %d", res, size);

      GST_LOG_OBJECT (ffmpegdec, "consuming %d bytes. Next ts at %d, ffpts:%"
          G_GINT64_FORMAT, size, left, ffmpegdec->pctx->pts);

      /* there is output, set pointers for next round. */
      bsize -= res;
      bdata += res;

      /* if there is no output, we must break and wait for more data. also the
       * timestamp in the context is not updated. */
      if (size == 0) {
        if (bsize > 0)
          continue;
        else
          break;
      }

      if (left <= size) {
        left = 0;
        /* activate the pending timestamp/duration and mark it invalid */
        next_timestamp = pending_timestamp;
        next_duration = pending_duration;

        pending_timestamp = GST_CLOCK_TIME_NONE;
        pending_duration = GST_CLOCK_TIME_NONE;

        GST_LOG_OBJECT (ffmpegdec,
            "activated ts:%" GST_TIME_FORMAT ", dur:%" GST_TIME_FORMAT,
            GST_TIME_ARGS (next_timestamp), GST_TIME_ARGS (next_duration));
      } else {
        left -= size;
        /* get new timestamp from the parser, this could be interpolated by the
         * parser. We lost track of duration here. */
        next_timestamp = gst_ffmpeg_time_ff_to_gst (ffmpegdec->pctx->pts,
            ffmpegdec->context->time_base);
        next_duration = GST_CLOCK_TIME_NONE;
        GST_LOG_OBJECT (ffmpegdec,
            "parse context next ts:%" GST_TIME_FORMAT ", ffpts:%"
            G_GINT64_FORMAT, GST_TIME_ARGS (next_timestamp), ffpts);
      }
    } else {
      data = bdata;
      size = bsize;
      /* after decoding this input buffer, we don't know the timestamp anymore
       * of any other decodable frame in this buffer, we let the interpolation
       * code work. */
      next_timestamp = GST_CLOCK_TIME_NONE;
      next_duration = GST_CLOCK_TIME_NONE;
    }

    /* decode a frame of audio/video now */
    len =
        gst_ffmpegdec_frame (ffmpegdec, data, size, &have_data, in_timestamp,
        in_duration, &ret);
    if (len < 0 || ret != GST_FLOW_OK)
      break;

    /* we decoded something, prepare to use next_timestamp in the next round */
    in_timestamp = next_timestamp;
    in_duration = next_duration;

    if (!ffmpegdec->pctx) {
      bsize -= len;
      bdata += len;
    }
    if (!have_data) {
      GST_LOG_OBJECT (ffmpegdec, "Decoding didn't return any data, breaking");
      break;
    }

    GST_LOG_OBJECT (ffmpegdec, "Before (while bsize>0).  bsize:%d , bdata:%p",
        bsize, bdata);
  } while (bsize > 0);

  /* keep left-over */
  if ((ffmpegdec->pctx || oclass->in_plugin->id == CODEC_ID_MP3) && bsize > 0) {
    GST_LOG_OBJECT (ffmpegdec,
        "Keeping %d bytes of data with timestamp %" GST_TIME_FORMAT, bsize,
        GST_TIME_ARGS (in_timestamp));

    ffmpegdec->pcache = gst_buffer_create_sub (inbuf,
        GST_BUFFER_SIZE (inbuf) - bsize, bsize);
    /* we keep timestamp, even though all we really know is that the correct
     * timestamp is not below the one from inbuf */
    GST_BUFFER_TIMESTAMP (ffmpegdec->pcache) = in_timestamp;
  } else if (bsize > 0) {
    GST_DEBUG_OBJECT (ffmpegdec, "Dropping %d bytes of data", bsize);
  }
  gst_buffer_unref (inbuf);

  return ret;

  /* ERRORS */
not_negotiated:
  {
    oclass = (GstFFMpegDecClass *) (G_OBJECT_GET_CLASS (ffmpegdec));
    GST_ELEMENT_ERROR (ffmpegdec, CORE, NEGOTIATION, (NULL),
        ("ffdec_%s: input format was not set before data start",
            oclass->in_plugin->name));
    gst_buffer_unref (inbuf);
    return GST_FLOW_NOT_NEGOTIATED;
  }
skip_keyframe:
  {
    GST_DEBUG_OBJECT (ffmpegdec, "skipping non keyframe");
    gst_buffer_unref (inbuf);
    return GST_FLOW_OK;
  }
}

static GstStateChangeReturn
gst_ffmpegdec_change_state (GstElement * element, GstStateChange transition)
{
  GstFFMpegDec *ffmpegdec = (GstFFMpegDec *) element;
  GstStateChangeReturn ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_OBJECT_LOCK (ffmpegdec);
      gst_ffmpegdec_close (ffmpegdec);
      if (ffmpegdec->last_buffer != NULL) {
        gst_buffer_unref (ffmpegdec->last_buffer);
        ffmpegdec->last_buffer = NULL;
      }
      GST_OBJECT_UNLOCK (ffmpegdec);
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_ffmpegdec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstFFMpegDec *ffmpegdec = (GstFFMpegDec *) object;

  switch (prop_id) {
    case ARG_LOWRES:
      ffmpegdec->lowres = ffmpegdec->context->lowres = g_value_get_enum (value);
      break;
    case ARG_SKIPFRAME:
      ffmpegdec->hurry_up = ffmpegdec->context->hurry_up =
          g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ffmpegdec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstFFMpegDec *ffmpegdec = (GstFFMpegDec *) object;

  switch (prop_id) {
    case ARG_LOWRES:
      g_value_set_enum (value, ffmpegdec->context->lowres);
      break;
    case ARG_SKIPFRAME:
      g_value_set_enum (value, ffmpegdec->context->hurry_up);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

gboolean
gst_ffmpegdec_register (GstPlugin * plugin)
{
  GTypeInfo typeinfo = {
    sizeof (GstFFMpegDecClass),
    (GBaseInitFunc) gst_ffmpegdec_base_init,
    NULL,
    (GClassInitFunc) gst_ffmpegdec_class_init,
    NULL,
    NULL,
    sizeof (GstFFMpegDec),
    0,
    (GInstanceInitFunc) gst_ffmpegdec_init,
  };
  GType type;
  AVCodec *in_plugin;
  gint rank;

  in_plugin = first_avcodec;

  GST_LOG ("Registering decoders");

  while (in_plugin) {
    GstFFMpegDecClassParams *params;
    GstCaps *srccaps = NULL, *sinkcaps = NULL;
    gchar *type_name;
    gchar *plugin_name;

    /* only decoders */
    if (!in_plugin->decode) {
      goto next;
    }

    /* no quasi-codecs, please */
    if (in_plugin->id == CODEC_ID_RAWVIDEO ||
        (in_plugin->id >= CODEC_ID_PCM_S16LE &&
            in_plugin->id <= CODEC_ID_PCM_S24DAUD)) {
      goto next;
    }

    /* no codecs for which we're GUARANTEED to have better alternatives */
    /* MPEG1VIDEO : the mpeg2video decoder is preferred */
    /* MP2 : Use MP3 for decoding */
    if (!strcmp (in_plugin->name, "gif") ||
        !strcmp (in_plugin->name, "vorbis") ||
        !strcmp (in_plugin->name, "mpeg1video") ||
        !strcmp (in_plugin->name, "mp2")) {
      GST_LOG ("Ignoring decoder %s", in_plugin->name);
      goto next;
    }

    /* name */
    if (!gst_ffmpeg_get_codecid_longname (in_plugin->id)) {
      GST_WARNING ("Add a longname mapping for decoder %s (%d) please",
          in_plugin->name, in_plugin->id);
      goto next;
    }

    /* first make sure we've got a supported type */
    sinkcaps = gst_ffmpeg_codecid_to_caps (in_plugin->id, NULL, FALSE);
    if (!sinkcaps) {
      GST_WARNING ("Couldn't get input caps for decoder '%s'", in_plugin->name);
    }
    if (in_plugin->type == CODEC_TYPE_VIDEO) {
      srccaps = gst_caps_from_string ("video/x-raw-rgb; video/x-raw-yuv");
    } else {
      srccaps =
          gst_ffmpeg_codectype_to_caps (in_plugin->type, NULL, in_plugin->id);
    }
    if (!sinkcaps || !srccaps) {
      GST_WARNING ("Couldn't get source or sink caps for decoder %s",
          in_plugin->name);
      goto next;
    }

    /* construct the type */
    plugin_name = g_strdup ((gchar *) in_plugin->name);
    g_strdelimit (plugin_name, NULL, '_');
    type_name = g_strdup_printf ("ffdec_%s", plugin_name);
    g_free (plugin_name);

    /* if it's already registered, drop it */
    if (g_type_from_name (type_name)) {
      g_free (type_name);
      goto next;
    }

    params = g_new0 (GstFFMpegDecClassParams, 1);
    params->in_plugin = in_plugin;
    params->srccaps = gst_caps_ref (srccaps);
    params->sinkcaps = gst_caps_ref (sinkcaps);

    /* create the gtype now */
    type = g_type_register_static (GST_TYPE_ELEMENT, type_name, &typeinfo, 0);
    g_type_set_qdata (type, GST_FFDEC_PARAMS_QDATA, (gpointer) params);

    /* (Ronald) MPEG-4 gets a higher priority because it has been well-
     * tested and by far outperforms divxdec/xviddec - so we prefer it.
     * msmpeg4v3 same, as it outperforms divxdec for divx3 playback.
     * VC1/WMV3 are not working and thus unpreferred for now. */
    switch (in_plugin->id) {
      case CODEC_ID_MPEG4:
      case CODEC_ID_MSMPEG4V3:
      case CODEC_ID_H264:
      case CODEC_ID_COOK:
        rank = GST_RANK_PRIMARY;
        break;
      case CODEC_ID_DVVIDEO:
        /* we have a good dv decoder, fast on both ppc as well as x86. they say
           libdv's quality is better though. leave as secondary.
           note: if you change this, see the code in gstdv.c in good/ext/dv. */
        rank = GST_RANK_SECONDARY;
        break;
        /* MP3 and MPEG2 have better alternatives and
           the ffmpeg versions don't work properly feel
           free to assign rank if you fix them */
      case CODEC_ID_MP3:
      case CODEC_ID_MPEG2VIDEO:
        rank = GST_RANK_NONE;
        break;
      default:
        rank = GST_RANK_MARGINAL;
        break;
    }
    if (!gst_element_register (plugin, type_name, rank, type)) {
      g_warning ("Failed to register %s", type_name);
      g_free (type_name);
      return FALSE;
    }

    g_free (type_name);

  next:
    if (sinkcaps)
      gst_caps_unref (sinkcaps);
    if (srccaps)
      gst_caps_unref (srccaps);
    in_plugin = in_plugin->next;
  }

  GST_LOG ("Finished Registering decoders");

  return TRUE;
}
