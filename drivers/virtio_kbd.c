#include "virtio_kbd.h"
#include "irq.h"
#include "string.h"

/* QEMU virt VirtIO-MMIO peripheral range: 32 slots starting at 0x0a000000 */
#define VIRTIO_MMIO_BASE        0x0a000000UL
#define VIRTIO_MMIO_STRIDE      0x200UL
#define VIRTIO_MMIO_SLOTS       32U
#define VIRTIO_MMIO_MAGIC       0x74726976U /* "virt" */
#define VIRTIO_ID_INPUT         18U
#define MAX_INPUT_DEVS          4U

/* VirtIO-MMIO register offsets */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000U
#define VIRTIO_MMIO_VERSION             0x004U
#define VIRTIO_MMIO_DEVICE_ID           0x008U
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010U
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014U
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020U
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024U
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028U
#define VIRTIO_MMIO_QUEUE_SEL           0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034U
#define VIRTIO_MMIO_QUEUE_NUM           0x038U
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03cU
#define VIRTIO_MMIO_QUEUE_PFN           0x040U
#define VIRTIO_MMIO_QUEUE_READY         0x044U
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050U
#define VIRTIO_MMIO_INT_STATUS          0x060U
#define VIRTIO_MMIO_INT_ACK             0x064U
#define VIRTIO_MMIO_STATUS              0x070U
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084U
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090U
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094U
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0U
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4U

#define VIRTIO_STATUS_ACKNOWLEDGE       1U
#define VIRTIO_STATUS_DRIVER            2U
#define VIRTIO_STATUS_DRIVER_OK         4U
#define VIRTIO_STATUS_FEATURES_OK       8U

#define VRING_DESC_F_WRITE              2U
#define EV_KEY                          1U
#define EV_REL                          2U
#define EV_ABS                          3U
#define REL_X                           0U
#define REL_Y                           1U
#define ABS_X                           0U
#define ABS_Y                           1U
#define BTN_LEFT                        0x110U
#define BTN_RIGHT                       0x111U
#define BTN_TOUCH                       0x14AU
#define QSIZE                           16U
#define KBD_RING_BUF_SIZE               64U

/* Linux input key codes for modifiers and special keys */
#define KEY_ESC        1U
#define KEY_BACKSPACE  14U
#define KEY_TAB        15U
#define KEY_ENTER      28U
#define KEY_LEFTCTRL   29U
#define KEY_LEFTSHIFT  42U
#define KEY_RIGHTSHIFT 54U
#define KEY_SPACE      57U
#define KEY_CAPSLOCK   58U
#define KEY_F1         59U
#define KEY_F2         60U
#define KEY_F3         61U
#define KEY_F4         62U
#define KEY_F5         63U
#define KEY_KPENTER    96U
#define KEY_RIGHTCTRL  97U
#define KEY_HOME       102U
#define KEY_UP         103U
#define KEY_LEFT       105U
#define KEY_RIGHT      106U
#define KEY_END        107U
#define KEY_DOWN       108U
#define KEY_DELETE     111U

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QSIZE];
} __attribute__((packed));

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/*
 * Per-device 2-page (8192-byte, 4KB-aligned) virtqueue ring storage:
 *   offset 0:    desc[16]  (256 bytes)
 *   offset 256:  avail     (38 bytes)
 *   offset 4096: used      (4KB-aligned for legacy QueueAlign=4096 & modern v2)
 */
struct virtio_input_dev {
    uint8_t vq_mem[8192] __attribute__((aligned(4096)));
    volatile struct virtio_input_event events[QSIZE] __attribute__((aligned(64)));
    uintptr_t mmio_base;
    uint16_t last_used_idx;
    bool active;
};

static struct virtio_input_dev input_devs[MAX_INPUT_DEVS] __attribute__((aligned(4096)));
static uint32_t num_input_devs = 0;

static bool shift_held = false;
static bool ctrl_held = false;
static bool caps_lock = false;

static char key_fifo[KBD_RING_BUF_SIZE];
static uint32_t key_head = 0;
static uint32_t key_tail = 0;

