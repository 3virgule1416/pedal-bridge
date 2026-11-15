/* Point d'entrée du bundle pedal-bridge.lv2. */
#include "plugin.h"

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    switch (index) {
    case 0:  return &pb_send_descriptor;
    case 1:  return &pb_return_descriptor;
    case 2:  return &pb_looper_mono_descriptor;
    case 3:  return &pb_looper_stereo_descriptor;
    case 4:  return &pb_drum_mono_descriptor;
    case 5:  return &pb_drum_stereo_descriptor;
    default: return NULL;
    }
}
