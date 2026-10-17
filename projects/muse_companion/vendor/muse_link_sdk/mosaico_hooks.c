// SPDX-License-Identifier: Apache-2.0
/* This adapter is the single owner of SDK/GSP integration. It never starts
 * another display, button GPIO owner, or OTA writer. Atomics cross SDK tasks. */
#include <stdatomic.h>
#include "mosaico_hooks.h"
#include "app.h"
#include "config_store.h"
#include "muse_glue.h"
#include "muse_link.h"
#include "muse_state.h"
#include "noise_control.h"
#include "ota.h"
#include "wifi_mgr.h"
#include "esp_app_desc.h"
static atomic_bool initialized;
static atomic_bool ready;
static atomic_int link_state;
static muse_companion_text_cb text_cb;
static void *text_ctx;
void muse_companion_text_observer(muse_companion_text_cb cb,void *ctx)
{text_cb=cb;text_ctx=ctx;}
void muse_companion_text_observe(const char *event,const char *id,const char *text,const char *raw,size_t n)
{if(text_cb)text_cb(text_ctx,event,id,text,raw,n);}
void muse_glue_start(void) {}
void muse_glue_storage_ready(void) { atomic_store(&initialized, true); }
void muse_glue_link_ready(void) { atomic_store(&ready, true); }
void muse_glue_led_state(led_state_t s) { atomic_store(&link_state, s); }
bool muse_port_initialized(void) { return atomic_load(&initialized); }
bool muse_port_link_ready(void) { return atomic_load(&ready); }
led_state_t muse_port_state(void) { return atomic_load(&link_state); }
bool led_status_init(void) { return true; }
void led_status_set_state(led_state_t s) { muse_glue_led_state(s); }
void led_status_set_title(const char *s) { (void)s; }
bool led_status_display_info(int *w, int *h) { (void)w; (void)h; return false; }
int led_status_display_bits(void) { return 0; }
bool led_status_draw_rect(int x,int y,int w,int h,const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; return false; }
void led_status_draw_done(void) {}
void led_status_show_animation(void) {}
bool muse_wifi_connected(void) { return wifi_mgr_is_connected(); }
bool muse_link_hatch_linked(void)
{ return muse_port_initialized() && config_is_provisioned(); }
bool muse_link_req_ready(void) { return noise_ctrl_is_connected(); }
int64_t muse_link_req_open(const char *verb,const char *path,
    const char *const *headers,bool end,muse_link_req_cb cb,void *ctx)
{ return noise_ctrl_req_open(verb,path,headers,end,cb,ctx); }
bool muse_link_req_send(int64_t id,const void *data,size_t len,bool end,int wait)
{ return noise_ctrl_req_send(id,data,len,end,wait); }
void muse_link_req_cancel(int64_t id) { noise_ctrl_req_cancel(id); }
void muse_state_page(int *cols,int *lines) { *cols=20; *lines=3; }
muse_power_t muse_state_power(void)
{ return (muse_power_t){.battery_pct=-1}; }
bool ota_is_enabled(void) { return false; }
void ota_start(const char *url,bool force,ota_status_cb cb,void *ctx)
{
    (void)url; (void)force;
    const esp_app_desc_t *app=esp_app_get_description();
    ota_event_t ev={.result=OTA_RESULT_SKIPPED,
        .detail="Firmware updates belong to retained Mosaico Recovery",
        .running_version=app ? app->version : "unknown"};
    if (cb) cb(&ev,ctx);
}
