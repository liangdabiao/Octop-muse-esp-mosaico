// SPDX-License-Identifier: Apache-2.0
#include "companion_runtime.h"
#include "companion_platform.h"
#include "companion_setup.h"
#include "companion_generation.h"
#include "companion_credentials.h"
#include "octop_link.h"
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
enum { AUDIO_RATE=16000, AUDIO_FRAMES=256, AUDIO_QUEUE_DEPTH=48,
    CONTROL_QUEUE_DEPTH=12,
    /* Each slot carries a whole answer, so depth is a direct PSRAM cost. The
     * render task drains every slot per tick and streamed answers publish at
     * most five times a second, so two slots already cover a full burst. */
    MESSAGE_QUEUE_DEPTH=2,
    WORKER_STACK=8192,
    /* The link clamps every event line to its own EVENT_CAP; this buffer has to
     * be at least that wide or a spoken question would be cut before the UI. */
    EVENT_TEXT_CAP=256,
    /* A streamed answer repaints five times a second: fast enough to read as
     * live, slow enough that the reader never sees a half-copied page. */
    REPLY_UI_PERIOD_US=200000,
    /* octop_link already aborts a turn after 45 s with not even a reasoning
     * frame, or 60 s mid-stream; this outer ceiling only guarantees the UI can
     * never stick while the server keeps talking. */
    WAIT_LIMIT_US=120000000LL };
/* The retired volume slider now drives microphone input gain: 0-100 % of the
 * codec's 0-30 dB range. It is a float, so it cannot live in the enum. */
static const float mic_gain_max_db = 30.0f;
bool companion_platform_setup(const char **ssid,const char **password)
{return companion_setup_active(ssid,password);}
bool companion_platform_setup_request(void)
{return companion_setup_request()==ESP_OK;}
bool companion_platform_setup_request_failed(void)
{return companion_setup_request_failed();}
typedef struct {unsigned effects;uint32_t generation;unsigned volume,brightness,language,sleep_seconds,page_seconds;} command_t;
typedef struct {uint32_t generation;int16_t pcm[AUDIO_FRAMES];} audio_t;
/* App-lifetime service. UI owns no hardware; octop_link owns the radio and the
 * socket; the capture task owns microphone reads; the voice worker alone calls
 * octop_turn_* and owns every blocking turn decision. Queue messages and
 * atomics are the only shared state. */