static int32_t mouse_x = 400;
static int32_t mouse_y = 300;
static bool mouse_left_down = false;
static bool mouse_left_pressed = false;
static bool mouse_left_released = false;
static bool mouse_right_pressed = false;

static inline uint32_t mmio_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline void dmb_sy(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline struct virtq_desc *vq_desc(struct virtio_input_dev *d) {
    return (struct virtq_desc *)(d->vq_mem + 0U);
}

static inline struct virtq_avail *vq_avail(struct virtio_input_dev *d) {
    return (struct virtq_avail *)(d->vq_mem + 256U);
}

static inline struct virtq_used *vq_used(struct virtio_input_dev *d) {
    return (struct virtq_used *)(d->vq_mem + 4096U);
}

static const char keymap_normal[128] = {
    [KEY_ESC] = 0x1B,
    [2]  = '1', [3]  = '2', [4]  = '3', [5]  = '4', [6]  = '5',
    [7]  = '6', [8]  = '7', [9]  = '8', [10] = '9', [11] = '0',
    [12] = '-', [13] = '=', [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']', [KEY_ENTER] = '\n',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = ';',
    [40] = '\'', [41] = '`', [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [KEY_SPACE] = ' ',
    [KEY_KPENTER] = '\n',
};

static const char keymap_shift[128] = {
    [KEY_ESC] = 0x1B,
    [2]  = '!', [3]  = '@', [4]  = '#', [5]  = '$', [6]  = '%',
    [7]  = '^', [8]  = '&', [9]  = '*', [10] = '(', [11] = ')',
    [12] = '_', [13] = '+', [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '{', [27] = '}', [KEY_ENTER] = '\n',
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L', [39] = ':',
    [40] = '"', [41] = '~', [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = 'M', [51] = '<', [52] = '>', [53] = '?',
    [KEY_SPACE] = ' ',
    [KEY_KPENTER] = '\n',
};

static void fifo_push(char ch) {
    uint32_t next = (key_head + 1U) % KBD_RING_BUF_SIZE;
    if (next != key_tail) {
        key_fifo[key_head] = ch;
        key_head = next;
    }
}

static void fifo_push_str(const char *s) {
    while (*s) {
        fifo_push(*s++);
    }
}

static void process_key_event(uint16_t code, uint32_t value) {
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        shift_held = (value != 0U);
        return;
    }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        ctrl_held = (value != 0U);
        return;
    }
    if (code == KEY_CAPSLOCK) {
        if (value == 1U) {
            caps_lock = !caps_lock;
        }
        return;
    }

    if (value != 1U && value != 2U) {
        return;
    }

    if (code >= KEY_F1 && code <= KEY_F5) {
        fifo_push((char)(0x81 + (code - KEY_F1)));
        return;
    }

    switch (code) {
        case KEY_UP:     fifo_push_str("\033[A"); return;
        case KEY_DOWN:   fifo_push_str("\033[B"); return;
        case KEY_RIGHT:  fifo_push_str("\033[C"); return;
        case KEY_LEFT:   fifo_push_str("\033[D"); return;
        case KEY_HOME:   fifo_push_str("\033[H"); return;
        case KEY_END:    fifo_push_str("\033[F"); return;
        case KEY_DELETE: fifo_push_str("\033[3~"); return;
        default: break;
    }

    if (code >= 128U) {
        return;
    }

    char base_ch = keymap_normal[code];
    if (base_ch == 0) {
        return;
    }

    if (ctrl_held && base_ch >= 'a' && base_ch <= 'z') {
        fifo_push((char)(base_ch - 'a' + 1));
        return;
    }

    bool is_alpha = (base_ch >= 'a' && base_ch <= 'z');
    bool use_shift = is_alpha ? (shift_held ^ caps_lock) : shift_held;
    char ch = use_shift ? keymap_shift[code] : base_ch;
    if (ch != 0) {
        fifo_push(ch);
    }
}

static bool virtio_input_init_slot(uintptr_t found, uint32_t version, struct virtio_input_dev *d) {
    /* Reset device */
    mmio_write32(found, VIRTIO_MMIO_STATUS, 0U);
    dmb_sy();

    /* Acknowledge and register driver */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    mmio_write32(found, VIRTIO_MMIO_STATUS, status);

    if (version == 2U) {
        mmio_write32(found, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0U);
        (void)mmio_read32(found, VIRTIO_MMIO_DEVICE_FEATURES);
        mmio_write32(found, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1U);
        uint32_t feat_hi = mmio_read32(found, VIRTIO_MMIO_DEVICE_FEATURES);

        mmio_write32(found, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0U);
        mmio_write32(found, VIRTIO_MMIO_DRIVER_FEATURES, 0U);
        mmio_write32(found, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1U);
        mmio_write32(found, VIRTIO_MMIO_DRIVER_FEATURES, feat_hi & 1U); /* VIRTIO_F_VERSION_1 */

        status |= VIRTIO_STATUS_FEATURES_OK;
        mmio_write32(found, VIRTIO_MMIO_STATUS, status);
        if ((mmio_read32(found, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0U) {
            mmio_write32(found, VIRTIO_MMIO_STATUS, 0U);
            return false;
        }
    } else {
        mmio_write32(found, VIRTIO_MMIO_DRIVER_FEATURES, 0U);
        mmio_write32(found, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096U);
    }

    /* Configure Virtqueue 0 (eventq) */
    mmio_write32(found, VIRTIO_MMIO_QUEUE_SEL, 0U);
    uint32_t qmax = mmio_read32(found, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < QSIZE) {
        mmio_write32(found, VIRTIO_MMIO_STATUS, 0U);
        return false;
    }

    memset(d->vq_mem, 0, sizeof(d->vq_mem));
    memset((void *)d->events, 0, sizeof(d->events));
    d->last_used_idx = 0;

    mmio_write32(found, VIRTIO_MMIO_QUEUE_NUM, QSIZE);

    if (version == 2U) {
        uint64_t desc_addr = (uint64_t)vq_desc(d);
        uint64_t avail_addr = (uint64_t)vq_avail(d);
        uint64_t used_addr = (uint64_t)vq_used(d);

        mmio_write32(found, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_addr);
        mmio_write32(found, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_addr >> 32));
        mmio_write32(found, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_addr);
        mmio_write32(found, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_addr >> 32));
        mmio_write32(found, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_addr);
        mmio_write32(found, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_addr >> 32));
        mmio_write32(found, VIRTIO_MMIO_QUEUE_READY, 1U);
    } else {
        mmio_write32(found, VIRTIO_MMIO_QUEUE_ALIGN, 4096U);
        mmio_write32(found, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)((uintptr_t)d->vq_mem >> 12));
    }

    struct virtq_desc *desc = vq_desc(d);
    struct virtq_avail *avail = vq_avail(d);
    for (uint16_t i = 0; i < QSIZE; i++) {
        desc[i].addr = (uint64_t)&d->events[i];
        desc[i].len = sizeof(struct virtio_input_event);
        desc[i].flags = VRING_DESC_F_WRITE;
        desc[i].next = 0;
        avail->ring[i] = i;
    }
    dmb_sy();
    avail->idx = QSIZE;
    dmb_sy();

    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(found, VIRTIO_MMIO_STATUS, status);
    mmio_write32(found, VIRTIO_MMIO_QUEUE_NOTIFY, 0U);

    d->mmio_base = found;
    d->active = true;
    return true;
}

void virtio_kbd_init(void) {
    num_input_devs = 0;
    for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS && num_input_devs < MAX_INPUT_DEVS; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(base, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) {
            continue;
        }
        uint32_t dev_id = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);
        uint32_t ver = mmio_read32(base, VIRTIO_MMIO_VERSION);
        if (dev_id == VIRTIO_ID_INPUT && (ver == 1U || ver == 2U)) {
            if (virtio_input_init_slot(base, ver, &input_devs[num_input_devs])) {
                num_input_devs++;
            }
        }
    }
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void process_input_event(uint16_t type, uint16_t code, uint32_t value) {
    if (type == EV_ABS) {
        if (code == ABS_X) {
            int32_t nx = (int32_t)(((int64_t)value * 799LL) / 32767LL);
            mouse_x = clamp_i32(nx, 0, 799);
        } else if (code == ABS_Y) {
            int32_t ny = (int32_t)(((int64_t)value * 599LL) / 32767LL);
            mouse_y = clamp_i32(ny, 0, 599);
        }
        return;
    }
    if (type == EV_REL) {
        int32_t delta = (int32_t)value;
        if (code == REL_X) {
            mouse_x = clamp_i32(mouse_x + delta, 0, 799);
        } else if (code == REL_Y) {
            mouse_y = clamp_i32(mouse_y + delta, 0, 599);
        }
        return;
    }
    if (type == EV_KEY) {
        if (code == BTN_LEFT || code == BTN_TOUCH) {
            if (value != 0U) {
                if (!mouse_left_down) mouse_left_pressed = true;
                mouse_left_down = true;
            } else {
                if (mouse_left_down) mouse_left_released = true;
                mouse_left_down = false;
            }
            return;
        }
        if (code == BTN_RIGHT) {
            if (value != 0U) mouse_right_pressed = true;
            return;
        }
        process_key_event(code, value);
    }
}

