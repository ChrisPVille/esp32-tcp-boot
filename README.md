# ESP32 TCP Bootloader
A simple TCP Socket bootloader for the ESP32 intended for use on WPA2-Enterprise networks.

```
I (804) boot: Loading app partition at offset 00010000 
I (4739) bootflash: Fetching update... 
I (5389) bootflash: Booting update... 

I (809) boot: Loading app partition at offset 00100000
Hello world!
```

## Concept
To avoid the waste of the usual factory program and two identical OTA partitions, this small bootloader is intended to flash a single large OTA partition containing the primary application.  On the ESP-WROOM-32, this results in a comfortable 3MB for the main application.  (See the [partition table](partitions.csv)).

### WiFi Connection
The bootloader attempts to connect to the WPA2-Enterprise network provided during the `make menuconfig` step using its MAC address as identity/username and the configured password.  Certificates are embedded into the application at build time (see [the Espressif WPA2-Enterprise example](https://github.com/espressif/esp-idf/tree/master/examples/wifi/wpa2_enterprise) for details).  This enables every esp to have a unique set of credentials known only to it and the RADIUS server, eliminating the need to reprogram every device even if a unit is lost or compromised.

### Image Download
Once connected to the network, the bootloader opens a TCP socket to the computer and port provided during the configuration step, sends the MAC address of the ESP, and waits for the OTA image.  The TCP server could deliver different images to devices depending on their MAC address, using knowledge of their individual hardware configuration (the example does not do this yet).

Due to RAM constraints, the image is flashed unconditionally while a running SHA1 checksum is calculated.  Once flashing is complete, the SHA1 sum observed during programming is compared with the value provided by the TCP server.  If this sum matches, the OTAData partition is updated, marking the delivered OTA application bootable.  If this hash fails, the OTA partition is not marked bootable, and the update process restarts.

#### Note on the checksum
As the TCP channel is supposedly reliable, the SHA1 sum during the update process is expected to fail very infrequently and is simply a confidence boosting measure.  If a less reliable channel was to be used for communication, it makes some sense to break up the OTA image into chunks small enough to fit into the ESP's RAM and compute the hashes before flashing.  Again, the TCP stack should insulate us from a lot of that, so a single checksum is used for simplicity.

## Running the Image Server

Included is an example [Node.js tcp server](test/tcp-server.js) handing out the flashable image to all who connect.  This server can be run with `node tcp-server.js` in the directory containing the target image.  The flashable image (`app.bin` by default) is delivered with a simple fixed header format: 


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
