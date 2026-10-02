#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fobos_sdr.h>

#include "fobos.h"

#define MAX_SERIALS_LEN 256
#define MAX_INFO_LEN 32
#define DEFAULT_BUFFER_COUNT 16
#define DEFAULT_DIRECT_SAMPLING 0
#define DEFAULT_CLK_SOURCE 0
#define DEFAUKT_LNA_GAIN 0
#define DEFAULT_VGA_GAIN 0
#define DEFAULT_FOBOS_SAMPLES_READ 65536

#define FOBOS_SAMPLE_SIZE sizeof(float) * 2 // I/Q samples

//#define DEBUG_FOBOS

#ifdef DEBUG_FOBOS
#define DEBUG_PRINTF(...)  fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINTF(...) 
#endif


int fobos_init(void){
    int ret = 0;
    char lib_version[MAX_INFO_LEN];
    char drv_version[MAX_INFO_LEN];
    
    if((ret = fobos_sdr_get_api_info(lib_version, drv_version)) == FOBOS_ERR_OK){
        printf("API Info lib: %s drv: %s\n", lib_version, drv_version);
    } else {
        fprintf(stderr, "Error getting API info: %d\n", ret);
    }

    return ret;
}

int fobos_get_device_count(void){
    char serials[MAX_SERIALS_LEN] = {0};

    int count = fobos_sdr_list_devices(serials);

    printf("found devices: %d\n", count);
    return count;
}

int fobos_get_serials(char* serials, int max_len){
    if(serials == NULL || max_len <= MAX_SERIALS_LEN){
        return -1; // Invalid parameters
    }

    return fobos_sdr_list_devices(serials);
}

fobos_device_t *fobos_open_by_idx(int index){
    struct fobos_sdr_dev_t * dev = NULL;
    char hw_revision[MAX_INFO_LEN];
    char fw_version[MAX_INFO_LEN];
    char manufacturer[MAX_INFO_LEN];
    char product[MAX_INFO_LEN];
    char serial[MAX_INFO_LEN];
    int result = 0;
    fobos_device_t *device = NULL;

    do{
        printf("fobos to open device at index %d\n", index);
        if ((result = fobos_sdr_open(&dev, index)) != FOBOS_ERR_OK) {
            fprintf(stderr, "Error opening device at index %d: %d\n", index, result);
            break;
        }

        printf("fobos open device open result %d\n", result);
        
        result = fobos_sdr_get_board_info(dev, hw_revision, fw_version, manufacturer, product, serial);
        if (result != 0)
        {
            printf("fobos_sdr_get_board_info - error!\n");
        }
        else
        {
            printf("board info\n");
            printf("    hw_revision:  %s\n", hw_revision);
            printf("    fw_version:   %s\n", fw_version);
            printf("    manufacturer: %s\n", manufacturer);
            printf("    product:      %s\n", product);
            printf("    serial:       %s\n", serial);
        }

        if((result = fobos_sdr_set_direct_sampling(dev, DEFAULT_DIRECT_SAMPLING)) != 0){
            printf("fobos_rx_set_direct_sampling - error!\n");
        }
        
        if ((result = fobos_sdr_set_clk_source(dev, DEFAULT_CLK_SOURCE)) != 0){
            printf("fobos_rx_set_clk_source - error!\n");
        }

        if ((result = fobos_sdr_set_lna_gain(dev, DEFAUKT_LNA_GAIN)) != 0){
            printf("fobos_rx_set_lna_gain - error!\n");
        }   
        
        if ((result = fobos_sdr_set_vga_gain(dev, DEFAULT_VGA_GAIN)) != 0)    {
            printf("fobos_rx_set_vga_gain - error!\n");
        }  
    
    }while (0);

    if (result != FOBOS_ERR_OK ) {
        if(dev != NULL){
            fobos_sdr_close(dev);
            dev = NULL;
        }        
    } else {
        printf("fobos device opened successfully at index %d\n", index);
        device = (fobos_device_t *)malloc(sizeof(fobos_device_t));
        if (device == NULL) {
            fprintf(stderr, "Failed to allocate memory for fobos_device_t\n");
            fobos_sdr_close(dev);
            dev = NULL;
        } else {
            device->dev = dev;
            device->buffer_size = 0;    
            device->buffer = NULL;
            device->samples_in_buffer = 0;
            device->buffer_ptr = NULL;
        }
    }

    return device;
}