static void virtio_input_poll(void) {
    if (num_input_devs == 0U) return;

    uint64_t flags = irq_save();
    for (uint32_t di = 0; di < num_input_devs; di++) {
        struct virtio_input_dev *d = &input_devs[di];
        if (!d->active) continue;

        volatile struct virtq_used *used = vq_used(d);
        struct virtq_avail *avail = vq_avail(d);
        bool recycled = false;

        dmb_sy();
        while (d->last_used_idx != used->idx) {
            dmb_sy();
            uint16_t u_slot = (uint16_t)(d->last_used_idx % QSIZE);
            uint32_t desc_id = used->ring[u_slot].id;
            d->last_used_idx++;

            if (desc_id < QSIZE) {
                process_input_event(d->events[desc_id].type,
                                    d->events[desc_id].code,
                                    d->events[desc_id].value);

                uint16_t a_slot = (uint16_t)(avail->idx % QSIZE);
                avail->ring[a_slot] = (uint16_t)desc_id;
                dmb_sy();
                avail->idx++;
                recycled = true;
            }
        }

        if (recycled) {
            dmb_sy();
            uint32_t isr = mmio_read32(d->mmio_base, VIRTIO_MMIO_INT_STATUS);
            if (isr != 0U) {
                mmio_write32(d->mmio_base, VIRTIO_MMIO_INT_ACK, isr);
            }
            mmio_write32(d->mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0U);
        }
    }
    irq_restore(flags);
}

int virtio_kbd_getc_nonblock(void) {
    virtio_input_poll();
    uint64_t flags = irq_save();
    int ch = -1;
    if (key_head != key_tail) {
        ch = (int)(uint8_t)key_fifo[key_tail];
        key_tail = (key_tail + 1U) % KBD_RING_BUF_SIZE;
    }
    irq_restore(flags);
    return ch;
}

void virtio_mouse_get_state(struct mouse_state *out) {
    if (!out) return;
    virtio_input_poll();
    uint64_t flags = irq_save();
    out->x = mouse_x;
    out->y = mouse_y;
    out->left_down = mouse_left_down;
    out->left_pressed = mouse_left_pressed;
    out->left_released = mouse_left_released;
    out->right_pressed = mouse_right_pressed;
    mouse_left_pressed = false;
    mouse_left_released = false;
    mouse_right_pressed = false;
    irq_restore(flags);
}

void virtio_mouse_set_pos(int32_t x, int32_t y) {
    uint64_t flags = irq_save();
    mouse_x = clamp_i32(x, 0, 799);
    mouse_y = clamp_i32(y, 0, 599);
    irq_restore(flags);
}
