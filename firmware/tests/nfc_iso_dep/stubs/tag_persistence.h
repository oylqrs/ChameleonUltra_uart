#ifndef TEST_TAG_PERSISTENCE_H
#define TEST_TAG_PERSISTENCE_H
#include "tag_emulation.h"
typedef struct { uint16_t key; uint16_t id; } fds_slot_record_map_t;
void get_fds_map_by_slot_sense_type_for_dump(uint8_t, tag_sense_type_t,
                                            fds_slot_record_map_t *);
#endif
