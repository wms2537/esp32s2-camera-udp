#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "driver/gpio.h"

#include "cam.h"
#include "ov2640.h"
#include "sensor.h"

#include "sccb.h"
#include "jpeg.h"

#if defined(CONFIG_IPV4)
#define HOST_IP_ADDR CONFIG_IPV4_ADDR
#elif defined(CONFIG_IPV6)
#define HOST_IP_ADDR CONFIG_IPV6_ADDR
#else
#define HOST_IP_ADDR ""
#endif

#define PORT CONFIG_PORT
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#define DEFAULT_LISTEN_INTERVAL 3 // unit is beacon interval
#define EXAMPLE_ESP_MAXIMUM_RETRY 10

#define CAM_WIDTH (640)
#define CAM_HIGH (480)

#define CAM_XCLK GPIO_NUM_7
#define CAM_PCLK GPIO_NUM_10
#define CAM_VSYNC GPIO_NUM_4
#define CAM_HREF GPIO_NUM_5
#define CAM_D2 GPIO_NUM_39
#define CAM_D3 41
#define CAM_D4 42
#define CAM_D5 40
#define CAM_D6 GPIO_NUM_38
#define CAM_D7 GPIO_NUM_9
#define CAM_D8 GPIO_NUM_8
#define CAM_D9 GPIO_NUM_6
#define CAM_SCL GPIO_NUM_3
#define CAM_SDA GPIO_NUM_1

#define CONFIG_CAMERA_OV2640
#define CONFIG_CAMERA_JPEG_MODE

static const char *TAG = "example";
static const char *payload = "Message from ESP32 ";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static MessageBufferHandle_t frame_buffer;

static void udp_client_task(void *pvParameters)
{
  char rx_buffer[128];
  char host_ip[] = HOST_IP_ADDR;
  int addr_family = 0;
  int ip_protocol = 0;

  while (1)
  {

#if defined(CONFIG_IPV4)
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_IPV6)
    struct sockaddr_in6 dest_addr = {0};
    inet6_aton(HOST_IP_ADDR, &dest_addr.sin6_addr);
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_port = htons(PORT);
    dest_addr.sin6_scope_id = esp_netif_get_netif_impl_index(INTERFACE);
    addr_family = AF_INET6;
    ip_protocol = IPPROTO_IPV6;
#elif defined(CONFIG_SOCKET_IP_INPUT_STDIN)
    struct sockaddr_in6 dest_addr = {0};
    ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_DGRAM, &ip_protocol, &addr_family, &dest_addr));
#endif

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0)
    {
      ESP_LOGE("UDP TASK", "Unable to create socket: errno %d", errno);
      break;
    }
    ESP_LOGI("UDP TASK", "Socket created, sending to %s:%d", HOST_IP_ADDR, PORT);

    while (1)
    {
      uint8_t cam_buf[20000];
      size_t frame_size = xMessageBufferReceive(frame_buffer, (void *)cam_buf, sizeof(cam_buf), 0);
      if (frame_size > 0)
      {
        int err = sendto(sock, &cam_buf, frame_size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0)
        {
          ESP_LOGE("UDP TASK", "Error occurred during sending: errno %d", errno);
          break;
        }
        ESP_LOGI("UDP TASK", "Message sent");
      }
      vTaskDelay(1); // prevent WDT reset

      // struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
      // socklen_t socklen = sizeof(source_addr);
      // int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

      // // Error occurred during receiving
      // if (len < 0)
      // {
      //   ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
      //   break;
      // }
      // // Data received
      // else
      // {
      //   rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
      //   ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
      //   ESP_LOGI(TAG, "%s", rx_buffer);
      //   if (strncmp(rx_buffer, "OK: ", 4) == 0)
      //   {
      //     ESP_LOGI(TAG, "Received expected message, reconnecting");
      //     break;
      //   }
      // }

      // vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    if (sock != -1)
    {
      ESP_LOGE(TAG, "Shutting down socket and restarting...");
      shutdown(sock, 0);
      close(sock);
    }
  }
  vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
    {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
    else
    {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void)
{
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = CONFIG_WIFI_SSID,
          .password = CONFIG_WIFI_PASS,
          /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
          .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          .listen_interval = DEFAULT_LISTEN_INTERVAL,

          .pmf_cfg = {
              .capable = true,
              .required = false},
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  // ESP_LOGI(TAG, "esp_wifi_set_ps().");
  // esp_wifi_set_ps(DEFAULT_PS_MODE);

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE,
                                         pdFALSE,
                                         portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
  if (bits & WIFI_CONNECTED_BIT)
  {
    ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
             CONFIG_WIFI_SSID, CONFIG_WIFI_PASS);
  }
  else if (bits & WIFI_FAIL_BIT)
  {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
             CONFIG_WIFI_SSID, CONFIG_WIFI_PASS);
  }
  else
  {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  /* The event will not be processed after unregister */
  ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
  ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
  vEventGroupDelete(s_wifi_event_group);
}