fobos_device_t *fobos_open_by_serial(const char* serial){
    char serials[MAX_SERIALS_LEN] = {0};
    int count = fobos_sdr_list_devices(serials);
    char* pserial = strtok(serials, " ");
    int index = 0;

    for (index = 0; index < count; index++) {
        if (strcmp(pserial, serial) == 0) {
            return fobos_open_by_idx(index);
        }
        pserial = strtok(0, " ");
    }

    fprintf(stderr, "Device with serial %s not found\n", serial);
    return NULL;
}

int fobos_close(fobos_device_t *dev){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }
    
    int result = fobos_sdr_close(dev->dev);
    free(dev);
    dev = NULL;
    return result;
}

int fobos_get_board_info(fobos_device_t *dev, char* hw_revision, char* fw_version, char* manufacturer, char* product, char* serial){
    if (dev == NULL || hw_revision == NULL || fw_version == NULL || manufacturer == NULL || product == NULL || serial == NULL) {
        return -1; // Invalid parameters
    }

    return fobos_sdr_get_board_info(dev->dev, hw_revision, fw_version, manufacturer, product, serial);
}

int fobos_set_samplerate(fobos_device_t *dev, int sample_rate){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    return fobos_sdr_set_samplerate(dev->dev, sample_rate);
}

int fobos_set_bandwidth(fobos_device_t *dev, float bw){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    } 
    
    return fobos_sdr_set_bandwidth(dev->dev, bw);
}

int fobos_set_frequency(fobos_device_t *dev, freq_t frequency){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    return fobos_sdr_set_frequency(dev->dev, (double)frequency);
}

int fobos_set_lna_gain(fobos_device_t *dev, int gain){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    return fobos_sdr_set_lna_gain(dev->dev, gain);    
}

int fobos_set_vga_gain(fobos_device_t *dev, int gain){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    return fobos_sdr_set_vga_gain(dev->dev, gain);    
}

int fobos_start_sync(fobos_device_t *dev, int buf_length){
    if (dev == NULL) {
        return -1; // Invalid parameters
    }

    if(dev->buffer){
        free(dev->buffer);
        dev->buffer = NULL;
    }

    dev->buffer_size = DEFAULT_FOBOS_SAMPLES_READ * FOBOS_SAMPLE_SIZE; // I/Q samples
    dev->buffer = (uint8_t *)malloc(dev->buffer_size);
    if (dev->buffer == NULL) {
        fprintf(stderr, "Failed to allocate buffer for synchronous reading\n");
        return -1;
    }   

    dev->samples_in_buffer = 0;
    dev->buffer_ptr = dev->buffer;

    return fobos_sdr_start_sync(dev->dev, buf_length);   
}

int fobos_stop_sync(fobos_device_t *dev){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    if(dev->buffer){
        free(dev->buffer);
        dev->buffer = NULL;
    }

    return fobos_sdr_stop_sync(dev->dev);
}

int fobos_stop_async(fobos_device_t *dev){
    if (dev == NULL) {
        return -1; // Invalid device pointer
    }

    return fobos_sdr_cancel_async(dev->dev);
}

