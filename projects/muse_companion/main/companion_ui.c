// SPDX-License-Identifier: Apache-2.0
#include "companion_ui.h"
#include "companion_model.h"
#include "companion_pager.h"
#include "companion_platform.h"
#include "character_profile.h"
#include "bundle_gsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { UI_TICK_MS=100, CHARACTER_X=90, CHARACTER_Y=75, CHARACTER_POSES=8 };
/* Bundle order is main.json, settings.json, read.json. */
enum { SCENE_HOME=0, SCENE_SETTINGS=1, SCENE_READ=2 };
/* Artwork slots of the home scene, fixed by the scene and not by the model:
 * the GSP scene still calls the receiving pose "speaking". */
enum { POSE_IDLE, POSE_LISTENING, POSE_THINKING, POSE_RECEIVING, POSE_SLEEPY,
       POSE_ERROR, POSE_HAPPY, POSE_BOOT };
/* Recognition and composition share one artwork but not one caption. */
static const uint8_t mode_pose[]={
    [COMP_IDLE]=POSE_IDLE,[COMP_LISTENING]=POSE_LISTENING,[COMP_STT]=POSE_THINKING,
    [COMP_THINKING]=POSE_THINKING,[COMP_RECEIVING]=POSE_RECEIVING,
    [COMP_SLEEPY]=POSE_SLEEPY,[COMP_ERROR]=POSE_ERROR};
/* Reading gestures. The paper rectangle mirrors read.json. */
enum { SWIPE_MIN_PX=64, PAPER_X0=12, PAPER_Y0=132, PAPER_X1=468, PAPER_Y1=444 };
struct companion_ui_t {
    esp_gsp_handle_t ui;
    companion_model_handle_t model;
    void *timer;
    companion_snapshot_t snapshot;
    /* Only the revision is needed to prove nothing moved; keeping a second full
     * snapshot cost 64 KiB of PSRAM and a 64 KiB copy on every painted page. */
    uint32_t previous_revision;
    companion_message_t message;
    bool last_key, ready, rendered, scene_pending;
    unsigned current_scene, requested_scene;
    unsigned wave_mask;
    unsigned tick, last_brightness;
    unsigned greeting_ticks, pet_ticks;
    int motion_pose;
    bool reading, auto_pages;
    unsigned reply_pages, page_elapsed_ms;
    bool touch_active, touch_swipe, swipe_held;
    int32_t touch_x, touch_y;
    esp_gsp_err_t last_error;
};
static esp_gsp_err_t (*const pose[])(esp_gsp_handle_t,bool)={
    gsp_companion_idle_set_visible,gsp_companion_listening_set_visible,
    gsp_companion_thinking_set_visible,gsp_companion_speaking_set_visible,
    gsp_companion_sleepy_set_visible,gsp_companion_error_set_visible};
static unsigned character_pose(companion_ui_handle_t s,const companion_snapshot_t *m)
{
    if(m->mode==COMP_IDLE&&!m->asleep){
        if(s->pet_ticks)return POSE_HAPPY;
        if(s->greeting_ticks)return POSE_BOOT;
    }
    return mode_pose[m->mode];
}
static const unsigned sleep_options[]={5,10,15,20,30,60};
static unsigned sleep_index(unsigned seconds)
{
    for(unsigned i=0;i<sizeof(sleep_options)/sizeof(sleep_options[0]);++i)
        if(sleep_options[i]==seconds)return i;
    return 2;
}
static void checked(companion_ui_handle_t s,esp_gsp_err_t err)
{ if(err!=ESP_GSP_OK){ s->last_error=err;fprintf(stderr,"companion UI error=%d\n",(int)err); } }
static void set_reading(companion_ui_handle_t s,bool on)
{ if(s->reading!=on){ s->reading=on;s->rendered=false; } }
static const char *english_error(const char *text)
{
    /* The link layer reports failures in Chinese, so every fixed sentence it
     * can produce has a caption here; unknown texts - including the two that
     * carry a numeric HTTP status - fall back to a safe line. */
    static const struct {const char *zh,*en;} errors[]={
        {"还没有配置服务，请先用手机完成配置","Not configured. Run phone setup"},
        {"网络未连接，请稍后重试","Wi-Fi offline. Try again"},
        {"Octop 还没就绪，请检查配置或网络","Octop not ready. Check setup"},
        {"Octop 连不上，请检查配置","Cannot reach Octop. Check setup"},
        {"Octop 对话失败，请检查连接后重试","Chat failed. Check connection"},
        {"与 Octop 的连接中断","Octop connection lost"},
        {"Octop 拒绝了这次请求","Octop refused the request"},
        {"问题没有发出去，请重试","Question not sent. Retry"},
        {"没有听到声音，请按住再说","No speech heard. Hold and talk"},
        {"没有听到声音","Nothing heard. Try again"},
        {"没有听清","Did not catch that"},
        {"语音识别失败","Recognition failed. Retry"},
        {"识别连接失败","Recognition unreachable"},
        {"识别服务未配置","Recognition not configured"},
        {"识别失败","Recognition failed"},
        {"没有收到文字回答，请重试","No reply received. Try again"},
        {"等了 45 秒没有回答，请重试","No answer in 45 s. Try again"},
        {"回答中断，请重试","Reply stopped. Try again"},
        {"录音传输未跟上，请重试","Recording interrupted. Retry"},
        {"本次等待超时，请重新提问","Timed out. Ask again"},
        {"操作繁忙，请重试","Busy. Please try again"},
        {"配置保存失败，请重试","Setup save failed. Retry"},
        {"登录已过期，正在重新登录","Session expired. Signing in again"},
        {"没有可用智能体","No agent available"},
        {"无法建立会话","Cannot open a conversation"},
        {"对话通道连不上","Chat channel unreachable"},
        {"地址无效","Address invalid. Check setup"},
        {"网络不可达","Server unreachable"}
    };
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i)
        if(!strcmp(text,errors[i].zh))return errors[i].en;
    return "Connection failed. Try again";
}
static const char *const english_link_state[]={
    "Not set","Wi-Fi offline","Preparing","Working","Octop ready","Octop unreachable"};
