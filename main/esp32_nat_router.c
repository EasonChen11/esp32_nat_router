/* Console example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/usb_serial_jtag.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"

#include "cmd_decl.h"
#include <esp_http_server.h>

#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"

#include "router_globals.h"

// global all file var
#include "common.h"
// 定义并初始化全局变量
char **global_ssid_list = NULL;
int global_ssid_list_len = 0;
// scan wifi
#include "SCANWIFI.h"

// OLED
#include "OLED.h"
// On board LED
bool try_connect = false;
char try_connect_ssid[32] = {0};
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define BLINK_GPIO 44
#else
#define BLINK_GPIO 2
#endif

#define CONFIG_STRUCT_NVS 0

typedef struct
{
    uint8_t *mac;
    char *ssid;
    char *ent_username;
    char *ent_identity;
    char *passwd;
    char *static_ip;
    char *subnet_mask;
    char *gateway_addr;
} WifiSTAConfig;
typedef struct
{
    uint8_t *ap_mac;
    char *ap_ssid;
    char *ap_passwd;
    char *ap_ip;
} WifiAPConfig;
// 定义Wi-Fi配置数组和当前索引
static WifiSTAConfig *sta_configs = NULL;
static int current_wifi_index = 0;
static int current_try_count = 0;
static int32_t total_wifi_count = 0;
static int32_t total_try = 0;
#define MAX_TRY 1
static WifiSTAConfig *mem_sta_configs = NULL;
static int mem_sta_configs_len = 0;
static WifiAPConfig ap_config = {0};
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT = BIT1;
#define DEFAULT_AP_IP "192.168.4.1"
#define DEFAULT_DNS "8.8.8.8"

/* Global vars */
uint16_t connect_count = 0;
bool ap_connect = false;
bool has_static_ip = false;

uint32_t my_ip;
uint32_t my_ap_ip;

struct portmap_table_entry
{
    u32_t daddr;
    u16_t mport;
    u16_t dport;
    u8_t proto;
    u8_t valid;
};
struct portmap_table_entry portmap_tab[IP_PORTMAP_MAX];

esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;

httpd_handle_t start_webserver(void);

static const char *TAG = "ESP32 NAT router";

/* Console command history can be stored to and loaded from a file.
 * The easiest way to do this is to use FATFS filesystem on top of
 * wear_levelling library.
 */
#if CONFIG_STORE_HISTORY

#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"

static void initialize_filesystem(void)
{
    static wl_handle_t wl_handle;
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }
}
#endif // CONFIG_STORE_HISTORY

void connect_to_next_wifi();

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

esp_err_t apply_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            ip_portmap_add(portmap_tab[i].proto, my_ip, portmap_tab[i].mport, portmap_tab[i].daddr, portmap_tab[i].dport);
        }
    }
    return ESP_OK;
}

esp_err_t delete_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            ip_portmap_remove(portmap_tab[i].proto, portmap_tab[i].mport);
        }
    }
    return ESP_OK;
}

void print_portmap_tab()
{
    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid)
        {
            printf("%s", portmap_tab[i].proto == PROTO_TCP ? "TCP " : "UDP ");
            ip4_addr_t addr;
            addr.addr = my_ip;
            printf(IPSTR ":%d -> ", IP2STR(&addr), portmap_tab[i].mport);
            addr.addr = portmap_tab[i].daddr;
            printf(IPSTR ":%d\n", IP2STR(&addr), portmap_tab[i].dport);
        }
    }
}

esp_err_t get_portmap_tab()
{
    esp_err_t err;
    nvs_handle_t nvs;
    size_t len;

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_get_blob(nvs, "portmap_tab", NULL, &len);
    if (err == ESP_OK)
    {
        if (len != sizeof(portmap_tab))
        {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        }
        else
        {
            err = nvs_get_blob(nvs, "portmap_tab", portmap_tab, &len);
            if (err != ESP_OK)
            {
                memset(portmap_tab, 0, sizeof(portmap_tab));
            }
        }
    }
    nvs_close(nvs);

    return err;
}

