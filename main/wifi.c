/*
  wifi.c - An embedded CNC Controller with rs274/ngc (g-code) support

  WiFi comms

  Part of grblHAL

  Copyright (c) 2018-2022 Terje Io

  Some parts of the code is based on example code by Espressif, in the public domain

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"

#if WIFI_ENABLE

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include "nvs_flash.h"

#include "networking/networking.h"
#include "networking/utils.h"
//#include "lwip/timeouts.h"

#include "wifi.h"
#include "dns_server.h"
#include "web/backend.h"
#include "grbl/report.h"
#include "grbl/nvs_buffer.h"
#include "grbl/protocol.h"


/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const static int CONNECTED_BIT = BIT0;
const static int SCANNING_BIT = BIT1;
const static int APSTA_BIT = BIT2;

static EventGroupHandle_t wifi_event_group;
static stream_type_t active_stream = StreamType_Null;
static network_settings_t network;
static network_services_t services = {0}, allowed_services;
static wifi_config_t wifi_sta_config;
static SemaphoreHandle_t aplist_mutex = NULL;
static ap_list_t ap_list = {0};
static wifi_settings_t wifi;
static nvs_address_t nvs_address;
static esp_netif_t *sta_netif = NULL, *ap_netif = NULL;
static on_report_options_ptr on_report_options;
static on_stream_changed_ptr on_stream_changed;
static char netservices[40] = ""; // must be large enough to hold all service names

ap_list_t *wifi_get_aplist (void)
{
    if(ap_list.ap_records && xSemaphoreTake(aplist_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        return &ap_list;
    else
        return NULL;
}

void wifi_release_aplist (void)
{
    xSemaphoreGive(aplist_mutex);
}

char *iptoa (void *ip) {
    static char aip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET, ip, aip, INET6_ADDRSTRLEN);
    return aip;
}

char *wifi_get_ipaddr (void)
{
    ip4_addr_t *ip;

#if NETWORK_IPMODE_STATIC
    ip = (ip4_addr_t *)&wifi.sta.network.ip;
#else
    ip = ap_list.ap_selected ? &ap_list.ip_addr : (ip4_addr_t *)&wifi.ap.network.ip;
#endif

    return iptoa(ip);
}

char *wifi_get_mac (void)
{
    static char mac[18];
    uint8_t bmac[6];

    esp_wifi_get_mac(ESP_IF_WIFI_STA, bmac);
    sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", bmac[0], bmac[1], bmac[2], bmac[3], bmac[4], bmac[5]);

    return mac;
}

static void reportIP (bool newopt)
{
    on_report_options(newopt);

    if(newopt) {
#if FTP_ENABLE
        hal.stream.write(",WIFI,FTP");
#else
        hal.stream.write(",WIFI");
#endif
    } else {
        hal.stream.write("[WIFI MAC:");
        hal.stream.write(wifi_get_mac());
        hal.stream.write("]" ASCII_EOL);

        hal.stream.write("[IP:");
        hal.stream.write(wifi_get_ipaddr());
        hal.stream.write("]" ASCII_EOL);

        if(active_stream == StreamType_Telnet || active_stream == StreamType_WebSocket) {
            hal.stream.write("[NETCON:");
            hal.stream.write(active_stream == StreamType_Telnet ? "Telnet" : "Websocket");
            hal.stream.write("]" ASCII_EOL);
        }
    }
}

bool wifi_dns_running (void)
{
    return services.dns == On;
}

static void lwIPHostTimerHandler (void *arg)
{
    if(services.mask)
        sys_timeout(STREAM_POLL_INTERVAL, lwIPHostTimerHandler, NULL);

#if TELNET_ENABLE
    if(services.telnet)
        telnetd_poll();
#endif
#if WEBSOCKET_ENABLE
    if(services.websocket)
        websocketd_poll();
#endif
#if FTP_ENABLE
    if(services.ftp)
        ftpd_poll();
#endif
}

static void start_services (void)
{
#if TELNET_ENABLE
    if(network.services.telnet && !services.telnet)
        services.telnet = telnetd_init(network.telnet_port == 0 ? NETWORK_TELNET_PORT : network.telnet_port);
#endif
#if WEBSOCKET_ENABLE
    if(network.services.websocket && !services.websocket)
        services.websocket = websocketd_init(network.websocket_port == 0 ? NETWORK_WEBSOCKET_PORT : network.websocket_port);
#endif
#if FTP_ENABLE
    if(network.services.ftp && !services.ftp)
        services.ftp = ftpd_init(network.ftp_port == 0 ? NETWORK_FTP_PORT : network.ftp_port);
#endif
#if HTTP_ENABLE
    if(network.services.http && !services.http)
        services.http = httpdaemon_start(&network);
#endif
#if TELNET_ENABLE || WEBSOCKET_ENABLE || FTP_ENABLE
    sys_timeout(STREAM_POLL_INTERVAL, lwIPHostTimerHandler, NULL);
#endif
}

static void stop_services (void)
{
    network_services_t running;

    running.mask = services.mask;
    services.mask = 0;

#if HTTP_ENABLE
    if(running.http)
        httpdaemon_stop();
#endif
#if TELNET_ENABLE
    if(running.telnet)
        telnetd_stop();
#endif
#if WEBSOCKET_ENABLE
    if(running.websocket)
        websocketd_stop();
#endif
    if(running.dns)
        dns_server_stop();

    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT|SCANNING_BIT|APSTA_BIT);
}

static void wifi_ap_scan (void)
{
    // https://esp32.com/viewtopic.php?t=5536
    // https://esp32.com/viewtopic.php?t=7305

    static const wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .scan_time.active = {
          .min = 500,
          .max = 1500
        },
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = false
    };

    if(!(xEventGroupGetBits(wifi_event_group) & SCANNING_BIT) && esp_wifi_scan_start(&scan_config, false) == ESP_OK)
        xEventGroupSetBits(wifi_event_group, SCANNING_BIT);
}

static void msg_ap_ready (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI AP READY]" ASCII_EOL);
}

static void msg_ap_connected (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI AP CONNECTED]" ASCII_EOL);
}

static void msg_ap_scan_completed (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI AP SCAN COMPLETED]" ASCII_EOL);
}

static void msg_ap_disconnected (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI AP DISCONNECTED]" ASCII_EOL);
}

static void msg_sta_active (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI STA ACTIVE]" ASCII_EOL);
}

static void msg_sta_disconnected (sys_state_t state)
{
    hal.stream.write_all("[MSG:WIFI STA DISCONNECTED]" ASCII_EOL);
}

static void ip_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch(event_id) {

        case IP_EVENT_STA_GOT_IP:
            // handle IP change (ip_change)
            protocol_enqueue_rt_command(msg_sta_active);
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            ap_list.ap_selected = wifi_sta_config.sta.ssid;
            memcpy(&ap_list.ip_addr, &((ip_event_got_ip_t *)event_data)->ip_info.ip, sizeof(ip4_addr_t));
            strcpy(ap_list.ap_status, "Connected");
            start_services();
            if(services.dns) {
                services.dns = Off;
                dns_server_stop();
            }
            if(xEventGroupGetBits(wifi_event_group) & APSTA_BIT) {
                strcpy(wifi.sta.ssid, (char *)wifi_sta_config.sta.ssid);
                strcpy(wifi.sta.password, (char *)wifi_sta_config.sta.password);
                // commit to EEPROM
            }
            break;

        default:
            break;
    }
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch(event_id) {

        case WIFI_EVENT_AP_START:
            protocol_enqueue_rt_command(msg_ap_ready);
            start_services();
            if(xEventGroupGetBits(wifi_event_group) & APSTA_BIT) {
                dns_server_start(sta_netif);
                services.dns = On;
            }
            break;
/*??
        case WIFI_EVENT_AP_STOP:
            protocol_enqueue_rt_command(msg_ap_disconnected);
            wifi_stop();
            break;
*/
        case WIFI_EVENT_AP_STACONNECTED:
            protocol_enqueue_rt_command(msg_ap_connected);
            if((xEventGroupGetBits(wifi_event_group) & APSTA_BIT) && !(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)) {
                /* // screws up dns?
                if(!(xEventGroupGetBits(wifi_event_group) & SCANNING_BIT)) {
                  //  ap_list.ap_selected = NULL;
                  //  wifi_ap_scan();
                }
                */
                if(!services.dns) {
                    dns_server_start(sta_netif);
                    services.dns = On;
                }

            }
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            telnetd_close_connections();
            websocketd_close_connections();
            protocol_enqueue_rt_command(msg_ap_disconnected);
            break;
                
        case WIFI_EVENT_STA_START:
            if(*wifi_sta_config.sta.ssid != '\0')
                esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            //stop_services();
            telnetd_close_connections();
            websocketd_close_connections();
            protocol_enqueue_rt_command(msg_sta_disconnected);
            memset(&wifi_sta_config, 0, sizeof(wifi_config_t));
            esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config);
            if((xEventGroupGetBits(wifi_event_group) & APSTA_BIT) && !(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)) {
                if(!services.dns) {
                    dns_server_start(sta_netif);
                    services.dns = On;
                }
//                ap_list.ap_selected = NULL;
//                wifi_ap_scan();
            }
