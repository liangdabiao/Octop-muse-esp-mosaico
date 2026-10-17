#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gsp_sim_bridge.h"
#include "companion_ui.h"
#include "companion_platform.h"
static companion_ui_handle_t app;
enum { QUEUE_DEPTH=8 };
static companion_message_t queue[QUEUE_DEPTH];
static unsigned queue_head,queue_count;
static uint32_t clock_ms,generation,reply_at,stream_step,heard_at;
static bool reply_error,streaming;
static char visible[4096];
/* Octop streams token by token; the simulator replays that as growing pages
 * so the reader and the pager can be exercised without a server. The eight
 * pieces add up to about three hundred and ninety hanzi, which is two of the
 * 220-character reading pages and makes the final page short on purpose. */
static const char *const pieces[]={
    "我在的，Octop 已经听到了你刚才说的话。这段回答故意写得比较长，这样就能看清翻页和自动翻页的效果，",
    "设备本身不再发声，所有内容都只读文字，所以阅读页要把长答案切成一页一页可以读完的样子，",
    "第一页停在屏幕上时你可以轻触屏幕翻到下一页，也可以按住屏幕不放回到首页去，",
    "如果回答还在继续接收，页脚的提示会告诉你现在还在接收中，等它写完才会开始自动翻页，",
    "自动翻页的间隔在伙伴设置里可以调整，从三秒到十五秒都可以，慢一点就读得更从容一些，",
    "识别与思考是两个不同的阶段，松开按键以后先看到把话转成文字的提示，再看到思考中的提示，",
    "汉字排版按行宽核定，一行二十二字，一页十行，正好二百二十字，五百字的回答三页就读完了，",
    "最后这一段用来确认最后一页不会被截断，也不会把一个汉字的编码切成两半，读起来才干净。",
    NULL};
static bool env(const char *name){const char *v=getenv(name);return v&&!strcmp(v,"1");}
/* The simulated frame clock is not wall time, so a fixed one-second key hold
 * cannot be captured phase by phase. OCTOP_SIM_KEY_MS="start:end" (seconds)
 * moves the hold anywhere the caller wants to look at. */
static void key_window(unsigned *from,unsigned *to)
{
    *from=1000;*to=2000;
    const char *spec=getenv("OCTOP_SIM_KEY_MS");
    if(!spec||!spec[0])return;
    char *end=NULL;
    long start=strtol(spec,&end,10);
    if(end==spec||start<0)return;
    if(*end!=':'||strtol(end+1,NULL,10)<=start)return;
    *from=(unsigned)start*1000U;*to=(unsigned)strtol(end+1,NULL,10)*1000U;
}
/* Recognition hands back the transcript, and the home card is meant to show it.
 * This line is deliberately longer than the two card lines so the capture frames
 * prove the slice and the ellipsis instead of a lucky short sentence. */
static const char *const sim_transcript=
    "今天的流量情况怎么样，把每一个站点的在线数量和掉线数量都列出来给我看一下，越详细越好。";
