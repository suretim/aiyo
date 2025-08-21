/**
 * This example takes a picture every 5s and print its size on serial monitor.
 */

// =============================== SETUP ======================================

// 1. Board setup (Uncomment):
// #define BOARD_WROVER_KIT
// #define BOARD_ESP32CAM_AITHINKER
// #define BOARD_ESP32S3_WROOM
// #define BOARD_ESP32S3_GOOUUU

/**
 * 2. Kconfig setup
 *
 * If you have a Kconfig file, copy the content from
 *  https://github.com/espressif/esp32-camera/blob/master/Kconfig into it.
 * In case you haven't, copy and paste this Kconfig file inside the src directory.
 * This Kconfig file has definitions that allows more control over the camera and
 * how it will be initialized.
 */

/**
 * 3. Enable PSRAM on sdkconfig:
 *
 * CONFIG_ESP32_SPIRAM_SUPPORT=y
 *
 * More info on
 * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-esp32-spiram-support
 */

// ================================ CODE ======================================

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>

#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// support IDF 5.x
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#include "esp_camera.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include <sys/socket.h>
#include <esp_netif.h>
#include "esp_timer.h"
#include <pthread.h>

#define CAM_PIN_PWDN    -1  //power down is not used
#define CAM_PIN_RESET   -1  //software reset will be performed
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5

#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11

#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

static const char *TAG = "http_web";

unsigned char interpreter_en = 0;
httpd_handle_t server = NULL;

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

                   // |--用于图像显示  PIXFORMAT_RGB888不支持
    //.pixel_format = PIXFORMAT_JPEG,//PIXFORMAT_RGB565, //YUV422,GRAYSCALE,RGB565,JPEG
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,//FRAMESIZE_QVGA,    //QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = 12, //0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 2,       //When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

#ifndef c_ret_ok
#define c_ret_ok        0
#define c_ret_nk        1
#endif

static unsigned int tick_get(void)
{
    return esp_timer_get_time() / 1000;
}
static unsigned int tick_cmp(unsigned int tmr, unsigned int tmo)
{
    return (tick_get() - tmr >= tmo) ? c_ret_ok : c_ret_nk;
}

static esp_err_t init_camera(void)
{
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}


#include "esp_wifi.h"
#include "esp_netif.h"

#define c_wifi_cfg_ap           0
#define c_wifi_cfg_sta          1
#define c_wifi_cfg              c_wifi_cfg_ap
#if c_wifi_cfg == c_wifi_cfg_sta
static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                             int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA启动完成");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "已连接到AP");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "连接断开，尝试重连...");
                esp_wifi_connect();  // 自动重连
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获取到IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init(void) {
    // 1. 初始化基础网络组件
    esp_netif_init();
    esp_event_loop_create_default();
    
    // 2. 创建默认STA接口并注册事件回调
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // 3. 注册WiFi和IP事件处理
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // 4. 初始化WiFi驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. 配置STA参数
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "HONOR80",
            .password = "1234567890",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // 加密方式
            .pmf_cfg = {
                .capable = true,  // 启用WPA3增强安全
                .required = false
            }
        }
    };

    // 6. 设置模式并启动
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    
    // 可选优化参数
    esp_wifi_set_ps(WIFI_PS_NONE);  // 禁用省电模式，提升稳定性
    esp_wifi_set_max_tx_power(70);
    //esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);  // 指定信道减少干扰
    
    // 7. 启动WiFi并连接
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());  // 主动触发连接

    ESP_LOGI(TAG, "STA模式已启动,正在连接HONOR80...");
}

#else
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (id == WIFI_EVENT_AP_START) ESP_LOGI(TAG, "AP启动成功");
    else if (id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "设备接入");
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "设备断开");
}

void wifi_init(void) 
{
    esp_netif_init();
    esp_event_loop_create_default();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32-CAM-AP",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    
    //esp_wifi_set_ps(WIFI_PS_NONE);  // 禁用省电模式，避免频繁协商
    esp_wifi_set_max_tx_power(80);  // 设置最大发射功率（单位0.25dBm）
    //esp_wifi_set_max_tx_power(70);
    //esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);   //Wi-Fi 4 (802.11n)
    //esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);  // 指定信道6减少干扰

    esp_wifi_start();

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    ESP_LOGI(TAG, "got ip:" IPSTR "\n", IP2STR(&ip_info.ip));
}
#endif