static const char *const chinese_link_state[]={
    "未配置","Wi-Fi 未连接","准备中","处理中","Octop 就绪","Octop 不可达"};
/* The home card is two 432 px lines of 20 px text, so the transcript is sliced
 * with the same pixel budget the reader uses for a page. Only complete UTF-8
 * codepoints are copied; whatever does not fit is marked with one ellipsis. */
enum { CARD_LINE_PX=432, CARD_LINE_BYTES=96 };
static void card_ellipsis(char *line,size_t cap)
{
    size_t n=strlen(line);
    if(n+4<=cap) { memcpy(line+n,"…",3);line[n+3]='\0';return; }
    size_t i=n;
    while(i>1&&((unsigned char)line[i-1]&0xc0)==0x80) --i;
    if(i+4<=cap) { memcpy(line+i,"…",3);line[i+3]='\0'; }
}
static void question_lines(const char *text,char *first,size_t c1,char *second,size_t c2)
{
    first[0]=second[0]='\0';
    char *out=first;size_t cap=c1,written=0;unsigned used=0;
    size_t in=0,len=strlen(text);
    bool clipped=false;
    while(in<len) {
        unsigned char c=(unsigned char)text[in];
        size_t b=comp_utf8_bytes(c);
        if(in+b>len)break;
        unsigned w=comp_char_px(c,b);
        if(used+w>(unsigned)CARD_LINE_PX) {
            if(out!=first){clipped=true;break;}
            out=second;cap=c2;written=0;used=0;
        }
        if(written+b+1>=cap){clipped=true;break;}
        memcpy(out+written,text+in,b);
        written+=b;out[written]='\0';
        used+=w;in+=b;
    }
    if(clipped)card_ellipsis(out,cap);
}
static void render_read(companion_ui_handle_t s,const companion_snapshot_t *m)
{
    bool en=m->language!=0;char body[COMP_PAGE_BYTES],text[96];
    unsigned pages=s->reply_pages?s->reply_pages:1,page=m->page<pages?m->page:pages-1;
    comp_page_text(m->reply,page,body,sizeof(body));
    checked(s,gsp_companion_read_body_set_text(s->ui,body));
    checked(s,gsp_companion_read_title_set_text(s->ui,en?"Octop reply":"Octop 的回答"));
    snprintf(text,sizeof(text),en?"Page %u/%u":"第 %u / %u 页",page+1,pages);
    checked(s,gsp_companion_read_page_set_text(s->ui,text));
    checked(s,gsp_companion_read_page_prev_set_visible(s->ui,page>0));
    checked(s,gsp_companion_read_page_next_set_visible(s->ui,page+1<pages));
    if(m->mode==COMP_RECEIVING)
        snprintf(text,sizeof(text),en?"Receiving…":"正在接收回答…");
    /* The last finished page closes the reader, so never promise a next page. */
    else if(page+1>=pages) snprintf(text,sizeof(text),en?"Tap to close":"读完轻触返回");
    else if(s->auto_pages)
        snprintf(text,sizeof(text),en?"Auto %us · tap to skip":"自动翻页 %u 秒 · 轻触抢先",
            m->page_seconds);
    else snprintf(text,sizeof(text),en?"Tap next · hold home":"轻触下一页 · 长按回首页");
    checked(s,gsp_companion_read_hint_set_text(s->ui,text));
    s->previous_revision=m->revision;s->rendered=true;
}
static void render(companion_ui_handle_t s)
{
    companion_model_snapshot(s->model,&s->snapshot);
    companion_snapshot_t *m=&s->snapshot;char text[128];
    if(s->last_brightness!=m->display_brightness){
        companion_platform_brightness(m->display_brightness);s->last_brightness=m->display_brightness;
    }
    if(s->scene_pending)return;
    /* The model bumps a revision on every change, so an idle tick no longer has
     * to walk a 64 KiB answer twice just to prove that nothing moved. */
    if(s->rendered&&m->revision==s->previous_revision)return;
    const char *ssid;
    bool setup=companion_platform_setup(&ssid,NULL);
    unsigned wanted=SCENE_HOME;
    if(!setup) wanted=m->settings?SCENE_SETTINGS:(s->reading?SCENE_READ:SCENE_HOME);
    if(wanted!=s->current_scene){
        s->requested_scene=wanted;s->scene_pending=true;
        esp_gsp_err_t err=esp_gsp_goto_scene(s->ui,wanted,ESP_GSP_NO_TRANSITION);
        checked(s,err);
        if(err!=ESP_GSP_OK)s->scene_pending=false;
        s->wave_mask=0;
        return;
    }
    bool en=m->language!=0;
    if(setup){
        for(unsigned i=0;i<6;++i)checked(s,pose[i](s->ui,i==POSE_IDLE));
        checked(s,gsp_companion_happy_set_visible(s->ui,false));
        checked(s,gsp_companion_boot_set_visible(s->ui,false));
        checked(s,gsp_companion_connection_set_text(s->ui,en?"First-time setup":"首次配置"));
        checked(s,gsp_companion_caption_set_text(s->ui,""));
        checked(s,gsp_companion_reply_card_set_visible(s->ui,true));
        checked(s,gsp_companion_reply_text_set_text(s->ui,ssid));
        checked(s,gsp_companion_reply_text_2_set_text(s->ui,en?"No password needed":"无需密码，直接连接"));
        checked(s,gsp_companion_hint_set_text(s->ui,en?"Join Wi-Fi / 192.168.4.1":"手机连接热点 / 192.168.4.1"));
        checked(s,gsp_companion_talk_hint_set_text(s->ui,"http://192.168.4.1"));
        checked(s,esp_gsp_component_set_visible(s->ui,GSP_COMPANION_OBJ_KEY_SETTINGS_OPEN,false));
        checked(s,gsp_companion_icon_gear_set_visible(s->ui,false));
        s->previous_revision=m->revision;s->rendered=true;return;
    }
    if(m->settings){
        checked(s,gsp_companion_settings_volume_set_value(s->ui,m->volume));
        checked(s,gsp_companion_settings_brightness_set_value(s->ui,m->brightness));
        snprintf(text,sizeof(text),en?"Mic %u%%":"拾音 %u%%",m->volume);
        checked(s,gsp_companion_settings_volume_text_set_text(s->ui,text));
        snprintf(text,sizeof(text),en?"Brightness %u%%":"亮度 %u%%",m->brightness);
        checked(s,gsp_companion_settings_brightness_text_set_text(s->ui,text));
        checked(s,gsp_companion_settings_title_set_text(s->ui,en?"Settings":"伙伴设置"));
        checked(s,gsp_companion_settings_phone_setup_set_text(s->ui,en?"Phone setup":"手机配置"));
        checked(s,gsp_companion_settings_back_label_set_text(s->ui,en?"Back":"返回"));
        checked(s,gsp_companion_settings_language_text_set_text(s->ui,en?"Language":"语言"));
        checked(s,gsp_companion_settings_language_value_set_text(s->ui,en?"English":"中文"));
        checked(s,gsp_companion_settings_sleep_set_value(s->ui,sleep_index(m->sleep_seconds)));
        checked(s,gsp_companion_settings_page_set_value(s->ui,m->page_seconds));
        snprintf(text,sizeof(text),en?"Screen off %us":"自动熄屏 %u秒",m->sleep_seconds);
        checked(s,gsp_companion_settings_sleep_text_set_text(s->ui,text));
        snprintf(text,sizeof(text),en?"Auto page %us":"翻页 %u秒",m->page_seconds);
        checked(s,gsp_companion_settings_page_text_set_text(s->ui,text));
        char status[192],detail[48];
        unsigned state=companion_platform_link_state();
        companion_platform_link_detail(detail,sizeof(detail));
        if(state>5)state=0;
        if(en)snprintf(status,sizeof(status),"Wi-Fi %s · %s",
            m->online?"online":"offline",english_link_state[state]);
        else snprintf(status,sizeof(status),"Wi-Fi %s · %s%s%s",
            m->online?"已连接":"未连接",chinese_link_state[state],
            detail[0]?" · ":"",detail);
        checked(s,gsp_companion_settings_pair_status_set_text(s->ui,status));
        s->previous_revision=m->revision;s->rendered=true;return;
    }
    if(s->current_scene==SCENE_READ){ render_read(s,m); return; }
    unsigned visual=character_pose(s,m);
    bool happy=visual==POSE_HAPPY, greeting=visual==POSE_BOOT;
    for(unsigned i=0;i<6;++i) checked(s,pose[i](s->ui,!happy&&!greeting&&i==mode_pose[m->mode]));
    checked(s,gsp_companion_happy_set_visible(s->ui,happy));
    checked(s,gsp_companion_boot_set_visible(s->ui,greeting));
    checked(s,gsp_companion_heart_0_set_visible(s->ui,happy&&!COMPANION_ANIMATED_CHARACTER));
    checked(s,gsp_companion_heart_1_set_visible(s->ui,happy&&!COMPANION_ANIMATED_CHARACTER));
    for(unsigned i=0;i<3;++i){
        static esp_gsp_err_t (*const thought[])(esp_gsp_handle_t,bool)={
            gsp_companion_thought_0_set_visible,gsp_companion_thought_1_set_visible,gsp_companion_thought_2_set_visible};
        checked(s,thought[i](s->ui,!COMPANION_ANIMATED_CHARACTER&&m->mode==COMP_THINKING));
    }
    snprintf(text,sizeof(text),"%s",m->paired?(m->online?(en?"Octop online":"Octop 在线"):(en?"Connecting":"正在连接")):(en?"Service offline":"服务未就绪"));
    checked(s,gsp_companion_connection_set_text(s->ui,text));
    if(m->battery>=0) snprintf(text,sizeof(text),"%d",m->battery);
    else snprintf(text,sizeof(text),"--");
    checked(s,gsp_companion_battery_set_text(s->ui,text));
    checked(s,gsp_companion_battery_fill_set_value(s->ui,m->battery<0?0:(m->battery>100?100:m->battery)));
    const char *caption=en?"Hi! Want to chat?":"我在，想聊点什么？",*hint=en?"Hold the top AI key to talk":"按住顶部 AI 键说话";
    if(m->mode==COMP_LISTENING) {caption=(en?"Listening...":"我在听…");hint=(en?"Release AI to send":"松开 AI 键发送");}
    else if(m->mode==COMP_STT) {caption=(en?"Turning that into text…":"正在把你的话转成文字…");hint=(en?"Hold AI to ask again":"按住 AI 键可以重新提问");}
    else if(m->mode==COMP_THINKING) {caption=(en?"Thinking...":"让我想一想…");hint=(en?"Hold AI to ask again":"按住 AI 键可以重新提问");}
    else if(m->mode==COMP_SLEEPY) {caption=(en?"Taking a little nap...":"我先眯一会儿…");hint=(en?"Pick up, touch or press to wake":"拿起、触摸或按键唤醒");}
    else if(m->mode==COMP_ERROR) {caption=en?english_error(m->error):m->error;hint=(en?"Hold AI to try again":"按住 AI 键重试");}
    else if(m->reply[0]) {caption=(en?"Reply closed. Ask me anything":"回答已收起，随时再问我");hint=(en?"Hold the AI key to talk":"按住 AI 键说话");}
    /* While the agent writes, the card shows the words the microphone actually
     * caught - the one place the user can check what was understood. The caption
     * is cream on black and sits inside the same rectangle, so it goes quiet
     * whenever the card is up. */
    char line1[CARD_LINE_BYTES],line2[CARD_LINE_BYTES];
    bool showing=m->mode==COMP_THINKING&&m->question[0]&&!m->settings;
    if(showing)question_lines(m->question,line1,sizeof(line1),line2,sizeof(line2));
    checked(s,gsp_companion_reply_card_set_visible(s->ui,showing));
    checked(s,gsp_companion_caption_set_text(s->ui,showing?"":caption));
    checked(s,gsp_companion_reply_text_set_text(s->ui,showing?line1:""));
    checked(s,gsp_companion_reply_text_2_set_text(s->ui,showing?line2:""));
    checked(s,gsp_companion_hint_set_text(s->ui,hint));
    checked(s,gsp_companion_talk_hint_set_text(s->ui,hint));
    checked(s,gsp_companion_cancel_set_text(s->ui,en?"Cancel":"取消"));
    checked(s,esp_gsp_component_set_visible(s->ui,GSP_COMPANION_OBJ_KEY_CANCEL,
        m->mode==COMP_LISTENING||m->mode==COMP_STT||m->mode==COMP_THINKING));
    s->previous_revision=m->revision;s->rendered=true;
}
/* A tap on the last finished page closes the reader instead of blanking it. */
static void action(companion_ui_handle_t s,companion_action_t a,unsigned value);
static void read_page(companion_ui_handle_t s,int forward)
{
    companion_model_snapshot(s->model,&s->snapshot);
    if(s->snapshot.asleep){ action(s,COMP_WAKE,0);return; }
    if(forward>0&&s->snapshot.mode==COMP_IDLE&&s->snapshot.page+1>=s->reply_pages)
        { set_reading(s,false);return; }
    action(s,forward>0?COMP_PAGE_NEXT:COMP_PAGE_PREV,0);
}
static void action_apply(companion_ui_handle_t s,companion_action_t a,unsigned value,bool persist)
{
    if(a==COMP_PRESS||a==COMP_CANCEL) set_reading(s,false);
    if(a==COMP_PAGE_NEXT||a==COMP_PAGE_PREV){
        s->auto_pages=false;s->page_elapsed_ms=0;s->rendered=false;
    }
    unsigned effects=companion_model_action(s->model,a,value);
    /* Drag events preview immediately; only release commits preferences to flash. */
    if(!persist&&(effects&COMP_EFFECT_PREFS))effects=(effects&~COMP_EFFECT_PREFS)|COMP_EFFECT_PREVIEW;
    companion_model_snapshot(s->model,&s->snapshot);
    if(effects&&!companion_platform_effect(effects,&s->snapshot))
        companion_model_error(s->model,s->snapshot.generation,"操作繁忙，请重试");
    render(s);
}
static void action(companion_ui_handle_t s,companion_action_t a,unsigned value)
{action_apply(s,a,value,true);}
static void slider(companion_ui_handle_t s,companion_action_t a,unsigned value,bool persist)
{
    companion_model_snapshot(s->model,&s->snapshot);
    unsigned previous=a==COMP_VOLUME?s->snapshot.volume:a==COMP_BRIGHTNESS?s->snapshot.brightness:
        a==COMP_SLEEP_TIMEOUT?s->snapshot.sleep_seconds:s->snapshot.page_seconds;
    if(a==COMP_PAGE_INTERVAL)s->page_elapsed_ms=0;
    if(!persist&&value==previous)action(s,COMP_WAKE,0);
    else action_apply(s,a,value,persist);
}
static int rise(unsigned tick,unsigned period,unsigned pixels)
{
    unsigned half=period/2,p=tick%period;
    if(p>half)p=period-p;
    return (int)(pixels*p*p*(3*half-2*p)/(half*half*half));
}
static void position(companion_ui_handle_t s,unsigned key,int x,int y)
{
    gsp_value_t v={.type=GSP_VALUE_I32,.data.i32=x};
    checked(s,esp_gsp_component_set_property(s->ui,key,GSP_PROPERTY_KEY_X,&v));
    v.data.i32=y;
    checked(s,esp_gsp_component_set_property(s->ui,key,GSP_PROPERTY_KEY_Y,&v));
}
static void character_motion(companion_ui_handle_t s)
{
    if(COMPANION_ANIMATED_CHARACTER)return; /* GSP owns GIF playback. */
    if(s->scene_pending||s->current_scene!=SCENE_HOME||s->snapshot.settings)return;
    static const unsigned keys[CHARACTER_POSES]={GSP_COMPANION_OBJ_KEY_IDLE,GSP_COMPANION_OBJ_KEY_LISTENING,
        GSP_COMPANION_OBJ_KEY_THINKING,GSP_COMPANION_OBJ_KEY_SPEAKING,GSP_COMPANION_OBJ_KEY_SLEEPY,
        GSP_COMPANION_OBJ_KEY_ERROR,GSP_COMPANION_OBJ_KEY_HAPPY,GSP_COMPANION_OBJ_KEY_BOOT};
    unsigned mode=character_pose(s,&s->snapshot);
    if(mode>=CHARACTER_POSES)return;
    if(s->motion_pose!=(int)mode){
        if(s->motion_pose>=0)position(s,keys[s->motion_pose],CHARACTER_X,CHARACTER_Y);
        s->motion_pose=(int)mode;
    }
    if(s->snapshot.asleep)return;
    unsigned t=s->tick;int dx=0,dy=0;
    switch(mode){
    case POSE_IDLE:dy=-rise(t,40,6);break;
    case POSE_LISTENING:dx=rise(t,30,4)-2;dy=-rise(t,30,3);break;
    case POSE_THINKING:dx=rise(t,60,4)-2;dy=-rise(t,40,4);break;
    case POSE_RECEIVING:dx=rise(t,24,4)-2;dy=-rise(t,12,5);break;
    case POSE_SLEEPY:dy=2-rise(t,60,3);break;
    case POSE_ERROR:dx=rise(t,10,14)-7;break;
    case POSE_HAPPY:dx=rise(t,24,4)-2;dy=-rise(t,12,14);break;
    case POSE_BOOT:dx=rise(t,30,8)-4;dy=-rise(t,20,4);break;
    default:break;
    }
    position(s,keys[mode],CHARACTER_X+dx,CHARACTER_Y+dy);
    if(mode==POSE_HAPPY){
        static const unsigned hearts[]={GSP_COMPANION_OBJ_KEY_HEART_0,GSP_COMPANION_OBJ_KEY_HEART_1};
        for(unsigned i=0;i<2;++i){
            gsp_value_t v={.type=GSP_VALUE_I32,.data.i32=105-(int)((t+i*13)%30)};
            checked(s,esp_gsp_component_set_property(s->ui,hearts[i],GSP_PROPERTY_KEY_Y,&v));
        }
    }
    if(mode==POSE_THINKING){
        static esp_gsp_err_t (*const dots[])(esp_gsp_handle_t,bool)={
            gsp_companion_thought_0_set_visible,gsp_companion_thought_1_set_visible,gsp_companion_thought_2_set_visible};
        for(unsigned i=0;i<3;++i)checked(s,dots[i](s->ui,(t/4)%3==i));
    }
}
/* Scene swiping is turned off in every scene because it navigates between
 * screens, not between pages of one answer, and GSP events carry no
 * coordinates. The pointer observer is the only place a drag can be measured,
 * so a horizontal fling is recognised here and the authored click that follows
 * it is swallowed. Tap, long press and the corner buttons work without it. */
