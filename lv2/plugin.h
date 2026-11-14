#ifndef PB_PLUGIN_H
#define PB_PLUGIN_H

#include <lv2/core/lv2.h>
#include <stdio.h>

#define PB_URI_BASE   "http://pedal-bridge.local/plugins/"
#define PB_URI_SEND   PB_URI_BASE "send"
#define PB_URI_RETURN PB_URI_BASE "return"

/* Messages d'erreur : seulement hors temps réel (instantiate). */
#define pb_log_error(...) fprintf(stderr, __VA_ARGS__)

extern const LV2_Descriptor pb_send_descriptor;
extern const LV2_Descriptor pb_return_descriptor;

#endif
