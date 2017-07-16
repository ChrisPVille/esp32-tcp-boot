# ESP32 TCP Bootloader
A simple TCP Socket bootloader for the ESP32 intended for use on WPA2-Enterprise networks.

## Concept

To avoid the waste of the usual factory program and two identical OTA partitions, this small bootloader is intended to flash a single large OTA partition containing the primary application.  On the ESP-WROOM-32, this results in a comfortable 3MB for the main application.  (See the [partition table](partitions.csv)).

## Running the Image Server

Included is an example [Node.js tcp server](test/tcp-server.js) handing out a program to all who connect.  The flashable image is delivered with a simple fixed header format: 


| Description  | Length |
| ------------- | ------------- |
| Payload size | 4 |
| Payload SHA1Sum | 20 |
| Payload | N |

## Returning to bootloader
To return to this bootloader, the ESP32 needs to be told the OTA delivered application is not bootable.  A simple way to do this is to erase the OTAData partition (a small metadata block) and reboot, letting the ESP32 fall-back onto our loader:

```c
const esp_partition_t *otadata_partition = \
esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);

ESP_ERROR_CHECK(esp_partition_erase_range(otadata_partition, 0, otadata_partition->size));
```
