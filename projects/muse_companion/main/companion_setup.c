// SPDX-License-Identifier: Apache-2.0
#include "companion_setup.h"
#include "companion_credentials.h"
#include "octop_link.h"
#include "octop_wifi.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

enum { REQUEST_CAP=4096, SETUP_STACK=6144, RESTART_STACK=3072, NETWORK_LIST=16 };
/* The portal borrows the octop_link radio in AP+STA mode: it must be able to
 * scan and to test-join the network the user picked before anything is saved.
 * Boot owns this service exclusively; the runtime starts only after a reboot. */
struct setup_t {
    httpd_handle_t server;
    bool saving;
    char ssid[33], nonce[33], body[REQUEST_CAP+1];
};
static struct setup_t *setup;
static atomic_bool reconfiguration_requested;
static atomic_bool reconfiguration_failed;
extern const char html_start[] asm("_binary_setup_html_start");
extern const char html_end[] asm("_binary_setup_html_end");

static void headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Content-Security-Policy",
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'");
}
static esp_err_t response(httpd_req_t *req,const char *status,const char *body)
{
    headers(req);httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t home(httpd_req_t *req)
{
    headers(req);httpd_resp_set_type(req,"text/html; charset=utf-8");
    return httpd_resp_send(req,html_start,html_end-html_start-1);
}
static esp_err_t session(httpd_req_t *req)
{
    struct setup_t *s=req->user_ctx;char json[64];
    snprintf(json,sizeof(json),"{\"nonce\":\"%s\"}",s->nonce);
    return response(req,"200 OK",json);
}
static esp_err_t networks(httpd_req_t *req)
{
    octop_ap_record_t *aps=calloc(NETWORK_LIST,sizeof(*aps));
    if(!aps)return response(req,"503 Service Unavailable","{\"error\":\"memory\"}");
    size_t found=octop_wifi_scan(aps,NETWORK_LIST);
    cJSON *root=cJSON_CreateArray();
    for(size_t i=0;i<found;++i){
        cJSON *item=cJSON_CreateObject();
        if(!item)break;
        cJSON_AddStringToObject(item,"ssid",aps[i].ssid);
        cJSON_AddNumberToObject(item,"rssi",aps[i].rssi);
        cJSON_AddItemToArray(root,item);
    }
    free(aps);
    char *json=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!json)return response(req,"503 Service Unavailable","{\"error\":\"memory\"}");
    ESP_LOGI("companion_setup","scan found %u networks",(unsigned)found);
    esp_err_t err=response(req,"200 OK",json);
    free(json);
    return err;
}
static void restart(void *ctx)
{
    (void)ctx;uint32_t commit=0;
    xTaskNotifyWait(0,UINT32_MAX,&commit,portMAX_DELAY);
    if(commit){vTaskDelay(pdMS_TO_TICKS(2000));esp_restart();}
    vTaskDelete(NULL);
}
static esp_err_t setup_flag(bool enabled)
{
    nvs_handle_t h;esp_err_t err=nvs_open("companion_keys",NVS_READWRITE,&h);
    if(err==ESP_OK){err=nvs_set_u8(h,"setup",enabled?1:0);if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
    return err;
}
static bool setup_requested(void)
{
    uint8_t value=0;nvs_handle_t h;
    if(nvs_open("companion_keys",NVS_READONLY,&h)==ESP_OK){nvs_get_u8(h,"setup",&value);nvs_close(h);}
    return value==1;
}
static void reconfigure(void *ctx)
{
    (void)ctx;
    if(setup_flag(true)==ESP_OK){
        octop_link_config_changed();
        vTaskDelay(pdMS_TO_TICKS(500));esp_restart();}
    atomic_store(&reconfiguration_failed,true);
    atomic_store(&reconfiguration_requested,false);vTaskDelete(NULL);
}
esp_err_t companion_setup_request(void)
{
    if(atomic_exchange(&reconfiguration_requested,true))return ESP_OK;
    if(xTaskCreate(reconfigure,"setup_request",4096,NULL,3,NULL)!=pdPASS){
        atomic_store(&reconfiguration_requested,false);return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
bool companion_setup_request_failed(void)
{return atomic_exchange(&reconfiguration_failed,false);}
static char *text_field(cJSON *json,const char *name)
{
    cJSON *item=cJSON_GetObjectItemCaseSensitive(json,name);
    return cJSON_IsString(item)?item->valuestring:NULL;
}
static esp_err_t configure(httpd_req_t *req)
{
    struct setup_t *s=req->user_ctx;char nonce[40],type[48];
    if(s->saving)return response(req,"409 Conflict","{\"error\":\"restarting\"}");
    if(httpd_req_get_hdr_value_str(req,"X-Setup-Nonce",nonce,sizeof(nonce))!=ESP_OK||strcmp(nonce,s->nonce))
        return response(req,"403 Forbidden","{\"error\":\"session\"}");
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||
        (strcmp(type,"application/json")&&strcmp(type,"application/json; charset=utf-8"))||
        !req->content_len||req->content_len>REQUEST_CAP)
        return response(req,"400 Bad Request","{\"error\":\"format\"}");
    size_t received=0;
    while(received<req->content_len){
        int n=httpd_req_recv(req,s->body+received,req->content_len-received);
        if(n<=0){memset(s->body,0,sizeof(s->body));
            return response(req,n==HTTPD_SOCK_ERR_TIMEOUT?"408 Request Timeout":"400 Bad Request","{\"error\":\"receive\"}");}
        received+=(size_t)n;
    }
    s->body[received]=0;
    cJSON *json=cJSON_ParseWithLengthOpts(s->body,received+1,NULL,true);
    const char *ssid=text_field(json,"ssid"),*pass=text_field(json,"wifi_password");
    const companion_credentials_config_t config={
        json?text_field(json,"octop_url"):NULL,json?text_field(json,"username"):NULL,
        json?text_field(json,"password"):NULL,json?text_field(json,"stt_url"):NULL,
        json?text_field(json,"stt_key"):NULL};
    esp_err_t err=ESP_ERR_INVALID_ARG;TaskHandle_t task=NULL;
    bool connected=false;
    if(cJSON_IsObject(json)&&companion_credentials_valid(&config)&&ssid&&ssid[0]){
        /* Join first: a saved network the device cannot reach would leave the
         * companion mute after reboot, and this is the only moment a human is
         * reading the error. */
        connected=octop_wifi_portal_join(ssid,pass?pass:"");
        if(connected){
            /* The reboot worker is allocated before anything is committed. */
            if(xTaskCreate(restart,"setup_restart",RESTART_STACK,NULL,3,&task)==pdPASS){
                err=companion_credentials_save(&config);
                if(err==ESP_OK)err=octop_wifi_save(ssid,pass?pass:"");
                if(err==ESP_OK)err=setup_flag(false);
            }else err=ESP_ERR_NO_MEM;
        }else err=ESP_ERR_INVALID_STATE;
    }
    cJSON_Delete(json);memset(s->body,0,sizeof(s->body));
    if(err==ESP_OK)s->saving=true;
    esp_err_t sent=response(req,err==ESP_OK?"200 OK":err==ESP_ERR_INVALID_ARG?"400 Bad Request":
        err==ESP_ERR_INVALID_STATE?"400 Bad Request":"503 Service Unavailable",
        err==ESP_OK?"{\"saved\":true,\"restart\":true}":
        err==ESP_ERR_INVALID_STATE?"{\"error\":\"wifi\"}":"{\"error\":\"save\"}");
    if(task)xTaskNotify(task,err==ESP_OK?1:0,eSetValueWithOverwrite);
    return sent;
}
esp_err_t companion_setup_start(void)
{
    if(setup||(companion_credentials_ready()&&!setup_requested()))return ESP_OK;
    struct setup_t *s=calloc(1,sizeof(*s));if(!s)return ESP_ERR_NO_MEM;
    uint8_t random[16],mac[6];esp_fill_random(random,sizeof(random));
    esp_err_t err=esp_read_mac(mac,ESP_MAC_WIFI_STA);if(err!=ESP_OK)goto fail;
    snprintf(s->ssid,sizeof(s->ssid),"Mochi-Setup-%02X%02X",mac[4],mac[5]);
    for(unsigned i=0;i<16;++i)snprintf(s->nonce+2*i,3,"%02x",random[i]);
    err=octop_wifi_portal_start(s->ssid);if(err!=ESP_OK)goto fail;
    httpd_config_t http=HTTPD_DEFAULT_CONFIG();http.stack_size=SETUP_STACK;
    http.max_open_sockets=3;http.lru_purge_enable=true;http.recv_wait_timeout=5;http.send_wait_timeout=5;
    err=httpd_start(&s->server,&http);if(err!=ESP_OK)goto fail;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=home,.user_ctx=s},
        {.uri="/api/session",.method=HTTP_GET,.handler=session,.user_ctx=s},
        {.uri="/api/networks",.method=HTTP_GET,.handler=networks,.user_ctx=s},
        {.uri="/api/config",.method=HTTP_POST,.handler=configure,.user_ctx=s}};
    for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);++i){err=httpd_register_uri_handler(s->server,&routes[i]);if(err!=ESP_OK)goto fail;}
    setup=s;
    ESP_LOGI("companion_setup","setup portal ready: %s / http://192.168.4.1",s->ssid);
    return ESP_OK;
fail:
    if(s->server)httpd_stop(s->server);
    memset(s,0,sizeof(*s));free(s);return err;
}
bool companion_setup_active(const char **ssid,const char **password)
{
    if(!setup)return false;
    if(ssid)*ssid=setup->ssid;
    if(password)*password="";
    return true;
}