int fobos_read_samples_sync(fobos_device_t *dev, float* buffer, size_t num_samples){
    if (dev == NULL || buffer == NULL || num_samples == 0) {
        return -1; // Invalid parameters
    }

    DEBUG_PRINTF("Starting synchronous read of %zu samples...\n", num_samples);

    int samples_read = 0;

    DEBUG_PRINTF("dev buffer pointer %p(%p), buffer size %zu, samples in buffer %zu\n", dev->buffer_ptr, dev->buffer, dev->buffer_size, dev->samples_in_buffer);

    while (samples_read < num_samples) {
        uint32_t samples_to_read = (uint32_t)(num_samples - samples_read);
        DEBUG_PRINTF("Reading samples: %d\n", samples_to_read);
        if(samples_to_read > dev->samples_in_buffer){
            //try to read from buffer first
            DEBUG_PRINTF("Rest samples in buffer: %zu\n", dev->samples_in_buffer);
            if(dev->samples_in_buffer){
                DEBUG_PRINTF("copy from buffer (%p): %zu\n", dev->buffer_ptr, dev->samples_in_buffer);
                memcpy(buffer + samples_read, dev->buffer_ptr, dev->samples_in_buffer * FOBOS_SAMPLE_SIZE);
                samples_to_read -= dev->samples_in_buffer;
                dev->samples_in_buffer = 0;                
            }

            uint32_t samples_read_from_device = DEFAULT_FOBOS_SAMPLES_READ;
            DEBUG_PRINTF("Reading samples from device: %d\n", samples_read_from_device);
            int result = fobos_sdr_read_sync(dev->dev, (float *)dev->buffer, &samples_read_from_device);

            if (result != FOBOS_ERR_OK) {
                fprintf(stderr, "Error reading samples: %d\n", result);
                return result;
            }

            DEBUG_PRINTF("Buffer pointer: %p, buffer %p\n", dev->buffer_ptr, dev->buffer);
            dev->buffer_ptr = dev->buffer;

            DEBUG_PRINTF("Read %u samples from device\n", samples_read_from_device);

            if( samples_read_from_device >= samples_to_read ){
                DEBUG_PRINTF("copy from buffer (%p): %u\n", dev->buffer_ptr, samples_to_read);
                memcpy(buffer + samples_read, dev->buffer_ptr, samples_to_read * FOBOS_SAMPLE_SIZE);
                dev->samples_in_buffer = samples_read_from_device - samples_to_read;
                dev->buffer_ptr += samples_to_read * FOBOS_SAMPLE_SIZE;
            } else {
                DEBUG_PRINTF("No more samples available\n");
                break; // No more samples available
            }
        } else {
            DEBUG_PRINTF("Read samples from buffer (%p): %u\n", dev->buffer_ptr, samples_to_read);
            memcpy(buffer + samples_read, dev->buffer_ptr, samples_to_read * FOBOS_SAMPLE_SIZE);
            dev->buffer_ptr += samples_to_read * FOBOS_SAMPLE_SIZE;
            dev->samples_in_buffer -= samples_to_read;
        }
          
        samples_read += samples_to_read;
    }

    return samples_read; 
}

int fobos_read_samples_sync_direct(fobos_device_t *dev, float* buffer, size_t num_samples){
    int ret;
    uint32_t samples_read_from_device = num_samples;
    DEBUG_PRINTF("Reading samples from device: %d\n", samples_read_from_device);
    if((ret = fobos_sdr_read_sync(dev->dev, buffer, &samples_read_from_device)) != 0){
        return ret;
    }
    return samples_read_from_device;
}

static void fobos_async_callback(float *buf, unsigned int buf_length,  struct fobos_sdr_dev_t * sender, void *ctx){
    fobos_device_t *dev = (fobos_device_t *)ctx;
    DEBUG_PRINTF("Async callback received %u samples\n", buf_length);
    // Process the received samples in 'buf' of length 'buf_length'
    // 'sender' is the device that triggered the callback
    // 'ctx' is the user data passed when setting up the callback
    if (dev->async_callback) {
        dev->async_callback(buf, buf_length, sender, dev->async_user_data);
    }
}

int fobos_read_samples_async(fobos_device_t *dev, size_t num_samples, fobos_data_callback_t callback, void* user_data){
    if (dev == NULL || num_samples <= 0 || callback == NULL) {
        return -1; // Invalid parameters
    }

    dev->async_callback = callback;
    dev->async_user_data = user_data;

    return fobos_sdr_read_async(dev->dev,  fobos_async_callback, dev, DEFAULT_BUFFER_COUNT, num_samples);
}