#include "esp_http_server.h"
#include "img_converters.h"
#include "cJSON.h"

#include "tflm.h"

SemaphoreHandle_t web_send_mutex;

#define c_web_http          0
#define c_web_socket        1
#define c_web_server        c_web_socket

#if c_web_server == c_web_http
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t stream_handler(httpd_req_t *req);
static esp_err_t control_handler(httpd_req_t *req);
static const httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t control_uri = {
    .uri       = "/control",
    .method    = HTTP_GET,
    .handler   = control_handler,
    .user_ctx  = NULL
};

// 注册URI路由
httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler
};

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if(ESP_OK != httpd_start(&server, &config))
        ESP_LOGE(TAG, "httpd_start failed");
    if(ESP_OK != httpd_register_uri_handler(server, &stream_uri))
        ESP_LOGE(TAG, "httpd_register_uri_handler_stream failed");
    if(ESP_OK != httpd_register_uri_handler(server, &index_uri))    
        ESP_LOGE(TAG, "httpd_register_uri_handler_index failed");    
    if(ESP_OK != httpd_register_uri_handler(server, &control_uri))    
        ESP_LOGE(TAG, "httpd_register_uri_handler_control failed");    
    #if c_wifi_cfg == c_wifi_cfg_ap    
    ESP_LOGI("MAIN", "Server ready at http://192.168.4.1");
    #endif
    web_send_mutex = xSemaphoreCreateMutex();
    return server;
}

void stop_webserver() {
    if (server) {
        httpd_stop(server); // 阻塞式停止，确保所有连接关闭
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
        vTaskDelay(500 / portTICK_PERIOD_MS); // 等待资源释放
    }
}

static void restart_task(void *arg) {
    stop_webserver();
    start_webserver();
    vTaskDelete(NULL);
}

extern const uint8_t _binary_index_http_html_start[] asm("_binary_index_http_html_start");
extern const uint8_t _binary_index_http_html_end[] asm("_binary_index_http_html_end");

// 定义URI处理器
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, 
        (const char *)_binary_index_http_html_start, 
        _binary_index_http_html_end - _binary_index_http_html_start
    );
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;
    
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = (char *)malloc(buf_len);
        if (!buf)
        {
            if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                httpd_resp_send_500(req);
                xSemaphoreGive(web_send_mutex);
            }
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
        httpd_resp_send_404(req);
        xSemaphoreGive(web_send_mutex);
    }
    return ESP_FAIL;
}

static esp_err_t control_handler(httpd_req_t *req) {
    char buf[128];
    ESP_LOGI("MAIN", "control_handler run");
    // 1. 获取URL查询字符串
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
            httpd_resp_send_404(req);
            xSemaphoreGive(web_send_mutex);
        }
        return ESP_FAIL;
    }

    // 2. 解析cmd参数
    char cmd[32];
    if (httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd)) != ESP_OK) {
        if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
            httpd_resp_send_404(req);
            xSemaphoreGive(web_send_mutex);
        }
        return ESP_FAIL;
    }

    // 3. 处理命令并响应
    ESP_LOGI(TAG, "Execute command: %s", cmd);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"cmd\":\"%s\"}", cmd);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
        httpd_resp_send(req, resp, strlen(resp));
        xSemaphoreGive(web_send_mutex);
    }
    return ESP_OK;
}

