#include "types.h"

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl;
    void *restore_tpl;
    void *allocate_pages;
    void *free_pages;
    uint64_t (*get_memory_map)(uint64_t *memory_map_size,
                               void *memory_map,
                               uint64_t *map_key,
                               uint64_t *descriptor_size,
                               uint32_t *descriptor_version);
    void *allocate_pool;
    void *free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    void *handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    uint64_t (*exit_boot_services)(void *image_handle, uint64_t map_key);
};

struct efi_system_table {
    struct efi_table_header hdr;
    void *firmware_vendor;
    uint32_t firmware_revision;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    void *con_out;
    void *standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
};

/*
 * Called by _efi_entry in boot/boot.S before relocating to 0x40200000.
 * Keep completely self-contained (no global references) so it runs at any load address.
 */
void efi_exit_boot_services(void *image_handle, struct efi_system_table *sys_table) {
    if (!sys_table || !sys_table->boot_services) return;
    struct efi_boot_services *bs = sys_table->boot_services;
    if (!bs->get_memory_map || !bs->exit_boot_services) return;

    uint8_t mmap_buf[16384];
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t mmap_size = sizeof(mmap_buf);
        uint64_t map_key = 0;
        uint64_t desc_size = 0;
        uint32_t desc_ver = 0;

        uint64_t status = bs->get_memory_map(&mmap_size, mmap_buf, &map_key, &desc_size, &desc_ver);
        if (status != 0) continue;

        status = bs->exit_boot_services(image_handle, map_key);
        if (status == 0) return;
    }
}
