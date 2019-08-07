#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysfs_dmi.h"

#define DMI_PATH "/sys/class/dmi/id"

void stripNewline(char * s) {
    int i;
    for (i = 0; s[i] != '\0' && i < DMI_BUFFER_SIZE; i++) {
        if (s[i] == '\n') s[i] = '\0';
    }
}

/**
 * Reads from file and puts value from file into metricBuffer
 * Returns 0 on success, 1 on error
 */
char getMetricFromFile(char * metricBuffer, const char * filename) {
    FILE * f = fopen (filename, "rb");

    if (f) {
        if (fread (metricBuffer, 1, DMI_BUFFER_SIZE, f) <= 0) {
            fclose(f);
            return 1; // Read error
        }
        fclose (f);
    } else {
        return 1; // File not found
    }
    stripNewline(metricBuffer);
    return 0; // Success
}

int refresh_sysfs_dmi(sysfs_dmi_t * sysfs_dmi) {

    // If the testing environment variable is set, change the path to the test path
    char * filePath = DMI_PATH;
    char * env = getenv("DMI_PATH_TEST");
    if (env != NULL) filePath = env;

    char dmiFilename[DMI_BUFFER_SIZE];

    // Move data from files to sysfs_dmi struct
    sprintf(dmiFilename, "%s/%s", filePath, "board_vendor");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->board_vendor, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "board_name");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->board_name, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "board_version");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->board_version, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "product_family");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->product_family, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "product_name");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->product_name, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "product_version");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->product_version, dmiFilename);

    sprintf(dmiFilename, "%s/%s", filePath, "sys_vendor");
    sysfs_dmi->board_vendor_error = getMetricFromFile(sysfs_dmi->sys_vendor, dmiFilename);

    return 0;
}
