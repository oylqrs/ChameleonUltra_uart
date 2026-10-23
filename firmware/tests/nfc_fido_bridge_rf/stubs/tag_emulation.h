#ifndef TEST_BRIDGE_RF_TAG_EMULATION_H
#define TEST_BRIDGE_RF_TAG_EMULATION_H

#include "../../nfc_iso_dep/stubs/tag_emulation.h"

void tag_emulation_sense_run(void);
void tag_emulation_sense_end(void);
void tag_emulation_load_data(void);

#endif