//流处理程序
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char *part_buf[128];
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    int64_t frame_tmr;
    unsigned int tmr;

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");

    while (true)
    {
        frame_tmr = esp_timer_get_time();
        //获取指向帧缓冲区的指针
        fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                httpd_resp_send_500(req);
                xSemaphoreGive(web_send_mutex);
            }
            break;
        }
                
        if(fb->format == PIXFORMAT_JPEG)
        {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            size_t len = snprintf(part_buf, 128,
                "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n");

            if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                res = httpd_resp_send_chunk(req, part_buf, len);
                if(res == ESP_OK) {
                    res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
                }
                xSemaphoreGive(web_send_mutex);
            }
            if (res != ESP_OK)
            {
                xTaskCreate(restart_task, "restart_task", 4096, NULL, 5, NULL);
                return ESP_FAIL;
            }
        }
        else
        {
            res = ESP_OK;
            uint8_t *rgb888_buf = heap_caps_malloc(fb->width * fb->height * 3,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(rgb888_buf != NULL)
            {
                if(fmt2rgb888(fb->buf, fb->len, fb->format, rgb888_buf) != true)
                {
                    ESP_LOGE(TAG, "fmt2rgb888 failed, fb: %d", fb->len);
                    esp_camera_fb_return(fb);
                    free(rgb888_buf);
                    res = ESP_FAIL;
                }
                else
                {
                    fb_data_t fb_data;

                    fb_data.width = fb->width;
                    fb_data.height = fb->height;
                    fb_data.data = rgb888_buf;

                    // rectangle box
                    int x, y, w, h;
                    uint32_t color = 0x0000ff00;                
                    x = 40;
                    y = 40;
                    w = 160;
                    h = 120;
                    fb_gfx_drawFastHLine(&fb_data, x, y, w, color);
                    fb_gfx_drawFastHLine(&fb_data, x, y + h - 1, w, color);
                    fb_gfx_drawFastVLine(&fb_data, x, y, h, color);
                    fb_gfx_drawFastVLine(&fb_data, x + w - 1, y, h, color);
                }            
            }

            if(res == ESP_OK)
            {
                _jpg_buf = NULL;                                                                                              //90          
                bool jpeg_converted = fmt2jpg(rgb888_buf, fb->width * fb->height * 3, fb->width, fb->height, PIXFORMAT_RGB888, 60,  &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                free(rgb888_buf);
                if (!jpeg_converted) {
                    ESP_LOGE(TAG, "JPEG compression failed");
                    if (_jpg_buf) free(_jpg_buf);
                }
                else {
                    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                    tmr = (esp_timer_get_time() - frame_tmr) / 1000;
                    size_t len = snprintf(part_buf, 128,
                        "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n");
                    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                        res = httpd_resp_send_chunk(req, part_buf, len);
                        if(res == ESP_OK) {
                            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
                        }
                        xSemaphoreGive(web_send_mutex);
                    }

                    if (_jpg_buf) free(_jpg_buf);

                    if (res != ESP_OK)
                    {
                        xTaskCreate(restart_task, "restart_task", 4096, NULL, 5, NULL);
                        return ESP_FAIL;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return res;
}

#else 
#define MAX_CLIENTS 3
struct st_client_sockets
{
    int sockfd;
    unsigned int tmr;
    unsigned int en;
};
static struct st_client_sockets client_sockets[MAX_CLIENTS] = {0};

static float frame_rate = 0;

static void add_client(int sockfd) {
    int i;

    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i].sockfd == sockfd) {
                client_sockets[i].tmr = tick_get();
                client_sockets[i].en = 0;
                xSemaphoreGive(web_send_mutex);
                ESP_LOGI(TAG, "Client %d exist", sockfd);
                return;
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i].sockfd == 0) {
                client_sockets[i].sockfd = sockfd;
                client_sockets[i].tmr =tick_get();
                client_sockets[i].en = 0;
                xSemaphoreGive(web_send_mutex);
                ESP_LOGI(TAG, "Client %d null add", sockfd);
                return;
            }
        }
        if(i >= MAX_CLIENTS) {            
            close(client_sockets[0].sockfd);
            client_sockets[0] = client_sockets[1];
            client_sockets[1] = client_sockets[2];
            client_sockets[2].sockfd = sockfd;            
            client_sockets[2].tmr = tick_get();
            client_sockets[2].en = 0;
            xSemaphoreGive(web_send_mutex);
            ESP_LOGI(TAG, "Client %d full add", sockfd);
        }
    } 
}

static void remove_client(int sockfd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i].sockfd == sockfd) {    
            client_sockets[i].sockfd = 0;                
            ESP_LOGI(TAG, "Client %d disconnected", sockfd);
            break;
        }
    }    
}