/*
            switch(((wifi_event_ap_stadisconnected_t *)event_data)->disconnected.reason) {

                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                // fall through
                case WIFI_REASON_NO_AP_FOUND:
                    strcpy(ap_list.ap_status, "Connection failed");
                // fall through
                case WIFI_REASON_ASSOC_LEAVE:
                    memset(&wifi_sta_config, 0, sizeof(wifi_config_t));
                    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config);
                    ap_list.ap_selected = NULL;
                    if(true)
                        wifi_ap_scan();
                    break;

                default:
                    if(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)
                        esp_wifi_connect(); // This is a workaround as ESP32 WiFi libs don't currently auto-reassociate.
                    break;
            }
*/
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            //TCPStreamNotifyLinkStatus(false);
            break;

        case WIFI_EVENT_SCAN_DONE:

            xEventGroupClearBits(wifi_event_group, SCANNING_BIT);

            if(xSemaphoreTake(aplist_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {

                if(ap_list.ap_records)
                    free(ap_list.ap_records);

                ap_list.ap_num = 0;

                esp_wifi_scan_get_ap_num(&ap_list.ap_num);

                if((ap_list.ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_list.ap_num)) != NULL)
                    esp_wifi_scan_get_ap_records(&ap_list.ap_num, ap_list.ap_records);

                protocol_enqueue_rt_command(msg_ap_scan_completed);

                xSemaphoreGive(aplist_mutex);
            }
            // Start a new scan in 10 secs if no station connected...
//          if(!(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT))
//              wifi_ap_scan();
            break;

        default:
            break;
    } 
}

