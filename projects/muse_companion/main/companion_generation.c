// SPDX-License-Identifier: Apache-2.0
#include "companion_generation.h"
#include "octop_link.h"
#include "esp_iris.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
enum { GENERATION_SERVICE=0x7e10, PROMPT_CAP=32768, REPLY_CAP=65536,
    READ_CHUNK=2048 };
typedef enum { GEN_INPUT,GEN_QUEUED,GEN_RUNNING,GEN_DONE,GEN_FAILED } gen_state_t;
struct companion_generation_t {
    SemaphoreHandle_t lock;
    char *prompt,*reply;
    size_t prompt_len,reply_len;
    gen_state_t state;
    bool cancel,overflow;
    char error[80];
};
static void finish_locked(companion_generation_handle_t g,const char *error)
{
    const char *why=error?error:g->overflow?"reply too large":
        !g->reply_len?"empty reply":NULL;
    g->state=why?GEN_FAILED:GEN_DONE;
    snprintf(g->error,sizeof(g->error),"%s",why?why:"");
}
/* Runs on the Octop link task: only the reply buffer and state are touched,
 * both under the service mutex, and never while an RPC is unanswered. */
static void capture(void *ctx,const char *event,const char *text)
{
    companion_generation_handle_t g=ctx;
    xSemaphoreTake(g->lock,portMAX_DELAY);
    if(g->state!=GEN_RUNNING||g->cancel)goto done;
    if(!strcmp(event,"start")){g->reply_len=0;g->reply[0]='\0';}
    else if(!strcmp(event,"delta")&&text&&text[0]){
        size_t n=strlen(text);
        if(n>=REPLY_CAP-g->reply_len)g->overflow=true;
        else{memcpy(g->reply+g->reply_len,text,n);g->reply_len+=n;g->reply[g->reply_len]='\0';}
    }
    else if(!strcmp(event,"done"))finish_locked(g,NULL);
    else if(!strcmp(event,"error"))finish_locked(g,text&&text[0]?text:"octop failed");
done:
    xSemaphoreGive(g->lock);
}
static uint32_t offset(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static esp_err_t rpc(const esp_iris_rpc_request_t *q,uint8_t *out,size_t cap,size_t *size,void *ctx)
{
    companion_generation_handle_t g=ctx;esp_err_t err=ESP_OK;*size=0;
    xSemaphoreTake(g->lock,portMAX_DELAY);
    switch(q->method_id){
    case 1: /* Reset an idle/completed request. */
        if(q->payload_size){err=ESP_ERR_INVALID_SIZE;break;}
        if(g->state==GEN_RUNNING||g->state==GEN_QUEUED){err=ESP_ERR_INVALID_STATE;break;}
        g->state=GEN_INPUT;g->prompt_len=g->reply_len=0;
        g->cancel=g->overflow=false;
        g->prompt[0]=g->reply[0]=g->error[0]=0;break;
    case 2: /* LE32 offset followed by UTF-8 prompt bytes. */
        if(q->payload_size<4){err=ESP_ERR_INVALID_SIZE;break;}
        if(g->state!=GEN_INPUT){err=ESP_ERR_INVALID_STATE;break;}
        if(offset(q->payload)!=g->prompt_len){err=ESP_ERR_INVALID_ARG;break;}
        if(q->payload_size-4>=PROMPT_CAP-g->prompt_len||memchr(q->payload+4,0,q->payload_size-4)){
            err=ESP_ERR_INVALID_SIZE;break;
        }
        memcpy(g->prompt+g->prompt_len,q->payload+4,q->payload_size-4);
        g->prompt_len+=q->payload_size-4;g->prompt[g->prompt_len]=0;break;
    case 3:
        if(q->payload_size){err=ESP_ERR_INVALID_SIZE;break;}
        if(g->state!=GEN_INPUT||!g->prompt_len||!octop_link_ready()){
            err=ESP_ERR_INVALID_STATE;break;
        }
        g->state=GEN_QUEUED;break;
    case 4:{
        if(q->payload_size){err=ESP_ERR_INVALID_SIZE;break;}
        static const char *const states[]={"input","queued","running","done","failed"};
        int n=snprintf((char*)out,cap,"{\"state\":\"%s\",\"prompt_bytes\":%u,\"reply_bytes\":%u,\"error\":\"%s\"}",
            states[g->state],(unsigned)g->prompt_len,(unsigned)g->reply_len,g->error);
        if(n<0||(size_t)n>=cap)err=ESP_ERR_INVALID_SIZE;else *size=(size_t)n;
        break;}
    case 5:{
        if(q->payload_size!=4){err=ESP_ERR_INVALID_SIZE;break;}
        if(g->state!=GEN_DONE&&g->state!=GEN_FAILED){err=ESP_ERR_INVALID_STATE;break;}
        size_t at=offset(q->payload);
        if(at>g->reply_len){err=ESP_ERR_INVALID_ARG;break;}
        size_t n=g->reply_len-at;if(n>READ_CHUNK)n=READ_CHUNK;if(n>cap)n=cap;
        memcpy(out,g->reply+at,n);*size=n;break;}
    case 6:
        if(q->payload_size){err=ESP_ERR_INVALID_SIZE;break;}
        g->cancel=true;
        if(g->state==GEN_QUEUED){g->state=GEN_FAILED;strcpy(g->error,"cancelled");}
        break;
    default:err=ESP_ERR_NOT_SUPPORTED;break;
    }
    xSemaphoreGive(g->lock);return err;
}
esp_err_t companion_generation_create(companion_generation_handle_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    *out=NULL;
    companion_generation_handle_t g=calloc(1,sizeof(*g));if(!g)return ESP_ERR_NO_MEM;
    g->lock=xSemaphoreCreateMutex();
    g->prompt=heap_caps_calloc(1,PROMPT_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    g->reply=heap_caps_calloc(1,REPLY_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!g->lock||!g->prompt||!g->reply){
        if(g->lock)vSemaphoreDelete(g->lock);
        free(g->prompt);free(g->reply);free(g);return ESP_ERR_NO_MEM;
    }
    /* Initialization runs before any client can call this private service. */
    for(unsigned method=1;method<=6;++method){
        esp_err_t e=esp_iris_rpc_register(GENERATION_SERVICE,method,rpc,g);
        if(e!=ESP_OK){
            /* The dispatcher may already serve other services. If unregister
             * is busy, retain the context rather than leave a dangling RPC. */
            bool released=true;
            for(unsigned prior=1;prior<method;++prior)
                if(esp_iris_rpc_unregister(GENERATION_SERVICE,prior)!=ESP_OK)released=false;
            if(released){vSemaphoreDelete(g->lock);free(g->prompt);free(g->reply);free(g);}
            return e;
        }
    }
    *out=g;
    octop_text_observer(capture,g);return ESP_OK;
}
const char *companion_generation_take(companion_generation_handle_t g)
{
    if(!g)return NULL;
    const char *prompt=NULL;
    xSemaphoreTake(g->lock,portMAX_DELAY);
    if(g->state==GEN_QUEUED){g->state=GEN_RUNNING;prompt=g->prompt;}
    xSemaphoreGive(g->lock);return prompt;
}
bool companion_generation_cancelled(companion_generation_handle_t g)
{
    xSemaphoreTake(g->lock,portMAX_DELAY);bool cancel=g->cancel;
    xSemaphoreGive(g->lock);return cancel;
}
bool companion_generation_finished(companion_generation_handle_t g)
{
    xSemaphoreTake(g->lock,portMAX_DELAY);
    bool done=g->state==GEN_DONE||g->state==GEN_FAILED;
    xSemaphoreGive(g->lock);return done;
}
void companion_generation_finish(companion_generation_handle_t g,const char *error)
{
    xSemaphoreTake(g->lock,portMAX_DELAY);
    /* The Octop observer may have closed the turn microseconds earlier; a late
     * worker verdict must never overwrite a completed request. */
    if(g->state==GEN_RUNNING||g->state==GEN_QUEUED)finish_locked(g,error);
    xSemaphoreGive(g->lock);
}
