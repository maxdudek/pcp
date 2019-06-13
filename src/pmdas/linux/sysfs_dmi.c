#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysfs_dmi.h"

#define DMI_PATH "/sys/class/dmi/id/"
#define DMI_NUM_METRICS (sizeof(SYSFS_DMI_METRICS)/sizeof(char *))

const char * const SYSFS_DMI_METRICS[] = {
    "board_vendor",
    "board_name",
    "board_version",
    "product_family",
    "product_name",
    "product_version",
    "sys_vendor",
};

void stripNewline(char * s) {
    int i;
    for (i = 0; s[i] != '\0' && i < DMI_BUFFER_SIZE; i++) {
        if (s[i] == '\n') s[i] = '\0';
    }
}

/**
 * Reads from file and puts value from file into metricBuffer
 * Return values:
 *  0: success
 *  1: file not found
 *  2: file exists, but does not contain anything
 *  3: data could not be read from file (fread returned 0)
 */
int getMetricFromFile(char * metricBuffer, const char * filename) {
    FILE * f = fopen (filename, "rb");

    if (f) {
        if (fread (metricBuffer, 1, DMI_BUFFER_SIZE, f) <= 0) {
            return 3; // Read error
        }
        fclose (f);
    } else {
        return 1; // File not found
    }
    stripNewline(metricBuffer);
    if (metricBuffer[0] == '\0') {
        return 2; // File doesn't contain a value
    }
    return 0; // Success
}

int refresh_sysfs_dmi(sysfs_dmi_t * sysfs_dmi) {

    // If the testing environment variable is set, change the path to the test path
    char * filePath = DMI_PATH;
    char * env = getenv("DMI_PATH_TEST");
    if (env != NULL) filePath = env;

    char buffer[DMI_NUM_METRICS][DMI_BUFFER_SIZE];
    memset(buffer, '\0', sizeof(buffer));

    int i;
    for (i = 0; i < DMI_NUM_METRICS; i++) {
        const char * metricName = SYSFS_DMI_METRICS[i];
        
        char dmiFilename[DMI_BUFFER_SIZE];
        sprintf(dmiFilename, "%s%s", filePath, metricName);
        int result = getMetricFromFile(buffer[i], dmiFilename);

        // If the file read failed, get an empty string
        if (result != 0) buffer[i][0] = '\0';
    }

    // Move data from buffer to struct
    strncpy(sysfs_dmi->board_vendor, buffer[0], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->board_name, buffer[1], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->board_version, buffer[2], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->product_family, buffer[3], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->product_name, buffer[4], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->product_version, buffer[5], DMI_BUFFER_SIZE);
    strncpy(sysfs_dmi->sys_vendor, buffer[6], DMI_BUFFER_SIZE);

    return 0;
}
