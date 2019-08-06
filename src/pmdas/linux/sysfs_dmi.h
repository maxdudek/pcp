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
} sysfs_dmi_t;

extern int refresh_sysfs_dmi(sysfs_dmi_t *);

#endif
