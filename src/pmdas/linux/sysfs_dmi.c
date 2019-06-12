//#include "linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysfs_dmi.h"

void stripNewline(char *s) {
    for (int i = 0; s[i] != '\0' && i < DMI_BUFFER_SIZE; i++) {
        if (s[i] == '\n') s[i] = '\0';
    }
}

/**
 * Reads from file and puts value from file into metricBuffer
 * Return values:
 *  0: success
 *  1: file not found
 *  2: file exists, but does not contain anything
 */
int getMetricFromFile(char * metricBuffer, const char * filename) {
    int length;
    FILE * f = fopen (filename, "rb");

    if (f) {
        fread (metricBuffer, 1, DMI_BUFFER_SIZE, f);
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

void printData(sysfs_dmi_t *sysfs_dmi) {
    printf("board_vendor: %s\n", sysfs_dmi->board_vendor);
    printf("board_name: %s\n", sysfs_dmi->board_name);
    printf("board_version: %s\n", sysfs_dmi->board_version);
    printf("product_family: %s\n", sysfs_dmi->product_family);
    printf("product_name: %s\n", sysfs_dmi->product_name);
    printf("product_version: %s\n", sysfs_dmi->product_version);
    printf("sys_vendor: %s\n", sysfs_dmi->sys_vendor);
}

int refresh_sysfs_dmi(sysfs_dmi_t *sysfs_dmi) {
    char buffer[DMI_NUM_METRICS][DMI_BUFFER_SIZE];
    memset(buffer, '\0', sizeof(buffer));

    for (int i = 0; i < DMI_NUM_METRICS; i++) {
        const char *metricName = SYSFS_DMI_METRICS[i];
        
        char dmiFilename[DMI_BUFFER_SIZE];
        sprintf(dmiFilename, "%s%s", DMI_PATH, metricName);

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

int main() {
    sysfs_dmi_t sysfs_dmi;
    refresh_sysfs_dmi(&sysfs_dmi);
    printData(&sysfs_dmi);

    return 0;
}