static inline void set_addr (char *ip, ip4_addr_t *addr)
{
    memcpy(ip, addr, sizeof(ip4_addr_t));
}

static inline void get_addr (esp_ip4_addr_t *addr, char *ip)
{
    memcpy(addr, ip, sizeof(esp_ip4_addr_t));
}

static bool init_adapter (esp_netif_t *netif, network_settings_t *settings)
{
    memcpy(&network, settings, sizeof(network_settings_t));

    esp_netif_ip_info_t ipInfo;

    if(network.ip_mode == IpMode_Static) {
        get_addr(&ipInfo.ip, network.ip);
        get_addr(&ipInfo.gw, network.gateway);
        get_addr(&ipInfo.netmask, network.mask);
        esp_netif_set_ip_info(netif, &ipInfo);
    }

    return network.ip_mode == IpMode_DHCP;
}

static wifi_mode_t settingToMode(grbl_wifi_mode_t mode)
{
    return mode == WiFiMode_AP ? WIFI_MODE_AP :
           mode == WiFiMode_STA ? WIFI_MODE_STA :
           mode == WiFiMode_APSTA ? WIFI_MODE_APSTA :
           WIFI_MODE_NULL;
}

bool wifi_start (void)
{
    wifi_mode_t currentMode;

    if(nvs_address == 0)
        return false;

#if !WIFI_SOFTAP
    if(wifi.mode == WiFiMode_APSTA) // Reset to default
        wifi.mode = WIFI_MODE;
#endif

    if(esp_wifi_get_mode(&currentMode) == ESP_ERR_WIFI_NOT_INIT) {

        esp_netif_init();
        esp_event_loop_create_default();

        wifi_event_group = xEventGroupCreate();
        aplist_mutex = xSemaphoreCreateMutex();

        if(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL) != ESP_OK)
            return false;

        if(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL) != ESP_OK)
            return false;

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        if(esp_wifi_init(&cfg) != ESP_OK)
            return false;

        if(esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK)
            return false;

        if(esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK)
            return false;

        currentMode = WIFI_MODE_NULL;
    }

    if(currentMode != settingToMode(wifi.mode) && (wifi.mode == WiFiMode_AP || wifi.mode == WiFiMode_APSTA)) {

        wifi_config_t wifi_config;

        if(ap_netif == NULL)
            ap_netif = esp_netif_create_default_wifi_ap();

        esp_netif_dhcps_stop(ap_netif);

        wifi.ap.network.ip_mode = IpMode_Static; // Only mode supported

        init_adapter(ap_netif, &wifi.ap.network);

        esp_netif_dhcps_start(ap_netif);

        memset(&wifi_config, 0, sizeof(wifi_config_t));

        if(*wifi.ap.ssid == '\0')
            return false;

        if(strlcpy((char *)wifi_config.ap.ssid, wifi.ap.ssid, sizeof(wifi_config.ap.ssid)) >= sizeof(wifi_config.ap.ssid))
            return false;

        if (*wifi.ap.password == '\0')
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        else if(strlcpy((char *)wifi_config.ap.password, wifi.ap.password, sizeof(wifi_config.ap.password)) < sizeof(wifi_config.ap.password))
            wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        else
            return false;

        wifi_config.ap.max_connection = 1;

        if(esp_wifi_set_mode(settingToMode(wifi.mode)) != ESP_OK)
            return false;

        if(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config) != ESP_OK)
            return false;

        if(wifi.mode == WiFiMode_APSTA)
            xEventGroupSetBits(wifi_event_group, APSTA_BIT);
    }

    if(currentMode != settingToMode(wifi.mode) && (wifi.mode == WiFiMode_STA || wifi.mode == WiFiMode_APSTA)) {
        
        if(sta_netif == NULL)
            sta_netif = esp_netif_create_default_wifi_sta();

        esp_netif_dhcps_stop(sta_netif);
    
        wifi.sta.network.ip_mode = IpMode_DHCP; // For now...

        if(init_adapter(sta_netif, &wifi.sta.network))
            esp_netif_dhcps_start(sta_netif);

        memset(&wifi_sta_config, 0, sizeof(wifi_config_t));

        if(*wifi.sta.ssid != '\0') {

            if(strlcpy((char *)wifi_sta_config.sta.ssid, wifi.sta.ssid, sizeof(wifi_sta_config.sta.ssid)) >= sizeof(wifi_sta_config.sta.ssid))
                return false;

            if(strlcpy((char *)wifi_sta_config.sta.password, wifi.sta.password, sizeof(wifi_sta_config.sta.password)) >= sizeof(wifi_sta_config.sta.password))
                return false;
        }

        if(esp_wifi_set_mode(settingToMode(wifi.mode)) != ESP_OK)
            return false;

        if(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config) != ESP_OK)
            return false;
    }

    if(esp_wifi_start() != ESP_OK)
        return false;

    if(wifi.mode == WiFiMode_AP || wifi.mode == WiFiMode_APSTA)
        esp_netif_set_hostname(ap_netif, wifi.ap.network.hostname);

    if(wifi.mode == WiFiMode_STA || wifi.mode == WiFiMode_APSTA)
        esp_netif_set_hostname(sta_netif, wifi.sta.network.hostname);

    if(wifi.mode == WiFiMode_APSTA)
        wifi_ap_scan();

    return true;
}