static void pointer(esp_gsp_handle_t ui,int32_t x,int32_t y,bool pressed,void *ctx)
{
    (void)ui;companion_ui_handle_t s=ctx;
    if(pressed){
        if(s->touch_active)return;
        s->touch_active=true;s->touch_x=x;s->touch_y=y;
        s->touch_swipe=s->current_scene==SCENE_READ&&!s->scene_pending
            &&x>=PAPER_X0&&x<=PAPER_X1&&y>=PAPER_Y0&&y<=PAPER_Y1;
        return;
    }
    if(!s->touch_active)return;
    s->touch_active=false;
    if(!s->touch_swipe)return;
    s->touch_swipe=false;
    int32_t dx=x-s->touch_x,dy=y-s->touch_y;
    if(dx<0)dx=-dx;
    if(dy<0)dy=-dy;
    if(dx<(int32_t)SWIPE_MIN_PX||dx<2*dy)return;
    s->swipe_held=true; /* the release also fires the authored tap */
    read_page(s,x<s->touch_x?1:-1);
}
static void event(esp_gsp_handle_t ui,const esp_gsp_event_t *e,void *ctx)
{
    (void)ui;companion_ui_handle_t s=ctx;
    if(e->type==ESP_GSP_EVENT_SCENE_CHANGED){
        if(s->scene_pending)s->current_scene=s->requested_scene;
        s->scene_pending=false;s->rendered=false;s->motion_pose=-1;render(s);return;
    }
    if(companion_platform_setup(NULL,NULL))return;
    if(s->current_scene==SCENE_READ){
        if(s->swipe_held){ s->swipe_held=false;return; }
        if(gsp_companion_read_event_is_page_next(e)) read_page(s,1);
        else if(gsp_companion_read_event_is_page_prev(e)) read_page(s,-1);
        else if(gsp_companion_read_event_is_home(e)) set_reading(s,false);
        else action(s,COMP_WAKE,0);
        return;
    }
    if(gsp_companion_event_is_pet(e)){
        action(s,COMP_WAKE,0);
        if(s->snapshot.mode==COMP_IDLE){
            s->pet_ticks=40;s->greeting_ticks=0;s->rendered=false;render(s);
        }
    }
    else if(gsp_companion_event_is_ai_press(e)) action(s,COMP_PRESS,0);
    else if(gsp_companion_event_is_ai_release(e)) action(s,COMP_RELEASE,0);
    else if(gsp_companion_event_is_cancel(e)) action(s,COMP_CANCEL,0);
    else if(gsp_companion_event_is_settings_open(e)) action(s,COMP_SETTINGS,0);
    else if(gsp_companion_settings_event_is_phone_setup(e)){
        action(s,COMP_WAKE,0);
        if(!companion_platform_setup_request()){
            companion_model_action(s->model,COMP_BACK,0);
            companion_model_error(s->model,s->snapshot.generation,"操作繁忙，请重试");render(s);
        }
    }
    else if(gsp_companion_settings_event_is_settings_back(e)) action(s,COMP_BACK,0);
    else if(gsp_companion_settings_event_is_volume_preview(e)) slider(s,COMP_VOLUME,e->arg,false);
    else if(gsp_companion_settings_event_is_volume_change(e)) slider(s,COMP_VOLUME,e->arg,true);
    else if(gsp_companion_settings_event_is_brightness_preview(e)) slider(s,COMP_BRIGHTNESS,e->arg,false);
    else if(gsp_companion_settings_event_is_brightness_change(e)) slider(s,COMP_BRIGHTNESS,e->arg,true);
    else if(gsp_companion_settings_event_is_language_change(e)) action(s,COMP_LANGUAGE,!s->snapshot.language);
    else if(gsp_companion_settings_event_is_sleep_preview(e)||gsp_companion_settings_event_is_sleep_change(e))
        slider(s,COMP_SLEEP_TIMEOUT,sleep_options[e->arg<6?e->arg:5],gsp_companion_settings_event_is_sleep_change(e));
    else if(gsp_companion_settings_event_is_page_preview(e)||gsp_companion_settings_event_is_page_change(e))
        slider(s,COMP_PAGE_INTERVAL,e->arg,gsp_companion_settings_event_is_page_change(e));
    else action(s,COMP_WAKE,0);
}
static void tick(esp_gsp_handle_t ui,void *ctx)
{
    (void)ui;companion_ui_handle_t s=ctx;
    if(companion_platform_setup(NULL,NULL)){
        companion_model_action(s->model,COMP_WAKE,0);
        companion_model_tick(s->model,UI_TICK_MS);
        ++s->tick;render(s);return;
    }
    bool online,paired,key,motion;int battery;
    companion_platform_tick(UI_TICK_MS);
    companion_platform_status(&online,&paired,&battery,&key,&motion);
    if(companion_platform_setup_request_failed()){
        companion_model_action(s->model,COMP_BACK,0);
        companion_model_snapshot(s->model,&s->snapshot);
        companion_model_error(s->model,s->snapshot.generation,"配置保存失败，请重试");
    }
    companion_model_connection(s->model,online,paired,battery);
    if(key!=s->last_key) {
        s->last_key=key;
        action(s,key?COMP_PRESS:COMP_RELEASE,0);
    }
    if(motion) action(s,COMP_WAKE,0);
    while(companion_platform_message(&s->message)) {
        companion_message_t *msg=&s->message;
        if(msg->kind==COMP_MSG_REPLY){
            companion_model_snapshot(s->model,&s->snapshot);
            if(msg->generation==s->snapshot.generation&&s->snapshot.mode!=COMP_LISTENING){
                companion_model_reply(s->model,msg->generation,msg->text,msg->streaming);
                s->reply_pages=comp_page_count(msg->text);s->page_elapsed_ms=0;
                s->auto_pages=s->reply_pages>1;
                set_reading(s,true);
            }
        }
        else if(msg->kind==COMP_MSG_HEARD)
            companion_model_heard(s->model,msg->generation,msg->text);
        else { companion_model_error(s->model,msg->generation,msg->text);set_reading(s,false); }
    }
    companion_model_tick(s->model,UI_TICK_MS);
    companion_model_snapshot(s->model,&s->snapshot);
    if(s->snapshot.mode!=COMP_IDLE||s->snapshot.asleep){
        if(s->pet_ticks||s->greeting_ticks)s->rendered=false;
        s->pet_ticks=s->greeting_ticks=0;
    }else{
        if(s->pet_ticks&&!--s->pet_ticks)s->rendered=false;
        if(s->greeting_ticks&&!--s->greeting_ticks)s->rendered=false;
    }
    if(s->auto_pages&&s->snapshot.reply[0]&&!s->scene_pending&&s->current_scene==SCENE_READ&&
        !s->snapshot.asleep&&s->snapshot.mode==COMP_IDLE){
        s->page_elapsed_ms+=UI_TICK_MS;
        if(s->page_elapsed_ms>=s->snapshot.page_seconds*1000){
            s->page_elapsed_ms=0;
            if(s->snapshot.page+1<s->reply_pages){
                companion_model_action(s->model,COMP_AUTO_PAGE_NEXT,0);
                companion_model_snapshot(s->model,&s->snapshot);
            }
            if(s->snapshot.page+1>=s->reply_pages)s->auto_pages=false;
        }
    }
    unsigned mask=!s->snapshot.settings&&s->snapshot.mode==COMP_LISTENING ? (s->tick%2 ? 0x555U:0x7ffU):0;
    if(!s->scene_pending&&s->current_scene==SCENE_HOME&&!s->snapshot.settings&&mask!=s->wave_mask){
        static esp_gsp_err_t (*const bars[])(esp_gsp_handle_t,bool)={
            gsp_companion_wave_0_set_visible,gsp_companion_wave_1_set_visible,
            gsp_companion_wave_2_set_visible,gsp_companion_wave_3_set_visible,
            gsp_companion_wave_4_set_visible,gsp_companion_wave_5_set_visible,
            gsp_companion_wave_6_set_visible,gsp_companion_wave_7_set_visible,
            gsp_companion_wave_8_set_visible,gsp_companion_wave_9_set_visible,
            gsp_companion_wave_10_set_visible};
        for(unsigned i=0;i<11;++i)if((mask^s->wave_mask)&(1U<<i))checked(s,bars[i](s->ui,(mask&(1U<<i))!=0));
        s->wave_mask=mask;
    }
    ++s->tick;render(s);character_motion(s);
}
esp_gsp_err_t companion_ui_create(esp_gsp_handle_t ui,companion_ui_handle_t *out)
{
    if(!ui||!out) return ESP_GSP_ERR_INVALID_ARG;
    *out=NULL;companion_ui_handle_t s=calloc(1,sizeof(*s));
    if(!s) return ESP_GSP_ERR_NO_MEM;
    s->ui=ui;s->last_brightness=101;s->model=companion_model_create();
    s->greeting_ticks=40;s->motion_pose=-1;
    if(!s->model) {free(s);return ESP_GSP_ERR_NO_MEM;}
    companion_model_snapshot(s->model,&s->snapshot);
    companion_platform_load_prefs(&s->snapshot);
    companion_model_prefs(s->model,s->snapshot.volume,s->snapshot.brightness);
    companion_model_options(s->model,s->snapshot.language,s->snapshot.sleep_seconds,s->snapshot.page_seconds);
    checked(s,esp_gsp_on_event(ui,event,s));
    /* Swipe is the only gesture that needs raw coordinates. A platform that
     * cannot observe the pointer still taps, long presses and uses the corner
     * buttons, so this result is reported without latching it into last_error. */
    esp_gsp_err_t observed=esp_gsp_set_pointer_observer(ui,pointer,s);
    if(observed!=ESP_GSP_OK)
        fprintf(stderr,"companion swipe paging unavailable=%d\n",(int)observed);
    s->timer=esp_gsp_timer_create(ui,UI_TICK_MS,tick,s);
    if(!s->timer) checked(s,ESP_GSP_ERR_NO_MEM);
    render(s);
    if(s->last_error!=ESP_GSP_OK) {esp_gsp_err_t err=s->last_error;companion_ui_delete(s);return err;}
    *out=s;return ESP_GSP_OK;
}
void companion_ui_delete(companion_ui_handle_t s)
{
    if(!s)return;
    esp_gsp_on_event(s->ui,NULL,NULL);
    esp_gsp_set_pointer_observer(s->ui,NULL,NULL);
    if(s->timer)esp_gsp_timer_delete(s->ui,s->timer);
    companion_model_delete(s->model);free(s);
}