static void cam_task(void *arg)
{
  if (gpio_set_level(GPIO_NUM_37, 0) != ESP_OK)
  {
    ESP_LOGI("CAM TASK", "GPIO failed to init");
  }
  ESP_LOGI("CAM TASK", "GPIO init");
  cam_config_t cam_config = {
      .bit_width = 8,
      .mode.jpeg = 1,
      .xclk_fre = 16 * 1000 * 1000,
      .pin = {
          .xclk = CAM_XCLK,
          .pclk = CAM_PCLK,
          .vsync = CAM_VSYNC,
          .hsync = CAM_HREF,
      },
      .pin_data = {CAM_D2, CAM_D3, CAM_D4, CAM_D5, CAM_D6, CAM_D7, CAM_D8, CAM_D9},
      .vsync_invert = true,
      .hsync_invert = false,
      .size = {
          .width = CAM_WIDTH,
          .high = CAM_HIGH,
      },
      .max_buffer_size = 8 * 1024,
      .task_stack = 1024,
      .task_pri = configMAX_PRIORITIES};

  /*!< With PingPang buffers, the frame rate is higher, or you can use a separate buffer to save memory */
  cam_config.frame1_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  cam_config.frame2_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  ESP_LOGI("CAM TASK", "Starting init sensor");
  cam_init(&cam_config);

  sensor_t sensor;
  int camera_version = 0; /*!<If the camera version is determined, it can be set to manual mode */
  SCCB_Init(CAM_SDA, CAM_SCL);
  sensor.slv_addr = SCCB_Probe();
  ESP_LOGI("CAM TASK", "sensor_id: 0x%x\n", sensor.slv_addr);

#ifdef CONFIG_CAMERA_OV2640
  camera_version = 2640;
#endif
#ifdef CONFIG_CAMERA_AUTO
  /*!< If you choose this mode, Dont insert the Audio board, audio will affect the camera register read. */
#endif
  if (sensor.slv_addr == 0x30 || camera_version == 2640)
  { /*!< Camera: OV2640 */
    ESP_LOGI("CAM TASK", "OV2640 init start...");

    if (OV2640_Init(0, 1) != 0)
    {
      goto fail;
    }

    if (cam_config.mode.jpeg)
    {
      OV2640_JPEG_Mode();
    }
    else
    {
      OV2640_RGB565_Mode(false); /*!< RGB565 mode */
    }

    OV2640_ImageSize_Set(800, 600);
    OV2640_ImageWin_Set(0, 0, 800, 600);
    OV2640_OutSize_Set(CAM_WIDTH, CAM_HIGH);
  }
  else
  {
    ESP_LOGE("CAM TASK", "sensor is temporarily not supported\n");
    goto fail;
  }

  ESP_LOGI("CAM TASK", "camera init done\n");
  cam_start();

  while (1)
  {
    int64_t before = esp_timer_get_time();
    uint8_t *cam_buf = NULL;
    ESP_LOGI("CAM TASK", "taking buffer...");
    size_t len = cam_take(&cam_buf);

    ESP_LOGI("CAM TASK", "size: %d", len);
    if (xMessageBufferSend(frame_buffer, (void *)cam_buf, len, 0) != len)
    {
      ESP_LOGI(TAG, "Queue full");
    }
    // esp_websocket_client_send_bin(client, (const char *)cam_buf, len, portMAX_DELAY);
    int64_t after = esp_timer_get_time();
    ESP_LOGI("CAM TASK", "Sent, time: %lld", after - before);

    cam_give(cam_buf);
    vTaskDelay(1); // prevent WDT reset
                   /*!< Use a logic analyzer to observe the frame rate */
  }

fail:
  free(cam_config.frame1_buffer);
  free(cam_config.frame2_buffer);
  cam_deinit();
  vTaskDelete(NULL);
}

void app_main(void)
{
  gpio_set_direction(GPIO_NUM_37, GPIO_MODE_OUTPUT);
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }

  wifi_init_sta();
  frame_buffer = xMessageBufferCreate(20000);
  if (frame_buffer == NULL)
  {
    ESP_LOGI("MAIN TASK", "Frame Buffer allocation failed");
  }
  else
  {
    xTaskCreate(cam_task, "cam_task", 8192, NULL, configMAX_PRIORITIES, NULL);
    xTaskCreate(udp_client_task, "udp_client", 32768, NULL, configMAX_PRIORITIES, NULL);
  }
}