esp_err_t add_portmap(u8_t proto, u16_t mport, u32_t daddr, u16_t dport)
{
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (!portmap_tab[i].valid)
        {
            portmap_tab[i].proto = proto;
            portmap_tab[i].mport = mport;
            portmap_tab[i].daddr = daddr;
            portmap_tab[i].dport = dport;
            portmap_tab[i].valid = 1;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK)
            {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs);
                if (err == ESP_OK)
                {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_add(proto, my_ip, mport, daddr, dport);

            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t del_portmap(u8_t proto, u16_t mport)
{
    esp_err_t err;
    nvs_handle_t nvs;

    for (int i = 0; i < IP_PORTMAP_MAX; i++)
    {
        if (portmap_tab[i].valid && portmap_tab[i].mport == mport && portmap_tab[i].proto == proto)
        {
            portmap_tab[i].valid = 0;

            err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
            if (err != ESP_OK)
            {
                return err;
            }
            err = nvs_set_blob(nvs, "portmap_tab", portmap_tab, sizeof(portmap_tab));
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs);
                if (err == ESP_OK)
                {
                    ESP_LOGI(TAG, "New portmap table stored.");
                }
            }
            nvs_close(nvs);

            ip_portmap_remove(proto, mport);
            return ESP_OK;
        }
    }
    return ESP_OK;
}

static void initialize_console(void)
{
    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    /* Drain stdout before reconfiguring it */
    fflush(stdout);
    fsync(fileno(stdout));

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_uart_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_uart_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_CRLF);

    /* Configure UART. Note that REF_TICK is used so that the baud rate remains
     * correct while APB frequency is changing in light sleep mode.
     */
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
        .source_clk = UART_SCLK_REF_TICK,
#else
        .source_clk = UART_SCLK_XTAL,
#endif
    };
    /* Install UART driver for interrupt-driven reads and writes */
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                        256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config));

    /* Tell VFS to use UART driver */
    esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);

    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };

    /* Install USB-SERIAL-JTAG driver for interrupt-driven reads and writes */
    usb_serial_jtag_driver_install(&usb_serial_jtag_config);

    /* Tell vfs to use usb-serial-jtag driver */
    esp_vfs_usb_serial_jtag_use_driver();
#endif

    /* Initialize the console */
    esp_console_config_t console_config = {
        .max_cmdline_args = 8,
        .max_cmdline_length = 256,
#if CONFIG_LOG_COLORS
        .hint_color = atoi(LOG_COLOR_CYAN)
#endif
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    /* Configure linenoise line completion library */
    /* Enable multiline editing. If not set, long commands will scroll within
     * single line.
     */
    linenoiseSetMultiLine(1);

    /* Tell linenoise where to get command completions and hints */
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);

    /* Set command history size */
    linenoiseHistorySetMaxLen(100);

#if CONFIG_STORE_HISTORY
    /* Load command history from filesystem */
    linenoiseHistoryLoad(HISTORY_PATH);
#endif
}