bool wifi_ap_connect (char *ssid, char *password)
{
    bool ok = !ssid || (strlen(ssid) > 0 && strlen(ssid) < sizeof(ssid_t) && strlen(password) < sizeof(password_t));

    if(!ok)
        return false;

    if(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)
        esp_wifi_disconnect(); // TODO: delay until response is sent...

    if(xSemaphoreTake(aplist_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {

        ap_list.ap_selected = NULL;
        memset(&ap_list.ip_addr, 0, sizeof(ip4_addr_t));
        strcpy(ap_list.ap_status, ssid ? "Connecting..." : "");

        xSemaphoreGive(aplist_mutex);
    }

    memset(&wifi_sta_config, 0, sizeof(wifi_config_t));

    if(ssid) {

        strcpy((char *)wifi_sta_config.sta.ssid, ssid);
        strcpy((char *)wifi_sta_config.sta.password, password);

        ok = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config) == ESP_OK && esp_wifi_connect() == ESP_OK;
    }

    return ok;
}

bool wifi_stop (void)
{
    stop_services();

    esp_wifi_stop();

    return true;
}

wifi_settings_t *get_wifi_settings (void)
{
    return &wifi;
}

network_settings_t *get_network_settings (void)
{
    return &network;   
}

static status_code_t wifi_set_int (setting_id_t setting, uint_fast16_t value);
static uint_fast16_t wifi_get_int (setting_id_t setting);
static status_code_t wifi_set_ip (setting_id_t setting, char *value);
static char *wifi_get_ip (setting_id_t setting);
static void wifi_settings_restore (void);
static void wifi_settings_load (void);

static const setting_group_detail_t ethernet_groups [] = {
    { Group_Root, Group_Networking, "Networking" },
    { Group_Networking, Group_Networking_Wifi, "WiFi" }
};

static const setting_detail_t ethernet_settings[] = {
    { Setting_NetworkServices, Group_Networking, "Network Services", NULL, Format_Bitfield, netservices, NULL, NULL, Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL },
#if WEBUI_AUTH_ENABLE
    { Setting_AdminPassword, Group_General, "Admin Password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &wifi.admin_password, NULL, NULL },
    { Setting_UserPassword, Group_General, "User Password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &wifi.user_password, NULL, NULL },
#endif
    { Setting_WiFi_STA_SSID, Group_Networking_Wifi, "WiFi Station (STA) SSID", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, &wifi.sta.ssid, NULL, NULL },
    { Setting_WiFi_STA_Password, Group_Networking_Wifi, "WiFi Station (STA) Password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &wifi.sta.password, NULL, NULL },
    { Setting_Hostname, Group_Networking, "Hostname", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, &wifi.sta.network.hostname, NULL, NULL },
/*    { Setting_IpMode, Group_Networking, "IP Mode", NULL, Format_RadioButtons, "Static,DHCP,AutoIP", NULL, NULL, Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL }, */
    { Setting_IpAddress, Group_Networking, "IP Address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
    { Setting_Gateway, Group_Networking, "Gateway", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
    { Setting_NetMask, Group_Networking, "Netmask", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
#if WIFI_SOFTAP
    { Setting_WifiMode, Group_Networking_Wifi, "WiFi Mode", NULL, Format_RadioButtons, "Off,Station,Access Point,Access Point/Station", NULL, NULL, Setting_NonCore, &wifi.mode, NULL, NULL },
    { Setting_WiFi_AP_SSID, Group_Networking_Wifi, "WiFi Access Point (AP) SSID", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, &wifi.ap.ssid, NULL, NULL },
    { Setting_WiFi_AP_Password, Group_Networking_Wifi, "WiFi Access Point (AP) Password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &wifi.ap.password, NULL, NULL },
    { Setting_Hostname2, Group_Networking, "Hostname (AP)", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, &wifi.ap.network.hostname, NULL, NULL },
    { Setting_IpAddress2, Group_Networking, "IP Address (AP)", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
    { Setting_Gateway2, Group_Networking, "Gateway (AP)", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
    { Setting_NetMask2, Group_Networking, "Netmask (AP)", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, wifi_set_ip, wifi_get_ip, NULL },
#else
    { Setting_WifiMode, Group_Networking_Wifi, "WiFi Mode", NULL, Format_RadioButtons, "Off,Station", NULL, NULL, Setting_NonCore, &wifi.mode, NULL, NULL },
#endif
#if TELNET_ENABLE
    { Setting_TelnetPort, Group_Networking, "Telnet port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL },
#endif
#if HTTP_ENABLE
    { Setting_HttpPort, Group_Networking, "HTTP port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL },
#endif
#if FTP_ENABLE
    { Setting_FtpPort, Group_Networking, "FTP port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL },
#endif
#if WEBSOCKET_ENABLE
    { Setting_WebSocketPort, Group_Networking, "Websocket port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, wifi_set_int, wifi_get_int, NULL }
#endif
};

#ifndef NO_SETTINGS_DESCRIPTIONS

static const setting_descr_t ethernet_settings_descr[] = {
    { Setting_NetworkServices, "Network services to enable. Consult driver documentation for availability." SETTINGS_HARD_RESET_REQUIRED },
#if WEBUI_AUTH_ENABLE
    { Setting_AdminPassword, "Administrator password." },
    { Setting_UserPassword, "User password." },
#endif
    { Setting_WiFi_STA_SSID, "WiFi Station (STA) SSID." },
    { Setting_WiFi_STA_Password, "WiFi Station (STA) Password." },
    { Setting_Hostname, "Network hostname." SETTINGS_HARD_RESET_REQUIRED },
//    { Setting_IpMode, "IP Mode." SETTINGS_HARD_RESET_REQUIRED },
    { Setting_IpAddress, "Static IP address." SETTINGS_HARD_RESET_REQUIRED },
    { Setting_Gateway, "Static gateway address."SETTINGS_HARD_RESET_REQUIRED },
    { Setting_NetMask, "Static netmask." SETTINGS_HARD_RESET_REQUIRED },
#if WIFI_SOFTAP
    { Setting_WifiMode, "WiFi Mode." },
    { Setting_WiFi_AP_SSID, "WiFi Access Point (AP) SSID." },
    { Setting_WiFi_AP_Password, "WiFi Access Point (AP) Password." },
    { Setting_Hostname2, "Network hostname." SETTINGS_HARD_RESET_REQUIRED },
    { Setting_IpAddress2, "Static IP address."SETTINGS_HARD_RESET_REQUIRED },
    { Setting_Gateway2, "Static gateway address." SETTINGS_HARD_RESET_REQUIRED },
    { Setting_NetMask2, "Static netmask." SETTINGS_HARD_RESET_REQUIRED },
#else
    { Setting_WifiMode, "WiFi Mode." },
#endif
#if TELNET_ENABLE
    { Setting_TelnetPort, "(Raw) Telnet port number listening for incoming connections." SETTINGS_HARD_RESET_REQUIRED },
#endif
#if FTP_ENABLE
    { Setting_FtpPort, "FTP port number listening for incoming connections." SETTINGS_HARD_RESET_REQUIRED },
#endif
#if HTTP_ENABLE
    { Setting_HttpPort, "HTTP port number listening for incoming connections." SETTINGS_HARD_RESET_REQUIRED },
#endif
#if WEBSOCKET_ENABLE
    { Setting_WebSocketPort, "Websocket port number listening for incoming connections." SETTINGS_HARD_RESET_REQUIRED
                             "NOTE: WebUI requires this to be HTTP port number + 1."
    }
#endif
};

#endif

static void wifi_settings_save (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&wifi, sizeof(wifi_settings_t), true);
}

static setting_details_t setting_details = {
    .groups = ethernet_groups,
    .n_groups = sizeof(ethernet_groups) / sizeof(setting_group_detail_t),
    .settings = ethernet_settings,
    .n_settings = sizeof(ethernet_settings) / sizeof(setting_detail_t),
#ifndef NO_SETTINGS_DESCRIPTIONS
    .descriptions = ethernet_settings_descr,
    .n_descriptions = sizeof(ethernet_settings_descr) / sizeof(setting_descr_t),
#endif
    .save = wifi_settings_save,
    .load = wifi_settings_load,
    .restore = wifi_settings_restore
};

static status_code_t wifi_set_int (setting_id_t setting, uint_fast16_t value)
{
    switch(setting) {

        case Setting_NetworkServices:
            wifi.sta.network.services.mask = wifi.ap.network.services.mask = (uint8_t)value & allowed_services.mask;
            break;

#if TELNET_ENABLE
        case Setting_TelnetPort:
            wifi.sta.network.telnet_port = wifi.ap.network.telnet_port = (uint16_t)value;
            break;
#endif

#if FTP_ENABLE
        case Setting_FtpPort:
            wifi.sta.network.ftp_port = wifi.ap.network.ftp_port = (uint16_t)value;
            break;
#endif

#if HTTP_ENABLE
        case Setting_HttpPort:
            wifi.sta.network.http_port = wifi.ap.network.http_port = (uint16_t)value;
            break;
#endif

#if WEBSOCKET_ENABLE
        case Setting_WebSocketPort:
            wifi.sta.network.websocket_port = wifi.ap.network.websocket_port = (uint16_t)value;
            break;
#endif
        default:
            break;
    }

    return Status_OK;
}

static uint_fast16_t wifi_get_int (setting_id_t setting)
{
    uint_fast16_t value = 0;

    switch(setting) {

        case Setting_NetworkServices:
            value = wifi.sta.network.services.mask & allowed_services.mask;
            break;

#if TELNET_ENABLE
        case Setting_TelnetPort:
            value = wifi.sta.network.telnet_port;
            break;
#endif

#if FTP_ENABLE
        case Setting_FtpPort:
            value = wifi.sta.network.ftp_port;
            break;
#endif

#if HTTP_ENABLE
        case Setting_HttpPort:
            value = wifi.sta.network.http_port;
            break;
#endif

#if WEBSOCKET_ENABLE
        case Setting_WebSocketPort:
            value = wifi.sta.network.websocket_port;
            break;
#endif
        default:
            break;
    }

    return value;
}

static status_code_t wifi_set_ip (setting_id_t setting, char *value)
{
    ip4_addr_t addr;

    if(inet_pton(AF_INET, value, &addr) != 1)
        return Status_InvalidStatement;

    status_code_t status = Status_OK;

    switch(setting) {

        case Setting_IpAddress:
            set_addr(wifi.sta.network.ip, &addr);
            break;

        case Setting_Gateway:
            set_addr(wifi.sta.network.gateway, &addr);
            break;

        case Setting_NetMask:
            set_addr(wifi.sta.network.mask, &addr);
            break;

#if WIFI_SOFTAP

        case Setting_IpAddress2:
            set_addr(wifi.ap.network.ip, &addr);
            break;

        case Setting_Gateway2:
            set_addr(wifi.ap.network.gateway, &addr);
            break;

        case Setting_NetMask2:
            set_addr(wifi.ap.network.mask, &addr);
            break;

#endif

        default:
            status = Status_Unhandled;
            break;
    }

    return status;
}

static char *wifi_get_ip (setting_id_t setting)
{
    static char ip[INET6_ADDRSTRLEN];

    switch(setting) {

        case Setting_IpAddress:
            inet_ntop(AF_INET, &wifi.sta.network.ip, ip, INET6_ADDRSTRLEN);
            break;

        case Setting_Gateway:
            inet_ntop(AF_INET, &wifi.sta.network.gateway, ip, INET6_ADDRSTRLEN);
            break;

        case Setting_NetMask:
            inet_ntop(AF_INET, &wifi.sta.network.mask, ip, INET6_ADDRSTRLEN);
            break;

#if WIFI_SOFTAP

        case Setting_IpAddress2:
            inet_ntop(AF_INET, &wifi.ap.network.ip, ip, INET6_ADDRSTRLEN);
            break;

        case Setting_Gateway2:
            inet_ntop(AF_INET, &wifi.ap.network.gateway, ip, INET6_ADDRSTRLEN);
            break;

        case Setting_NetMask2:
            inet_ntop(AF_INET, &wifi.ap.network.mask, ip, INET6_ADDRSTRLEN);
            break;

#endif

        default:
            *ip = '\0';
            break;
    }

    return ip;
}

static void wifi_settings_restore (void)
{
    ip4_addr_t addr;

    memset(&wifi, 0, sizeof(wifi_settings_t));

    wifi.mode = WIFI_MODE;

// Station

    strlcpy(wifi.sta.network.hostname, NETWORK_HOSTNAME, sizeof(wifi.sta.network.hostname));

    wifi.sta.network.ip_mode = (ip_mode_t)NETWORK_IPMODE;

    if(inet_pton(AF_INET, NETWORK_IP, &addr) == 1)
        set_addr(wifi.sta.network.ip, &addr);

    if(inet_pton(AF_INET, NETWORK_GATEWAY, &addr) == 1)
        set_addr(wifi.sta.network.gateway, &addr);

#if NETWORK_IPMODE == 0
    if(inet_pton(AF_INET, NETWORK_MASK, &addr) == 1)
        set_addr(wifi.sta.network.mask, &addr);
 #else
    if(inet_pton(AF_INET, "255.255.255.0", &addr) == 1)
        set_addr(wifi.sta.network.mask, &addr);
#endif

// Access Point

#if WIFI_SOFTAP

    wifi.ap.network.ip_mode = IpMode_Static;
    strlcpy(wifi.ap.network.hostname, NETWORK_AP_HOSTNAME, sizeof(wifi.ap.network.hostname));
    strlcpy(wifi.ap.ssid, WIFI_AP_SSID, sizeof(wifi.ap.ssid));
    strlcpy(wifi.ap.password, WIFI_AP_PASSWORD, sizeof(wifi.ap.password));

    if(inet_pton(AF_INET, NETWORK_AP_IP, &addr) == 1)
        set_addr(wifi.ap.network.ip, &addr);

    if(inet_pton(AF_INET, NETWORK_AP_GATEWAY, &addr) == 1)
        set_addr(wifi.ap.network.gateway, &addr);

    if(inet_pton(AF_INET, NETWORK_AP_MASK, &addr) == 1)
        set_addr(wifi.ap.network.mask, &addr);

#endif

// Common

    wifi.sta.network.telnet_port = wifi.ap.network.telnet_port = NETWORK_TELNET_PORT;
    wifi.sta.network.ftp_port = wifi.ap.network.ftp_port = NETWORK_FTP_PORT;
    wifi.sta.network.http_port = wifi.ap.network.http_port = NETWORK_HTTP_PORT;
    wifi.sta.network.websocket_port = wifi.ap.network.websocket_port = NETWORK_WEBSOCKET_PORT;
    wifi.sta.network.services = wifi.ap.network.services = allowed_services;

    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&wifi, sizeof(wifi_settings_t), true);
}

static void wifi_settings_load (void)
{
    if(hal.nvs.memcpy_from_nvs((uint8_t *)&wifi, nvs_address, sizeof(wifi_settings_t), true) != NVS_TransferResult_OK)
        wifi_settings_restore();

    wifi.sta.network.services.mask &= allowed_services.mask;
    wifi.ap.network.services.mask &= allowed_services.mask;
}

static void stream_changed (stream_type_t type)
{
    if(type != StreamType_SDCard)
        active_stream = type;

    if(on_stream_changed)
        on_stream_changed(type);
}

bool wifi_init (void)
{
    if((nvs_address = nvs_alloc(sizeof(wifi_settings_t)))) {

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = reportIP;

        on_stream_changed = grbl.on_stream_changed;
        grbl.on_stream_changed = stream_changed;

        settings_register(&setting_details);

        allowed_services.mask = networking_get_services_list((char *)netservices).mask;
    }

    return nvs_address != 0;
}

#endif // WIFI_ENABLE

