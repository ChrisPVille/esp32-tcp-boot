/*
 * WPA2-Enterprise HTTP bootloader for the ESP32
 */

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event_loop.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_ota_ops.h"

#include "mbedtls/sha1.h"

#include "tcpip_adapter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
 but we only care about one event - are we connected
 to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static const char *TAG = "bootflash";

static uint8_t macAddress_int[6];
static char macAddress[13];

/* CA cert, taken from wpa2_ca.pem
 Client cert, taken from wpa2_client.crt
 Client key, taken from wpa2_client.key

 The PEM, CRT and KEY file were provided by the person or organization
 who configured the AP with wpa2 enterprise.

 To embed it in the app binary, the PEM, CRT and KEY file is named
 in the component.mk COMPONENT_EMBED_TXTFILES variable.
 */
extern uint8_t ca_pem_start[] asm("_binary_wpa2_ca_pem_start");
extern uint8_t ca_pem_end[] asm("_binary_wpa2_ca_pem_end");
extern uint8_t client_crt_start[] asm("_binary_wpa2_client_crt_start");
extern uint8_t client_crt_end[] asm("_binary_wpa2_client_crt_end");
extern uint8_t client_key_start[] asm("_binary_wpa2_client_key_start");
extern uint8_t client_key_end[] asm("_binary_wpa2_client_key_end");

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    const unsigned int ca_pem_bytes = ca_pem_end - ca_pem_start;
    const unsigned int client_crt_bytes = client_crt_end - client_crt_start;
    const unsigned int client_key_bytes = client_key_end - client_key_start;

    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = { .sta = { .ssid = CONFIG_WIFI_SSID } };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, macAddress_int));
    for (int i = 0; i < 6; i++)
    {
        sprintf(macAddress + (i * 2), "%02x", macAddress_int[i]);
    }
    macAddress_int[12] = 0;
    ESP_LOGI(TAG,"MAC address is %s\n", macAddress);

    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_ca_cert(ca_pem_start, ca_pem_bytes));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_cert_key(client_crt_start, client_crt_bytes, client_key_start, client_key_bytes, NULL, 0));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_identity((uint8_t * )macAddress, strlen(macAddress)));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_username((uint8_t * )macAddress, strlen(macAddress)));
    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_password((uint8_t *)CONFIG_EAP_PASSWORD, strlen(CONFIG_EAP_PASSWORD)));

    ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_enable());
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void printHash(const unsigned char* hash, size_t size)
{
    char hashStr[size*2+1];
    for(int i = 0; i<size; i++)
    {
        sprintf(hashStr+(i*2), "%02X", hash[i]);
    }
    ESP_LOGD(TAG, "%s", hashStr);
}

static void wpa2_enterprise_example_task(void *pvParameters)
{
    const struct addrinfo hints =
    { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[1024];
    const esp_partition_t *update_partition = NULL;
    esp_ota_handle_t update_handle = 0 ;

    while (1)
    {
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        ESP_LOGI(TAG, "Connected to AP");

        int err = getaddrinfo(CONFIG_UPDATE_SERVER, CONFIG_UPDATE_PORT, &hints, &res);

        if (err != 0 || res == NULL)
        {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        /* Code to print the resolved IP.

         Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if (s < 0)
        {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGD(TAG, "... allocated socket");

        if (connect(s, res->ai_addr, res->ai_addrlen) != 0)
        {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, macAddress, strlen(macAddress)) < 0)
        {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        mbedtls_sha1_context updateSha1;
        mbedtls_sha1_init(&updateSha1);
        mbedtls_sha1_starts(&updateSha1);

        unsigned char proposedSHA1[20];
        unsigned char calculatedSHA1[20];

        update_partition = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
                 update_partition->subtype, update_partition->address);
        if(update_partition == NULL) ESP_ERROR_CHECK(ESP_FAIL);

        ESP_ERROR_CHECK(esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle));

        ESP_LOGI(TAG, "esp_ota_begin succeeded");

        ESP_LOGI(TAG,"Fetching update size... ");

        size_t total = 0;
        uint32_t updateSize = 0;
        do
        {
            r = read(s, (&updateSize)+total, sizeof(updateSize)-total);
            total += r;
            if(total>sizeof(updateSize))
            {
                //Overran updateSize
                ESP_ERROR_CHECK(ESP_FAIL);
            }
        } while (r > 0);

        ESP_LOGD(TAG,"Done");
        ESP_LOGD(TAG,"Update size: %d", updateSize);
        ESP_LOGI(TAG,"Fetching update checksum... ");

        total = 0;
        do
        {
            r = read(s, proposedSHA1+total, sizeof(proposedSHA1)-total);
            total += r;
            if(total>sizeof(proposedSHA1))
            {
                //Overran proposedSHA1
                ESP_ERROR_CHECK(ESP_FAIL);
            }
        } while (r > 0);

        ESP_LOGD(TAG,"Done");
        printHash(proposedSHA1, sizeof(proposedSHA1));
        ESP_LOGI(TAG,"Fetching update... ");

        total = 0;
        do
        {
            r = read(s, recv_buf, sizeof(recv_buf));

            ESP_ERROR_CHECK(esp_ota_write( update_handle, (void *)recv_buf, r));

            total += r;
            mbedtls_sha1_update(&updateSha1,(unsigned char*)recv_buf,r);
            vPortYield(); //Be polite
        } while (r > 0 && total < updateSize);

        close(s);
        ESP_LOGD(TAG,"Done");
        ESP_LOGI(TAG,"Received %d bytes", total);

        //TODO We need to handle receiving less than the expected update size

        mbedtls_sha1_finish(&updateSha1,calculatedSHA1);
        mbedtls_sha1_free(&updateSha1);

        printHash(calculatedSHA1, sizeof(calculatedSHA1));

        bool checksumPass = true;
        for(int i = 0; i<sizeof(proposedSHA1); i++)
        {
            if(proposedSHA1[i]!=calculatedSHA1[i])
            {
                checksumPass = false;
            }
        }

        ESP_ERROR_CHECK(esp_ota_end(update_handle));

        if(!checksumPass)
        {
            ESP_LOGI(TAG,"Checksum FAILED\n");
            //TODO failure?
        }
        else
        {
            ESP_LOGI(TAG,"Checksum PASS\n");
            ESP_ERROR_CHECK(esp_ota_set_boot_partition(update_partition));
            ESP_LOGI(TAG,"Booting update... ");
            esp_restart();
        }

        /*for(int countdown = 10; countdown >= 0; countdown--) {
         ESP_LOGI(TAG, "%d... ", countdown);
         vTaskDelay(1000 / portTICK_PERIOD_MS);
         }*/
        vTaskDelay(30000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Starting again!");
    }
}

void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(mbedtls_sha1_self_test(0));
    initialise_wifi();
    xTaskCreate(&wpa2_enterprise_example_task, "wpa2_enterprise_example_task", 8192, NULL, 5, NULL);

    while (1)
    {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
    /*
     for (int i = 30; i >= 0; i--) {
     printf("Restarting in %d seconds...\n", i);
     vTaskDelay(1000 / portTICK_PERIOD_MS);
     }
     printf("Restarting now.\n");*/
    //fflush(stdout);
    //esp_restart();
}
