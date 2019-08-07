#ifndef _DMI_
#define _DMI_

#define DMI_BUFFER_SIZE 64

typedef struct {
    char board_vendor[DMI_BUFFER_SIZE];
    char board_name[DMI_BUFFER_SIZE];
    char board_version[DMI_BUFFER_SIZE];
    char product_family[DMI_BUFFER_SIZE];
    char product_name[DMI_BUFFER_SIZE];
    char product_version[DMI_BUFFER_SIZE];
    char sys_vendor[DMI_BUFFER_SIZE];

    // Error flags - 1 if metric can't be read
    char board_vendor_error;
    char board_name_error;
    char board_version_error;
    char product_family_error;
    char product_name_error;
    char product_version_error;
    char sys_vendor_error;
} sysfs_dmi_t;

extern int refresh_sysfs_dmi(sysfs_dmi_t *);

#endif