struct runtime_t {
    QueueHandle_t control,messages,audio;
    companion_generation_handle_t generator;
    esp_codec_dev_handle_t mic;
    button_handle_t buttons[BSP_BUTTON_NUM];
    atomic_uint generation;
    atomic_bool recording,key,overrun,wants_record;
    command_t command;
    audio_t captured;
    char event_text[128],reply[COMP_REPLY_CAP];
    uint32_t recorded_frames;
    unsigned volume,brightness,last_brightness,language,sleep_seconds,page_seconds;
    int64_t phase_started_us,reply_published_us;
    uint32_t ui_ms;
    bool active,waiting,sntp,imu_ready,battery_ready,accel_valid,generating;
    float x,y,z;
    int battery;
};
static struct runtime_t *runtime;
static void publish(struct runtime_t *r,companion_message_kind_t kind,
    uint32_t gen,const char *text,bool streaming)
{
    companion_message_t *msg=calloc(1,sizeof(*msg));
    if(!msg){ESP_LOGE("companion","no message memory");return;}
    msg->kind=kind;msg->generation=gen;msg->streaming=streaming;
    snprintf(msg->text,sizeof(msg->text),"%s",text?text:"");
    if(xQueueSend(r->messages,msg,pdMS_TO_TICKS(100))!=pdTRUE)
        ESP_LOGE("companion","UI message queue full");
    free(msg);
}
static void capture(void *ctx)
{
    struct runtime_t *r=ctx;audio_t *a=calloc(1,sizeof(*a));
    if(!a){atomic_store(&r->overrun,true);vTaskDeleteWithCaps(NULL);return;}
    for(;;){
        bool rec=atomic_load(&r->recording);uint32_t gen=atomic_load(&r->generation);
        int ret=esp_codec_dev_read(r->mic,a->pcm,sizeof(a->pcm));
        if(ret!=ESP_CODEC_DEV_OK){atomic_store(&r->overrun,true);vTaskDelay(pdMS_TO_TICKS(30));continue;}
        if(rec&&atomic_load(&r->recording)&&gen==atomic_load(&r->generation)){
            a->generation=gen;
            if(xQueueSend(r->audio,a,0)!=pdTRUE)atomic_store(&r->overrun,true);
        }
    }
}
static void persist(struct runtime_t *r)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("companion",NVS_READWRITE,&h);
    if(err!=ESP_OK)return;
    err=nvs_set_u8(h,"volume",r->volume);
    if(err==ESP_OK)err=nvs_set_u8(h,"brightness",r->brightness);
    if(err==ESP_OK)err=nvs_set_u8(h,"language",r->language);
    if(err==ESP_OK)err=nvs_set_u8(h,"sleep_sec",r->sleep_seconds);
    if(err==ESP_OK)err=nvs_set_u8(h,"page_sec",r->page_seconds);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err!=ESP_OK)ESP_LOGW("companion","preference save failed: %s",esp_err_to_name(err));
}
static void drain_audio(struct runtime_t *r)
{
    while(xQueueReceive(r->audio,&r->captured,0)==pdTRUE){
        if(r->active&&r->captured.generation==r->command.generation){
            octop_turn_audio(r->captured.pcm,AUDIO_FRAMES);
            r->recorded_frames+=AUDIO_FRAMES;
        }
    }
}
static void command(struct runtime_t *r)
{
    command_t *c=&r->command;
    /* The slider is microphone sensitivity now that the device never speaks. */
    if(c->volume!=r->volume){
        r->volume=c->volume;
        int err=esp_codec_dev_set_in_gain(r->mic,r->volume*mic_gain_max_db/100.0f);
        if(err!=ESP_CODEC_DEV_OK)ESP_LOGW("companion","mic gain failed: %d",err);
    }
    r->brightness=c->brightness;
    r->language=c->language;r->sleep_seconds=c->sleep_seconds;
    r->page_seconds=c->page_seconds;
    octop_language(c->language);
    if(c->effects&COMP_EFFECT_PREFS)persist(r);
    if(c->effects&COMP_EFFECT_CANCEL){
        if(r->generating){companion_generation_finish(r->generator,"interrupted by AI key");r->generating=false;}
        octop_turn_cancel();r->active=false;r->waiting=false;r->phase_started_us=esp_timer_get_time();
    }
    if(c->effects&COMP_EFFECT_BEGIN){
        r->recorded_frames=0;
        if(!octop_turn_ready()){
            atomic_store(&r->recording,false);
            publish(r,COMP_MSG_ERROR,c->generation,
                companion_credentials_ready()?"网络未连接，请稍后重试":"还没有配置服务，请先用手机完成配置",false);
        }else{octop_turn_begin();r->active=true;r->phase_started_us=esp_timer_get_time();
            atomic_store(&r->recording,atomic_load(&r->wants_record));}
    }
    if((c->effects&COMP_EFFECT_END)&&r->active){
        drain_audio(r);
        ESP_LOGI("companion","recording released: generation=%u frames=%u",(unsigned)c->generation,(unsigned)r->recorded_frames);
        octop_turn_end();r->active=false;r->waiting=true;r->phase_started_us=esp_timer_get_time();
    }
}
static void voice(void *ctx)
{
    struct runtime_t *r=ctx;
    for(;;){
        if(!r->sntp&&octop_wifi_connected()){
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();r->sntp=true;
        }
        command_t next;
        if(xQueueReceive(r->control,&next,pdMS_TO_TICKS(10))==pdTRUE
            && next.generation==atomic_load(&r->generation)){r->command=next;command(r);}
        if(r->generating&&companion_generation_cancelled(r->generator)){
            octop_turn_cancel();companion_generation_finish(r->generator,"cancelled");r->generating=false;
        }
        if(!r->generating&&!r->active&&!r->waiting){
            const char *prompt=companion_generation_take(r->generator);
            if(prompt){
                if(octop_text_begin(prompt)){
                    r->generating=true;r->phase_started_us=esp_timer_get_time();
                    ESP_LOGI("companion","Octop text turn started");
                }else companion_generation_finish(r->generator,"could not send request");
            }
        }
        drain_audio(r);
        if(atomic_exchange(&r->overrun,false)&&(r->active||r->waiting)){
            atomic_store(&r->recording,false);octop_turn_cancel();r->active=false;r->waiting=false;
            publish(r,COMP_MSG_ERROR,r->command.generation,"录音传输未跟上，请重试",false);
        }
        char text[EVENT_TEXT_CAP];
        octop_ev_t ev=octop_turn_event(text,sizeof(text));
        if(r->generating){
            if(companion_generation_finished(r->generator))r->generating=false;
            else if(esp_timer_get_time()-r->phase_started_us>300000000LL){
                octop_turn_cancel();companion_generation_finish(r->generator,"generation timed out");r->generating=false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));continue;
        }
        uint32_t gen=r->command.generation;
        switch(ev){
        case OCTOP_EV_HEARD:
            ESP_LOGI("companion","heard: %s",text);
            /* The transcript belongs on the card, not only in the log: it is how
             * the reader checks what the microphone actually caught. */
            r->reply_published_us=0;
            publish(r,COMP_MSG_HEARD,gen,text,false);
            break;
        case OCTOP_EV_REPLY:
            /* Tokens land several times a second and the answer lives in the
             * link's own buffer, so one copy per fifth of a second is enough to
             * read as live; DONE always delivers the complete text. */
            if(esp_timer_get_time()-r->reply_published_us<REPLY_UI_PERIOD_US)break;
            if(octop_reply_copy(r->reply,sizeof(r->reply))){
                r->reply_published_us=esp_timer_get_time();
                publish(r,COMP_MSG_REPLY,gen,r->reply,true);
            }
            break;
        case OCTOP_EV_DONE:
            r->waiting=false;
            if(octop_reply_copy(r->reply,sizeof(r->reply))){
                ESP_LOGI("companion","reply received: generation=%u bytes=%u",(unsigned)gen,(unsigned)strlen(r->reply));
                publish(r,COMP_MSG_REPLY,gen,r->reply,false);
            }else publish(r,COMP_MSG_ERROR,gen,"没有收到文字回答，请重试",false);
            break;
        case OCTOP_EV_ERROR:
            atomic_store(&r->recording,false);r->active=false;r->waiting=false;
            publish(r,COMP_MSG_ERROR,gen,text[0]?text:"Octop 对话失败，请检查连接后重试",false);
            break;
        default:break;
        }
        if(ev==OCTOP_EV_NONE&&(r->active||r->waiting)
            && esp_timer_get_time()-r->phase_started_us>WAIT_LIMIT_US){
            atomic_store(&r->recording,false);octop_turn_cancel();r->active=false;r->waiting=false;
            publish(r,COMP_MSG_ERROR,gen,"本次等待超时，请重新提问",false);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
static void key_changed(void *button,void *ctx)
{(void)ctx;atomic_store(&runtime->key,iot_button_get_event(button)==BUTTON_PRESS_DOWN);}
/* A start-up allocation that lands in the wrong heap used to abort app_main,
 * which the bootloader reads as a bad image and answers by dropping the device
 * back into Recovery. Naming the stage and the water marks here keeps the
 * failure legible from the host instead of silently rolling back. */
static void report_stage(const char *stage,esp_err_t err)
{
    ESP_LOGE("companion","runtime start failed at %s: %s | psram free %u (max %u), internal free %u (max %u)",
        stage,esp_err_to_name(err),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
esp_err_t companion_runtime_start(void)
{
    if(runtime)return ESP_ERR_INVALID_STATE;
    ESP_LOGI("companion","runtime start: psram free %u (max %u), internal free %u (max %u)",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    struct runtime_t *r=calloc(1,sizeof(*r));if(!r){report_stage("service memory",ESP_ERR_NO_MEM);return ESP_ERR_NO_MEM;}
    r->battery=-1;r->volume=55;r->brightness=70;r->last_brightness=101;
    r->control=xQueueCreate(CONTROL_QUEUE_DEPTH,sizeof(command_t));
    r->messages=xQueueCreate(MESSAGE_QUEUE_DEPTH,sizeof(companion_message_t));
    r->audio=xQueueCreate(AUDIO_QUEUE_DEPTH,sizeof(audio_t));
    if(!r->control||!r->messages||!r->audio){
        report_stage(!r->messages?"message queue":"queue memory",ESP_ERR_NO_MEM);
        goto nomem;}
    r->mic=bsp_audio_codec_microphone_init();
    if(!r->mic){report_stage("microphone",ESP_FAIL);goto fail;}
    esp_codec_dev_sample_info_t format={.sample_rate=AUDIO_RATE,.bits_per_sample=16,.channel=1};
    if(esp_codec_dev_open(r->mic,&format)!=ESP_CODEC_DEV_OK){report_stage("codec open",ESP_FAIL);goto fail;}
    esp_codec_dev_set_in_gain(r->mic,r->volume*mic_gain_max_db/100.0f);
    /* From this point failure paths must not free r: the capture task borrows
     * it and maintenance stays reachable. */
    runtime=r;
    ESP_RETURN_ON_ERROR(companion_generation_create(&r->generator),"companion","generation service");
    ESP_RETURN_ON_ERROR(octop_link_start(),"companion","octop link");
    int count=0;ESP_RETURN_ON_ERROR(bsp_iot_button_create(r->buttons,&count,BSP_BUTTON_NUM),"companion","buttons");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(r->buttons[BSP_BUTTON_AI],BUTTON_PRESS_DOWN,NULL,key_changed,NULL),"companion","AI press");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(r->buttons[BSP_BUTTON_AI],BUTTON_PRESS_UP,NULL,key_changed,NULL),"companion","AI release");
    r->battery_ready=bsp_battery_init()==ESP_OK;
    if(bsp_imu_init()==ESP_OK){const bsp_imu_config_t cfg=BSP_IMU_CONFIG_DEFAULT();r->imu_ready=bsp_imu_start(&cfg)==ESP_OK;}
    /* Continuous codec capture holds the 16 KiB DMA burst margin, so its stack
     * comes from PSRAM; the voice worker never touches flash-heavy paths but
     * keeps an internal stack because it runs NVS preference writes. */
    if(xTaskCreateWithCaps(capture,"companion_mic",4096,r,5,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){report_stage("capture task",ESP_ERR_NO_MEM);return ESP_ERR_NO_MEM;}
    if(xTaskCreate(voice,"companion_voice",WORKER_STACK,r,4,NULL)!=pdPASS){report_stage("voice task",ESP_ERR_NO_MEM);return ESP_ERR_NO_MEM;}
    return ESP_OK;
nomem:
    if(r->control)vQueueDelete(r->control);
    if(r->messages)vQueueDelete(r->messages);
    if(r->audio)vQueueDelete(r->audio);
    free(r);return ESP_ERR_NO_MEM;
fail:
    if(r->mic)esp_codec_dev_close(r->mic);
    if(r->control)vQueueDelete(r->control);
    if(r->messages)vQueueDelete(r->messages);
    if(r->audio)vQueueDelete(r->audio);
    free(r);return ESP_FAIL;
}
void companion_platform_tick(uint32_t ms){if(runtime)runtime->ui_ms+=ms;}
void companion_platform_status(bool *online,bool *paired,int *battery,bool *key,bool *motion)
{
    struct runtime_t *r=runtime;*online=false;*paired=false;*battery=-1;*key=false;*motion=false;
    if(!r)return;
    *key=atomic_load(&r->key);
    *online=octop_wifi_connected();*paired=octop_link_ready();
    if(r->battery_ready&&r->ui_ms%5000==0){
        bsp_battery_status_t *b=malloc(sizeof(*b));
        if(b){if(bsp_battery_read(b)==ESP_OK)r->battery=b->state_of_charge;free(b);}
    }
    *battery=r->battery;
    if(r->imu_ready){
        float x,y,z;
        if(bsp_imu_get_accel(&x,&y,&z)==ESP_OK){
            if(r->accel_valid&&fabsf(x-r->x)+fabsf(y-r->y)+fabsf(z-r->z)>0.25f)*motion=true;
            r->x=x;r->y=y;r->z=z;r->accel_valid=true;
        }
    }
}
unsigned companion_platform_link_state(void)
{
    octop_status_t s;octop_link_status(&s);
    return (unsigned)s.state;
}
void companion_platform_link_detail(char *out,size_t cap)
{
    if(!out||!cap)return;
    octop_status_t s;octop_link_status(&s);
    snprintf(out,cap,"%s",s.detail);
}
bool companion_platform_message(companion_message_t *out)
{return runtime&&xQueueReceive(runtime->messages,out,0)==pdTRUE;}
bool companion_platform_effect(unsigned effects,const companion_snapshot_t *s)
{
    if(!runtime)return false;
    ESP_LOGI("companion","interaction: effects=%u generation=%u",(unsigned)effects,(unsigned)s->generation);
    command_t c={.effects=effects,.generation=s->generation,.volume=s->volume,
        .brightness=s->brightness,.language=s->language,
        .sleep_seconds=s->sleep_seconds,.page_seconds=s->page_seconds};
    atomic_store(&runtime->generation,s->generation);
    if(effects&COMP_EFFECT_CANCEL){atomic_store(&runtime->recording,false);atomic_store(&runtime->wants_record,false);}
    if(effects&COMP_EFFECT_BEGIN)atomic_store(&runtime->wants_record,true);
    /* Release stops feeding frames; the worker still drains the queue once so
     * the tail of the clip reaches the server. */
    if(effects&COMP_EFFECT_END){atomic_store(&runtime->recording,false);atomic_store(&runtime->wants_record,false);}
    bool queued=xQueueSend(runtime->control,&c,0)==pdTRUE;
    if(!queued)atomic_store(&runtime->recording,false);
    return queued;
}
void companion_platform_brightness(unsigned value)
{esp_err_t err=bsp_display_brightness_set(value);if(err!=ESP_OK)ESP_LOGW("companion","brightness failed: %s",esp_err_to_name(err));}
void companion_platform_load_prefs(companion_snapshot_t *prefs)
{
    nvs_handle_t h;if(nvs_open("companion",NVS_READONLY,&h)!=ESP_OK)return;
    uint8_t value;if(nvs_get_u8(h,"volume",&value)==ESP_OK)prefs->volume=value;
    if(nvs_get_u8(h,"brightness",&value)==ESP_OK)prefs->brightness=value;
    if(nvs_get_u8(h,"language",&value)==ESP_OK)prefs->language=value;
    if(nvs_get_u8(h,"sleep_sec",&value)==ESP_OK)prefs->sleep_seconds=value;
    if(nvs_get_u8(h,"page_sec",&value)==ESP_OK)prefs->page_seconds=value;
    nvs_close(h);
}
