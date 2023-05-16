/**
 * A set of methods for using a serial device as external memory.
 *
 * Transmissions use the following format:
 * 8-byte header indicating the virtual address of the data
 * 8-byte header indicating the size of the data to be transmitted
 * A byte indicating the type of transmission (read or write)
 * Data
 */

#include "stdio_mem.h"
#include <stdio.h>

const char READ = 0;
const char WRITE = 1;

/**
 * Print some test data, including:
 * The size of a long long
 * Whether the device is little-endian or big-endian
 */
void print_test_data()
{
    printf("Size of long long: %d\n", sizeof(long long));
    long long test = 0x1234567890ABCDEF;
    char* test_ptr = (char*)&test;

    if (test_ptr[0] == 0xEF) {
        printf("Little-endian\n");
    } else {
        printf("Big-endian\n");
    }
}

/**
 * Send a string of bytes to the serial device
 */
void write_blocking(char* data, long long address, long long size)
{
    // write the 8-byte address header
    fwrite((char*)&address, 1, 8, stdout);

    // write the 8-byte size header
    fwrite((char*)&size, 1, 8, stdout);

    fwrite((char*)&WRITE, 1, 1, stdout);
    fflush(stdout);

    // write the data using fwrite, which is likely faster (TODO: verify)
    fwrite(data, 1, size, stdout);
    fflush(stdout);
}

/**
 * Read a string of bytes from the serial device
 */
void read_blocking(char* data, long long address, long long size)
{
    // write the 8-byte address header
    fwrite((char*)&address, 1, 8, stdout);

    // write the 8-byte size header
    fwrite((char*)&size, 1, 8, stdout);

    fwrite((char*)&READ, 1, 1, stdout);
    fflush(stdout);

    // read the data using fread, which is likely faster (TODO: verify)
    fread(data, 1, size, stdin);
    fflush(stdin);
}