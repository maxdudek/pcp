#define DMI_BUFFER_SIZE 64
#define DMI_PATH "/sys/class/dmi/id/"
#define DMI_NUM_METRICS (sizeof(SYSFS_DMI_METRICS)/sizeof(char *))

typedef struct {
    char board_vendor[DMI_BUFFER_SIZE];
    char board_name[DMI_BUFFER_SIZE];
    char board_version[DMI_BUFFER_SIZE];
    char product_family[DMI_BUFFER_SIZE];
    char product_name[DMI_BUFFER_SIZE];
    char product_version[DMI_BUFFER_SIZE];
    char sys_vendor[DMI_BUFFER_SIZE];
} sysfs_dmi_t;

const char * const SYSFS_DMI_METRICS[] = {
    "board_vendor",
    "board_name",
    "board_version",
    "product_family",
    "product_name",
    "product_version",
    "sys_vendor",
    "blarg",
};

extern int refresh_sysfs_dmi(sysfs_dmi_t *);