static void push(companion_message_kind_t kind,const char *text,bool stream)
{
    if(queue_count==QUEUE_DEPTH){fprintf(stderr,"sim push dropped queue full\n");return;}
    companion_message_t *out=&queue[(queue_head+queue_count)%QUEUE_DEPTH];
    ++queue_count;
    memset(out,0,sizeof(*out));
    out->kind=kind;out->generation=generation;out->streaming=stream;
    snprintf(out->text,sizeof(out->text),"%s",text);
    /* Simulated time, not wall time: a capture script reads these lines to see
     * which phase a frame was taken in, because frames and ticks are not one
     * and the same rate. */
    fprintf(stderr,"sim push kind=%u stream=%d clock_ms=%u\n",kind,stream,clock_ms);
}
bool companion_platform_setup_request_failed(void){return false;}
bool companion_platform_setup_request(void)
{
    fprintf(stderr,"MOCK phone setup requested; hardware would restart into hotspot\n");
    return true;
}
bool companion_platform_setup(const char **ssid,const char **password)
{
    if(!env("OCTOP_SIM_SETUP"))return false;
    if(ssid)*ssid="Mochi-Setup-1234";
    if(password)*password="";
    return true;
}
void companion_platform_tick(uint32_t ms)
{
    clock_ms+=ms;
    if(heard_at&&clock_ms>=heard_at){ heard_at=0;push(COMP_MSG_HEARD,sim_transcript,false); }
    if(!reply_at)return;
    if(clock_ms<reply_at)return;
    if(reply_error){reply_at=0;push(COMP_MSG_ERROR,"Octop 连不上，请检查配置",false);return;}
    if(!streaming){reply_at=0;push(COMP_MSG_ERROR,"Octop 没有返回文字，请重试",false);return;}
    if(pieces[stream_step]){
        strcat(visible,pieces[stream_step]);
        push(COMP_MSG_REPLY,visible,true);
        ++stream_step;reply_at=clock_ms+700;
        return;
    }
    streaming=false;reply_at=0;
    push(COMP_MSG_REPLY,visible,false);
}
void companion_platform_status(bool *online,bool *paired,int *battery,bool *key,bool *motion)
{
    *online=!env("OCTOP_SIM_OFFLINE");*paired=*online&&!env("OCTOP_SIM_UNPAIRED");
    *battery=82;*motion=false;
    /* OCTOP_SIM_AUTOTURN holds the AI key so a whole turn - recognition,
     * thinking, streaming and the reading page - runs unattended. */
    unsigned from,to;key_window(&from,&to);
    *key=env("OCTOP_SIM_AUTOTURN")&&clock_ms>=from&&clock_ms<to;
}
bool companion_platform_message(companion_message_t *out)
{
    if(!queue_count)return false;
    *out=queue[queue_head];
    queue_head=(queue_head+1)%QUEUE_DEPTH;--queue_count;
    return true;
}
bool companion_platform_effect(unsigned effects,const companion_snapshot_t *s)
{
    generation=s->generation;
    if(effects&COMP_EFFECT_CANCEL){reply_at=0;heard_at=0;streaming=false;queue_head=queue_count=0;}
    if(effects&COMP_EFFECT_END){
        visible[0]='\0';stream_step=0;streaming=true;
        /* Recognition answers first, then the agent starts writing. */
        heard_at=clock_ms+(env("OCTOP_SIM_REPLY_ERROR")?1200:500);
        reply_at=clock_ms+(env("OCTOP_SIM_REPLY_ERROR")?1200:900);
        reply_error=env("OCTOP_SIM_REPLY_ERROR");
    }
    fprintf(stderr,"companion effects=%u generation=%u\n",effects,generation);
    fprintf(stderr,"preferences volume=%u brightness=%u language=%u sleep=%u page=%u\n",
        s->volume,s->brightness,s->language,s->sleep_seconds,s->page_seconds);
    return true;
}
void companion_platform_brightness(unsigned value){fprintf(stderr,"display brightness=%u\n",value);}
unsigned companion_platform_link_state(void)
{
    const char *value=getenv("OCTOP_SIM_LINK_STATE");
    return value?(unsigned)atoi(value):4U; /* OCTOP_REACHABLE */
}
void companion_platform_link_detail(char *out,size_t cap)
{
    const char *value=getenv("OCTOP_SIM_LINK_DETAIL");
    snprintf(out,cap,"%s",value?value:"");
}
void companion_platform_load_prefs(companion_snapshot_t *prefs)
{
    const char *language=getenv("OCTOP_SIM_LANGUAGE");
    if(language&&!strcmp(language,"en"))prefs->language=1;
}
esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    fprintf(stderr,"MOCK Octop link; shared native UI\n");
    return companion_ui_create(ui,&app);
}
void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{(void)ui;companion_ui_delete(app);app=NULL;}