void *led_status_thread(void *p)
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while (true)
    {
        gpio_set_level(BLINK_GPIO, ap_connect);

        for (int i = 0; i < connect_count; i++)
        {
            gpio_set_level(BLINK_GPIO, 1 - ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, ap_connect);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
void connect_err_msg(void *event_data)
{
    wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGI(TAG, "Disconnect reason: %d", disc->reason);
    // 根据断开原因选择是否重连
    switch (disc->reason)
    {
    case WIFI_REASON_UNSPECIFIED: // 未指定的原因
        ESP_LOGI(TAG, "retrying due to unspecified reason.");
        break;
    case WIFI_REASON_AUTH_EXPIRE: // 在认证阶段之前连接已断开
        ESP_LOGI(TAG, "retrying due to auth expire.");
        break;
    case WIFI_REASON_AUTH_LEAVE: // 发送解除认证帧
        ESP_LOGI(TAG, "retrying due to auth leave.");
        break;
    case WIFI_REASON_ASSOC_EXPIRE: // 关联过期
        ESP_LOGI(TAG, "retrying due to assoc expire.");
        break;
    case WIFI_REASON_NOT_AUTHED: // 设备未经认证
        ESP_LOGI(TAG, "retrying due to not authed.");
        break;
    case WIFI_REASON_NOT_ASSOCED: // 设备未关联
        ESP_LOGI(TAG, "retrying due to not assoced.");
        break;
    case WIFI_REASON_ASSOC_LEAVE: // 发送解除关联帧
        ESP_LOGI(TAG, "retrying due to assoc leave.");
        break;
    case WIFI_REASON_ASSOC_NOT_AUTHED: // 在接收到关联请求之前，设备未经认证
        ESP_LOGI(TAG, "retrying due to assoc not authed.");
        break;
    case WIFI_REASON_IE_INVALID: // 无效信息元素
        ESP_LOGI(TAG, "retrying due to invalid IE.");
        break;
    case WIFI_REASON_MIC_FAILURE: // MIC 失败
        ESP_LOGI(TAG, "retrying due to MIC failure.");
        break;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // 四次握手超时
        ESP_LOGI(TAG, "retrying due to 4-way handshake timeout.");
        break;
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: // 组密钥更新超时
        ESP_LOGI(TAG, "retrying due to group key update timeout.");
        break;
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: // 四次握手中的IE不同
        ESP_LOGI(TAG, "retrying due to IE in 4-way differs.");
        break;
    case WIFI_REASON_BEACON_TIMEOUT: // 信标超时，设备因为一段时间内没有收到来自接入点（AP）的信标帧而断开连接。
        ESP_LOGI(TAG, "retrying due to beacon timeout.");
        break;
    case WIFI_REASON_NO_AP_FOUND: // 未找到AP
        ESP_LOGI(TAG, "retrying due to no AP found.");
        break;
    case WIFI_REASON_AUTH_FAIL: // 认证失败
        ESP_LOGI(TAG, "retrying due to auth fail.");
        break;
    case WIFI_REASON_ASSOC_FAIL: // 关联失败
        ESP_LOGI(TAG, "retrying due to assoc fail.");
        break;
    case WIFI_REASON_HANDSHAKE_TIMEOUT: // 握手超时
        ESP_LOGI(TAG, "retrying due to handshake timeout.");
        break;
    default:
        break;
    }
}
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_netif_dns_info_t dns;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // 根据断开原因选择是否重连
        connect_err_msg(event_data);
        ESP_LOGI(TAG, "disconnected - retry to connect to the AP");
        ap_connect = false;

        if (total_wifi_count >= 1)
            connect_to_next_wifi();
        ESP_LOGI(TAG, "retry to connect to the AP");
        printf("total_try: %ld\n", total_try);
        if (total_try < MAX_TRY)
        {
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
        else // stop trying
        {
            ESP_LOGI(TAG, "Max connection attempts reached, not retrying.");
            total_wifi_count = 0;
            try_connect = false;
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT); // 可以设置一个失败的事件位
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ap_connect = true;
        my_ip = event->ip_info.ip.addr;
        delete_portmap_tab();
        apply_portmap_tab();
        if (esp_netif_get_dns_info(wifiSTA, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dns);
            ESP_LOGI(TAG, "set dns to:" IPSTR, IP2STR(&(dns.ip.u_addr.ip4)));
        }
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        connect_count++;
        ESP_LOGI(TAG, "%d. station connected", connect_count);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        connect_count--;
        ESP_LOGI(TAG, "station disconnected - %d remain", connect_count);
    }
}

const int CONNECTED_BIT = BIT0;
#define JOIN_TIMEOUT_MS (2000)

/*
void wifi_init(const uint8_t *mac, const char *ssid, const char *ent_username, const char *ent_identity, const char *passwd, const char *static_ip, const char *subnet_mask, const char *gateway_addr, const uint8_t *ap_mac, const char *ap_ssid, const char *ap_passwd, const char *ap_ip)
{
    esp_netif_dns_info_t dnsserver;
    // esp_netif_dns_info_t dnsinfo;

    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    esp_netif_ip_info_t ipInfo_sta;
    if ((strlen(ssid) > 0) && (strlen(static_ip) > 0) && (strlen(subnet_mask) > 0) && (strlen(gateway_addr) > 0))
    {
        has_static_ip = true;
        ipInfo_sta.ip.addr = esp_ip4addr_aton(static_ip);
        ipInfo_sta.gw.addr = esp_ip4addr_aton(gateway_addr);
        ipInfo_sta.netmask.addr = esp_ip4addr_aton(subnet_mask);
        esp_netif_dhcpc_stop(ESP_IF_WIFI_STA); // Don't run a DHCP client
        esp_netif_set_ip_info(ESP_IF_WIFI_STA, &ipInfo_sta);
        apply_portmap_tab();
    }

    my_ap_ip = esp_ip4addr_aton(ap_ip);

    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ESP WIFI CONFIG
    wifi_config_t wifi_config = {0};
    wifi_config_t ap_config = {
        .ap = {
            .channel = 0,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .ssid_hidden = 0,
            .max_connection = 8,
            .beacon_interval = 100,
        }};

    strlcpy((char *)ap_config.sta.ssid, ap_ssid, sizeof(ap_config.sta.ssid));
    if (strlen(ap_passwd) < 8)
    {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strlcpy((char *)ap_config.sta.password, ap_passwd, sizeof(ap_config.sta.password));
    }

    if (strlen(ssid) > 0)
    {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

        // Set SSID
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        // Set passwprd
        if (strlen(ent_username) == 0)
        {
            ESP_LOGI(TAG, "STA regular connection");
            strlcpy((char *)wifi_config.sta.password, passwd, sizeof(wifi_config.sta.password));
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        if (strlen(ent_username) != 0 && strlen(ent_identity) != 0)
        {
            ESP_LOGI(TAG, "STA enterprise connection");
            if (strlen(ent_username) != 0 && strlen(ent_identity) != 0)
            {
                esp_eap_client_set_identity((uint8_t *)ent_identity, strlen(ent_identity)); // provide identity
            }
            else
            {
                esp_eap_client_set_identity((uint8_t *)ent_username, strlen(ent_username));
            }
            esp_eap_client_set_username((uint8_t *)ent_username, strlen(ent_username)); // provide username
            esp_eap_client_set_password((uint8_t *)passwd, strlen(passwd));             // provide password
            esp_wifi_sta_enterprise_enable();
        }

        if (mac != NULL)
        {
            ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_STA, mac));
        }
    }
    else
    {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

    if (ap_mac != NULL)
    {
        ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_AP, ap_mac));
    }

    // Enable DNS (offer) for dhcp server
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));

    // // Set custom dns server address for dhcp server
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton(DEFAULT_DNS);
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);

    // esp_netif_get_dns_info(ESP_IF_WIFI_AP, ESP_NETIF_DNS_MAIN, &dnsinfo);
    // ESP_LOGI(TAG, "DNS IP:" IPSTR, IP2STR(&dnsinfo.ip.u_addr.ip4));

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        pdFALSE, pdTRUE, JOIN_TIMEOUT_MS / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(ssid) > 0)
    {
        ESP_LOGI(TAG, "wifi_init_apsta finished.");
        ESP_LOGI(TAG, "connect to ap SSID: %s ", ssid);
    }
    else
    {
        ESP_LOGI(TAG, "wifi_init_ap with default finished.");
    }
}
*/

uint8_t *mac = NULL;
char *ssid = NULL;
char *ent_username = NULL;
char *ent_identity = NULL;
char *passwd = NULL;
char *static_ip = NULL;
char *subnet_mask = NULL;
char *gateway_addr = NULL;
uint8_t *ap_mac = NULL;
char *ap_ssid = NULL;
char *ap_passwd = NULL;
char *ap_ip = NULL;
WifiSTAConfig default_sta_config = {0};
/*void static_ip_config(const char *ip, const char *netmask, const char *gateway)
{
    esp_netif_ip_info_t ipInfo;
    if ((strlen(ip) > 0) && (strlen(netmask) > 0) && (strlen(gateway) > 0))
    {
        has_static_ip = true;
        ipInfo.ip.addr = esp_ip4addr_aton(ip);
        ipInfo.gw.addr = esp_ip4addr_aton(gateway);
        ipInfo.netmask.addr = esp_ip4addr_aton(netmask);
        esp_netif_dhcpc_stop(ESP_IF_WIFI_STA); // Don't run a DHCP client
        esp_netif_set_ip_info(ESP_IF_WIFI_STA, &ipInfo);
        apply_portmap_tab();
    }
}*/
void AP_IP() // 设置AP的IP地址，子网掩码，网关地址，DNS服务器地址,并启动DHCP服务器，讓AP可以分配IP给STA，可以藉由STA讓連上AP的設備獲得IP
{
    esp_netif_ip_info_t ipInfo_ap;
    my_ap_ip = esp_ip4addr_aton(ap_ip);
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);
}
void config_AP_wifi(WifiAPConfig *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "AP config is NULL");
        return;
    }
    wifi_config_t ap_config = {
        .ap = {
            .channel = 0,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .ssid_hidden = 0,
            .max_connection = 8,
            .beacon_interval = 100,
        }};

    strlcpy((char *)ap_config.ap.ssid, config->ap_ssid, sizeof(ap_config.ap.ssid));
    if (strlen(config->ap_passwd) < 8)
    {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strlcpy((char *)ap_config.ap.password, config->ap_passwd, sizeof(ap_config.ap.password));
    }

    if (config->ap_mac != NULL)
    {
        ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_AP, config->ap_mac));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
}
void config_STA_wifi(WifiSTAConfig *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "STA config is NULL");
        return;
    }
    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    if (strlen(config->ent_username) == 0)
    {
        ESP_LOGI(TAG, "STA regular connection");
        strlcpy((char *)wifi_config.sta.password, config->passwd, sizeof(wifi_config.sta.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    if (strlen(config->ent_username) != 0 && strlen(config->ent_identity) != 0)
    {
        ESP_LOGI(TAG, "STA enterprise connection");
        if (strlen(config->ent_username) != 0 && strlen(config->ent_identity) != 0)
        {
            esp_eap_client_set_identity((uint8_t *)config->ent_identity, strlen(config->ent_identity)); // provide identity
        }
        else
        {
            esp_eap_client_set_identity((uint8_t *)config->ent_username, strlen(config->ent_username));
        }
        esp_eap_client_set_username((uint8_t *)config->ent_username, strlen(config->ent_username)); // provide username
        esp_eap_client_set_password((uint8_t *)config->passwd, strlen(config->passwd));             // provide password
        esp_wifi_sta_enterprise_enable();
    }
    if (config->mac != NULL)
    {
        ESP_ERROR_CHECK(esp_wifi_set_mac(ESP_IF_WIFI_STA, config->mac));
    }
}
void config_PM_wifi()
{
    // let wifi more power
    wifi_ps_type_t ps_type = WIFI_PS_NONE;
    esp_wifi_set_ps(ps_type);
}
void DNS_server()
{
    esp_netif_dns_info_t dnsserver;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton(DEFAULT_DNS);
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);
}
void wifi_init()
{
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    AP_IP();
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    // static_ip_config(static_ip, subnet_mask, gateway_addr);
    config_AP_wifi(&ap_config);
    config_STA_wifi(&default_sta_config);
    config_PM_wifi();
    DNS_server();
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdTRUE, JOIN_TIMEOUT_MS / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_wifi_start());
}