static void process_command(httpd_req_t *req, const char *command) {
    char response[128];

    response[0] = 0;
    if(strcmp(command, "help") == 0) {
        sprintf(response, "help fps=? tflite=1");
    }
    else if(strcmp(command, "fps=?") == 0) {
        sprintf(response, "fps=%.1f", frame_rate);
    }    
    else if(strcmp(command, "tflite=1") == 0) {
        interpreter_en = 1;
        return;
    }    

    if(response[0] == 0) {
        snprintf(response, 127, "%s", command);
    }

    httpd_ws_frame_t resp = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)response,
        .len = strlen(response)
    };
    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
        httpd_ws_send_frame(req, &resp);
        xSemaphoreGive(web_send_mutex);
    }
}

static void handle_save_image(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    cJSON *root = cJSON_Parse((char*)ws_pkt->payload);
    if (!root) return;
    
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *filename = cJSON_GetObjectItem(root, "filename");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    
    if (cJSON_IsString(action) && strcmp(action->valuestring, "save_image") == 0 &&
        cJSON_IsString(filename) && cJSON_IsArray(data)) {
                
        const char *response = "Image save request received";
        httpd_ws_frame_t resp = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)response,
            .len = strlen(response)
        };
        if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
            httpd_ws_send_frame(req, &resp);
            xSemaphoreGive(web_send_mutex);
        }
    }
    
    cJSON_Delete(root);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
        return ESP_OK;
}

const char *ws_resp_type[] = {"CONTINUE", "TEXT", "BINARY", "null","null","null","null","null","CLOSE","PING","PONG"};
static esp_err_t websocket_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive frame: %s", esp_err_to_name(ret));
        return ret;
    }

    if(ws_pkt.type < sizeof(ws_resp_type)/sizeof(ws_resp_type[0]))
    {
        ESP_LOGI(TAG, "ws_pkt.type=%s", ws_resp_type[ws_pkt.type]);
    }
    else
    {
        ESP_LOGI(TAG, "ws_pkt.type=ERROR");
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
            int sockfd = httpd_req_to_sockfd(req);
            remove_client(sockfd);
            xSemaphoreGive(web_send_mutex);
            return ESP_OK;
        }
    }    

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "ws_pkt.payload=%s", ws_pkt.payload);
            process_command(req, (char*)ws_pkt.payload);
        }
        free(buf);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        handle_save_image(req, &ws_pkt);
    }
    
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    extern const char index_socket_html_start[] asm("_binary_index_socket_html_start");
    extern const char index_socket_html_end[] asm("_binary_index_socket_html_end");
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_socket_html_start, index_socket_html_end - index_socket_html_start);
}

