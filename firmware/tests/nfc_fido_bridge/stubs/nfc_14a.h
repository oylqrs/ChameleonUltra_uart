#ifndef TEST_NFC_14A_H
#define TEST_NFC_14A_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NFC_TAG_14A_UID_SINGLE_SIZE = 4u,
    NFC_TAG_14A_UID_DOUBLE_SIZE = 7u,
    NFC_TAG_14A_UID_TRIPLE_SIZE = 10u,
} nfc_tag_14a_uid_size;

typedef struct {
    uint8_t data[0xff];
    uint8_t length;
} nfc_14a_ats_t;

typedef struct {
    nfc_tag_14a_uid_size size;
    uint8_t atqa[2];
    uint8_t sak[1];
    uint8_t uid[10];
    nfc_14a_ats_t ats;
} nfc_tag_14a_coll_res_entity_t;

void nfc_tag_14a_sense_switch(bool enable);

#endif