char *param_set_default(const char *def_val)
{
    char *retval = malloc(strlen(def_val) + 1);
    strcpy(retval, def_val);
    return retval;
}

void init_wifi_STA_config(WifiSTAConfig *config, int32_t id)
{
    memset(config, 0, sizeof(WifiSTAConfig)); // 初始化结构体内存为0
    char name[32];                            // 用于存储参数名
    snprintf(name, sizeof(name), "mac%ld", id);
    get_config_param_blob(name, &(config->mac), 6);
    snprintf(name, sizeof(name), "ssid%ld", id);
    get_config_param_str(name, &(config->ssid));
    config->ssid = config->ssid ? config->ssid : param_set_default("");
    // 重复上述模式，用于其他字段
    snprintf(name, sizeof(name), "ent_username%ld", id);
    get_config_param_str(name, &(config->ent_username));
    config->ent_username = config->ent_username ? config->ent_username : param_set_default("");
    snprintf(name, sizeof(name), "ent_identity%ld", id);
    get_config_param_str(name, &(config->ent_identity));
    config->ent_identity = config->ent_identity ? config->ent_identity : param_set_default("");
    snprintf(name, sizeof(name), "passwd%ld", id);
    get_config_param_str(name, &(config->passwd));
    config->passwd = config->passwd ? config->passwd : param_set_default("");
    snprintf(name, sizeof(name), "static_ip%ld", id);
    get_config_param_str(name, &(config->static_ip));
    config->static_ip = config->static_ip ? config->static_ip : param_set_default("");
    snprintf(name, sizeof(name), "subnet_mask%ld", id);
    get_config_param_str(name, &(config->subnet_mask));
    config->subnet_mask = config->subnet_mask ? config->subnet_mask : param_set_default("");
    snprintf(name, sizeof(name), "gateway_addr%ld", id);
    get_config_param_str(name, &(config->gateway_addr));
    config->gateway_addr = config->gateway_addr ? config->gateway_addr : param_set_default("");
    return;
}
void init_wifi_AP_config(WifiAPConfig *config)
{
    memset(config, 0, sizeof(WifiAPConfig));
    get_config_param_blob("ap_mac", &(config->ap_mac), 6);
    get_config_param_str("ap_ssid", &(config->ap_ssid));
    config->ap_ssid = config->ap_ssid ? config->ap_ssid : param_set_default("ESP32_NAT_Router");
    get_config_param_str("ap_passwd", &(config->ap_passwd));
    config->ap_passwd = config->ap_passwd ? config->ap_passwd : param_set_default("");
    get_config_param_str("ap_ip", &(config->ap_ip));
    config->ap_ip = config->ap_ip ? config->ap_ip : param_set_default(DEFAULT_AP_IP);
    return;
}
int32_t Get_How_Many_WIFI_Config(void)
{
    int32_t count = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed");
        return 0;
    }
    err = nvs_get_i32(nvs, "len", &count);
    nvs_close(nvs);
    if (err != ESP_OK)
    {
        return 0;
    }
    return count;
}
void load_wifi_configs()
{
    total_wifi_count = Get_How_Many_WIFI_Config();
    printf("total_wifi_count: %ld\n", total_wifi_count);
    sta_configs = malloc(sizeof(WifiSTAConfig) * total_wifi_count);
    for (int32_t i = 0; i < total_wifi_count; i++)
    {
        init_wifi_STA_config(&sta_configs[i], i);
    }
    init_wifi_AP_config(&ap_config);
}

