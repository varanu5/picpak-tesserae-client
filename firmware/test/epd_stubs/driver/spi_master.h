#pragma once
#include <stddef.h>
typedef void *spi_device_handle_t;
typedef struct { int mosi_io_num,miso_io_num,sclk_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz,mode,spics_io_num,queue_size; } spi_device_interface_config_t;
typedef struct { int length; const void *tx_buffer; } spi_transaction_t;
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 1
int spi_bus_initialize(int host,const spi_bus_config_t *c,int dma);
int spi_bus_add_device(int host,const spi_device_interface_config_t *c,spi_device_handle_t *dev);
int spi_device_polling_transmit(spi_device_handle_t dev,spi_transaction_t *t);

int spi_bus_free(int host);