void video_stream_task(void *pvParameters) {
    static unsigned int frame_tmr = 0;
    static unsigned int frame_cnt = 0;

    httpd_handle_t server = (httpd_handle_t)pvParameters;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    esp_err_t res = ESP_OK;
    
    frame_tmr = tick_get();
    while (1) {        
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if(fb->format == PIXFORMAT_JPEG)
        {
            if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_sockets[i].sockfd > 0) {                    
                        if(client_sockets[i].en == 0)
                        {
                            if(tick_cmp(client_sockets[i].tmr, 100) == c_ret_ok)     
                                client_sockets[i].en = 1;
                        }
                        else
                        {
                            httpd_ws_frame_t ws_pkt = {
                                .final = true,
                                .fragmented = false,
                                .type = HTTPD_WS_TYPE_BINARY,
                                .payload = fb->buf,
                                .len = fb->len
                            };                
                            if(httpd_ws_send_frame_async(server, client_sockets[i].sockfd, &ws_pkt) != ESP_OK)
                            {
                                close(client_sockets[i].sockfd);
                                ESP_LOGI(TAG, "Client %d disconnected", client_sockets[i].sockfd);
                                client_sockets[i].sockfd = 0;
                            }
                        }
                    }
                }
                xSemaphoreGive(web_send_mutex);
            }
            
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(50));   //30
        }
        else
        {
            res = ESP_OK;
            uint8_t *rgb888_buf = heap_caps_malloc(fb->width * fb->height * 3,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(rgb888_buf != NULL)
            {
                if(fmt2rgb888(fb->buf, fb->len, fb->format, rgb888_buf) != true)
                {
                    ESP_LOGE(TAG, "fmt2rgb888 failed, fb: %d", fb->len);
                    esp_camera_fb_return(fb);
                    free(rgb888_buf);
                    res = ESP_FAIL;
                }
                else if(interpreter_en != 0)
                {               
                    interpreter_en = 2;
                    printf("run_inference start\r\n");                         
                    run_inference(rgb888_buf);
                    printf("run_inference stop\r\n"); 
                }            
            }

            if(res == ESP_OK)
            {
                _jpg_buf = NULL;                                                                                              //90        
                bool jpeg_converted = fmt2jpg(rgb888_buf, fb->width * fb->height * 3, fb->width, fb->height, PIXFORMAT_RGB888, 60,  &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                free(rgb888_buf);
                if (!jpeg_converted) {
                    ESP_LOGE(TAG, "JPEG compression failed");
                    if (_jpg_buf) free(_jpg_buf);
                }
                else {
                    if (xSemaphoreTake(web_send_mutex, portMAX_DELAY) == pdTRUE) {
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (client_sockets[i].sockfd > 0) {                            
                                if(client_sockets[i].en == 0)
                                {
                                    if(tick_cmp(client_sockets[i].tmr, 100) == c_ret_ok)
                                        client_sockets[i].en = 1;
                                }
                                else
                                {
                                    httpd_ws_frame_t ws_pkt = {
                                        .final = true,
                                        .fragmented = false,
                                        .type = HTTPD_WS_TYPE_BINARY,
                                        .payload = _jpg_buf,
                                        .len = _jpg_buf_len
                                    };                
                                    if(httpd_ws_send_frame_async(server, client_sockets[i].sockfd, &ws_pkt) != ESP_OK)
                                    {
                                        close(client_sockets[i].sockfd);
                                        ESP_LOGI(TAG, "Client %d disconnected", client_sockets[i].sockfd);
                                        client_sockets[i].sockfd = 0;
                                    }
                                }
                            }
                        }
                        xSemaphoreGive(web_send_mutex);
                    }
                    if (_jpg_buf) free(_jpg_buf);            
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                if(interpreter_en == 2)
                {
                    interpreter_en = 0;
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
        } 
        frame_cnt++;
        if(tick_cmp(frame_tmr, 10000) == c_ret_ok)
        {
            frame_tmr = tick_get();
            frame_rate = frame_cnt / 10.0f;
            frame_cnt = 0;
            ESP_LOGI(TAG, "frame_rate=%.1f", frame_rate);

            wifi_sta_list_t wifi_sta_list = {0};
            if(esp_wifi_ap_get_sta_list(&wifi_sta_list) == ESP_OK)
            {
                for(int i = 0; i < wifi_sta_list.num; i++)
                {
                    ESP_LOGI(TAG, "num:%d,rssi:%d", i, wifi_sta_list.sta[i].rssi);
                }
            }
        }
    }
}

httpd_handle_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.max_resp_headers = 16;
    config.max_open_sockets = MAX_CLIENTS;
    config.server_port = 81;
    config.lru_purge_enable = true;       // 启用LRU清理
    config.recv_wait_timeout = 5;         // 接收超时
    config.send_wait_timeout = 5;         // 发送超时    

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t favicon_uri = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = favicon_handler,  // 空处理
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &favicon_uri);        

        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = websocket_handler,
            .user_ctx = NULL,
            .is_websocket = true,
            .handle_ws_control_frames = true    // 确保能处理网页刷新和网页关闭
        };
        httpd_register_uri_handler(server, &ws_uri);
        
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        xTaskCreate(video_stream_task, "video_stream", 40960, server, 5, NULL);
    }
    web_send_mutex = xSemaphoreCreateMutex();
    return server;
}
#endif


#include "esp_task_wdt.h"

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_task_wdt_deinit();
    #ifdef CONFIG_ESP_INT_WDT_ENABLED
    esp_int_wdt_disable();
    #endif


    wifi_init();
    tflite_init();
    init_camera();
    start_webserver();    
}