void connect_to_next_wifi()
{
    if (current_try_count < 3)
    {
        // 如果当前Wi-Fi尝试次数小于10次，继续尝试当前Wi-Fi
        current_try_count++;
    }
    else
    {
        // 否则，移动到下一个Wi-Fi配置
        current_try_count = 0;                                            // 重置尝试次数
        current_wifi_index = (current_wifi_index + 1) % total_wifi_count; // 循环索引
        total_try++;                                                      // 增加尝试次数
        ESP_LOGI(TAG, "Switch to next Wi-Fi configuration");
    }

    // 根据当前索引设置Wi-Fi配置
    WifiSTAConfig *cfg = &sta_configs[current_wifi_index];
    config_STA_wifi(cfg);
    ESP_LOGI(TAG, "connect to ap SSID: %s ", cfg->ssid);
    strcpy(try_connect_ssid, cfg->ssid);
    // esp_wifi_disconnect();
    // esp_wifi_connect();
}

char **ForWebServerSsidList()
{
    char **ssid_list = malloc(sizeof(char *) * mem_sta_configs_len);
    for (int i = 0; i < mem_sta_configs_len; i++)
    {
        ssid_list[i] = mem_sta_configs[i].ssid;
    }
    return ssid_list;
}

void compare_scan_and_load_wifi_configs()
{
    if (total_wifi_count == 0)
        return;
    // scan wifi is scan_wifi_list and scan_wifi_num, load wifi is sta_configs and total_wifi_count
    // compare scan_wifi_list and sta_configs
    // let sta_configs only store the wifi that is in scan_wifi_list
    int32_t new_total_wifi_count = 0;
    mem_sta_configs = malloc(sizeof(WifiSTAConfig) * total_wifi_count);
    WifiSTAConfig *new_sta_configs = malloc(sizeof(WifiSTAConfig) * total_wifi_count);
    for (int i = 0; i < total_wifi_count; i++)
    {
        // copy sta_configs to mem_sta_configs
        mem_sta_configs[i] = sta_configs[mem_sta_configs_len++];
        for (int j = 0; j < scan_wifi_num; j++)
        {
            if (strcmp(sta_configs[i].ssid, scan_wifi_list[j]) == 0)
            {
                new_sta_configs[new_total_wifi_count] = sta_configs[i];
                new_total_wifi_count++;
                break;
            }
        }
    }
    // rewrite sta_configs
    for (int i = 0; i < new_total_wifi_count; i++)
    {
        sta_configs[i] = new_sta_configs[i];
    }
    total_wifi_count = new_total_wifi_count;
    free(new_sta_configs);
}
void MemLoadWifiConfig()
{
    for (int i = 0; i < mem_sta_configs_len; i++)
    {
        sta_configs[i] = mem_sta_configs[i];
    }
    total_wifi_count = mem_sta_configs_len;
    free(mem_sta_configs);
}

