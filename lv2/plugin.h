#ifndef PB_PLUGIN_H
#define PB_PLUGIN_H

#include <lv2/core/lv2.h>
#include <stdio.h>

#define PB_URI_BASE          "http://pedal-bridge.local/plugins/"
#define PB_URI_SEND          PB_URI_BASE "send"
#define PB_URI_RETURN        PB_URI_BASE "return"
#define PB_URI_LOOPER_MONO   PB_URI_BASE "looper-mono"
#define PB_URI_LOOPER_STEREO PB_URI_BASE "looper-stereo"
#define PB_URI_DRUM_MONO     PB_URI_BASE "drum-mono"
#define PB_URI_DRUM_STEREO   PB_URI_BASE "drum-stereo"
#define PB_URI_DRUM_SONG     PB_URI_BASE "drum#song"      /* propriété patch : morceau */
#define PB_URI_DRUM_KIT      PB_URI_BASE "drum#kit"       /* propriété patch : kit */

/* Messages d'erreur : seulement hors temps réel (instantiate). */
#define pb_log_error(...) fprintf(stderr, __VA_ARGS__)

extern const LV2_Descriptor pb_send_descriptor;
extern const LV2_Descriptor pb_return_descriptor;
extern const LV2_Descriptor pb_looper_mono_descriptor;
extern const LV2_Descriptor pb_looper_stereo_descriptor;
extern const LV2_Descriptor pb_drum_mono_descriptor;
extern const LV2_Descriptor pb_drum_stereo_descriptor;

#endif
