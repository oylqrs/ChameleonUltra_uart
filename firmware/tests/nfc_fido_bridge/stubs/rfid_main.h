#ifndef TEST_RFID_MAIN_H
#define TEST_RFID_MAIN_H

typedef enum {
    DEVICE_MODE_NONE,
    DEVICE_MODE_READER,
    DEVICE_MODE_TAG,
} device_mode_t;

device_mode_t get_device_mode(void);
void tag_mode_enter(void);
void reader_mode_enter(void);
void device_mode_exit(void);

#endif