// OLED
void OLED_task(void *pvParameter)
{
    OLED_text = (char *)malloc(100);
    OLED_xSemaphore = xSemaphoreCreateMutex();
    xSemaphoreTake(OLED_xSemaphore, portMAX_DELAY);
    sprintf(OLED_text, "Scan WiFi...");
    xSemaphoreGive(OLED_xSemaphore);
    OLED_app_main();
}

void OLED_display_change(void *pvParameter)
{
    int scan_index = 0;
    int delay_time;
    bool try_connect_ans = total_wifi_count > 0;
    if (total_wifi_count == 0)
    {
        xSemaphoreTake(OLED_xSemaphore, portMAX_DELAY);
        sprintf(OLED_text, "No WiFi matched");
        xSemaphoreGive(OLED_xSemaphore);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        xSemaphoreTake(OLED_xSemaphore, portMAX_DELAY);
        sprintf(OLED_text, "Scan find %d WiFi", scan_wifi_num);
        xSemaphoreGive(OLED_xSemaphore);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    while (true)
    {
        xSemaphoreTake(OLED_xSemaphore, portMAX_DELAY);
        if (try_connect)
        {
            sprintf(OLED_text, "connect %s", try_connect_ssid);
            xSemaphoreGive(OLED_xSemaphore);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        else if (try_connect_ans)
        {
            sprintf(OLED_text, "fail to connect %s", try_connect_ssid);
            xSemaphoreGive(OLED_xSemaphore);
            try_connect_ans = false;
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            strcpy(OLED_text, "");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        else if (total_wifi_count == 0 || !try_connect)
        {
            strcpy(OLED_text, scan_wifi_list[scan_index]);
            xSemaphoreGive(OLED_xSemaphore);
            OLED_text[strlen(scan_wifi_list[scan_index])] = '\0'; // remove '\n' at the end
            delay_time = strlen(scan_wifi_list[scan_index]) > 8 ? strlen(scan_wifi_list[scan_index]) - 8 : 1;
            vTaskDelay(delay_time * 3000 / portTICK_PERIOD_MS);
            scan_index = (scan_index + 1) % scan_wifi_num;
        }
    }
}

void app_main(void)
{
    // create task to OLED
    xTaskCreate(&OLED_task, "OLED_task", 4096, NULL, 5, NULL);
    ScanWifi(); // use station mode to scan wifi
    initialize_nvs();
#if CONFIG_STORE_HISTORY
    initialize_filesystem();
    ESP_LOGI(TAG, "Command history enabled");
#else
    ESP_LOGI(TAG, "Command history disabled");
#endif

#if CONFIG_STRUCT_NVS
    get_config_param_blob("mac", &mac, 6);
    get_config_param_str("ssid", &ssid);
    if (ssid == NULL)
    {
        ssid = param_set_default("");
    }
    get_config_param_str("ent_username", &ent_username);
    if (ent_username == NULL)
    {
        ent_username = param_set_default("");
    }
    get_config_param_str("ent_identity", &ent_identity);
    if (ent_identity == NULL)
    {
        ent_identity = param_set_default("");
    }
    get_config_param_str("passwd", &passwd);
    if (passwd == NULL)
    {
        passwd = param_set_default("");
    }
    get_config_param_str("static_ip", &static_ip);
    if (static_ip == NULL)
    {
        static_ip = param_set_default("");
    }
    get_config_param_str("subnet_mask", &subnet_mask);
    if (subnet_mask == NULL)
    {
        subnet_mask = param_set_default("");
    }
    get_config_param_str("gateway_addr", &gateway_addr);
    if (gateway_addr == NULL)
    {
        gateway_addr = param_set_default("");
    }
    get_config_param_blob("ap_mac", &ap_mac, 6);
    get_config_param_str("ap_ssid", &ap_ssid);
    if (ap_ssid == NULL)
    {
        ap_ssid = param_set_default("ESP32_NAT_Router");
    }
    get_config_param_str("ap_passwd", &ap_passwd);
    if (ap_passwd == NULL)
    {
        ap_passwd = param_set_default("");
    }
    get_config_param_str("ap_ip", &ap_ip);
    if (ap_ip == NULL)
    {
        ap_ip = param_set_default(DEFAULT_AP_IP);
    }
#else
    // read NVS len to check how many WIFI struct stored
    load_wifi_configs();
    compare_scan_and_load_wifi_configs();
    if (total_wifi_count > 0)
    {
        mac = sta_configs[0].mac;
        ssid = sta_configs[0].ssid;
        ent_username = sta_configs[0].ent_username;
        ent_identity = sta_configs[0].ent_identity;
        passwd = sta_configs[0].passwd;
        static_ip = sta_configs[0].static_ip;
        subnet_mask = sta_configs[0].subnet_mask;
        gateway_addr = sta_configs[0].gateway_addr;
        try_connect = true;
    }
    else
    {
        get_config_param_blob("mac", &mac, 6);
        ssid = param_set_default("");
        ent_username = param_set_default("");
        ent_identity = param_set_default("");
        passwd = param_set_default("");
        static_ip = param_set_default("");
        subnet_mask = param_set_default("");
        gateway_addr = param_set_default("");
        sprintf(OLED_text, "No WiFi Found");
        try_connect = false;
    }
    default_sta_config.mac = mac;
    default_sta_config.ssid = ssid;
    default_sta_config.ent_username = ent_username;
    default_sta_config.ent_identity = ent_identity;
    default_sta_config.passwd = passwd;
    default_sta_config.static_ip = static_ip;
    default_sta_config.subnet_mask = subnet_mask;
    default_sta_config.gateway_addr = gateway_addr;
    ap_mac = ap_config.ap_mac;
    ap_ssid = ap_config.ap_ssid;
    ap_passwd = ap_config.ap_passwd;
    ap_ip = ap_config.ap_ip;
#endif
    // create char list about ssid for webserver
    global_ssid_list_len = mem_sta_configs_len;
    global_ssid_list = ForWebServerSsidList();

    get_portmap_tab();

    // Setup WIFI
    wifi_init();

    pthread_t t1, OLED_change;
    pthread_create(&t1, NULL, led_status_thread, NULL);
    pthread_create(&OLED_change, NULL, OLED_display_change, NULL);
    ip_napt_enable(my_ap_ip, 1); // 开启NAT功能
    ESP_LOGI(TAG, "NAT is enabled");

    char *lock = NULL;
    get_config_param_str("lock", &lock);
    if (lock == NULL)
    {
        lock = param_set_default("0");
    }
    if (strcmp(lock, "0") == 0)
    {
        ESP_LOGI(TAG, "Starting config web server");
        start_webserver();
    }
    free(lock);

    initialize_console();

    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_nvs();
    register_router();

    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    const char *prompt = LOG_COLOR_I "esp32> " LOG_RESET_COLOR;

    printf("\n"
           "ESP32 NAT ROUTER\n"
           "Type 'help' to get the list of commands.\n"
           "Use UP/DOWN arrows to navigate through command history.\n"
           "Press TAB when typing command name to auto-complete.\n");

    if (strlen(ssid) == 0)
    {
        printf("\n"
               "Unconfigured WiFi\n"
               "Configure using 'set_sta' and 'set_ap' and restart.\n");
    }

    /* Figure out if the terminal supports escape sequences */
    int probe_status = linenoiseProbe();
    if (probe_status)
    { /* zero indicates success */
        printf("\n"
               "Your terminal application does not support escape sequences.\n"
               "Line editing and history features are disabled.\n"
               "On Windows, try using Putty instead.\n");
        linenoiseSetDumbMode(1);
#if CONFIG_LOG_COLORS
        /* Since the terminal doesn't support escape sequences,
         * don't use color codes in the prompt.
         */
        prompt = "esp32> ";
#endif // CONFIG_LOG_COLORS
    }

    /* Main loop */
    while (true)
    {
        /* Get a line using linenoise.
         * The line is returned when ENTER is pressed.
         */
        char *line = linenoise(prompt);
        if (line == NULL)
        { /* Ignore empty lines */
            continue;
        }
        /* Add the command to the history */
        linenoiseHistoryAdd(line);
#if CONFIG_STORE_HISTORY
        /* Save command history to filesystem */
        linenoiseHistorySave(HISTORY_PATH);
#endif

        /* Try to run the command */
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND)
        {
            printf("Unrecognized command\n");
        }
        else if (err == ESP_ERR_INVALID_ARG)
        {
            // command was empty
        }
        else if (err == ESP_OK && ret != ESP_OK)
        {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        }
        else if (err != ESP_OK)
        {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        /* linenoise allocates line buffer on the heap, so need to free it */
        linenoiseFree(line);
    }
}
