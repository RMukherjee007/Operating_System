#ifndef FULMEN_VIRTIO_KBD_H
#define FULMEN_VIRTIO_KBD_H

#include "types.h"

struct mouse_state {
    int32_t x;
    int32_t y;
    bool left_down;
    bool left_pressed;
    bool left_released;
    bool right_pressed;
};

void virtio_kbd_init(void);
uint32_t virtio_kbd_device_count(void);
int virtio_kbd_getc_nonblock(void);
void virtio_mouse_get_state(struct mouse_state *out);
void virtio_mouse_set_pos(int32_t x, int32_t y);

#endif /* FULMEN_VIRTIO_KBD_H */
