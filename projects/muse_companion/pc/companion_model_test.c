#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "companion_model.h"
#include "companion_pager.h"
/* Repeat a multi-byte glyph to build the answer shapes the reader must cope with. */
static void glyphs(char *out,const char *glyph,unsigned count)
{
    size_t n=strlen(glyph);
    for(unsigned i=0;i<count;++i)memcpy(out+(size_t)i*n,glyph,n);
    out[(size_t)count*n]='\0';
}
static unsigned line_px(const char *line)
{
    unsigned used=0;
    /* Measure one wrapped line, not the remainder of the page. */
    for(size_t i=0;line[i]&&line[i]!='\n';) {
        unsigned char c=(unsigned char)line[i];
        size_t n=comp_utf8_bytes(c);
        used+=comp_char_px(c,n);i+=n;
    }
    return used;
}
/* Every line fits the 440 px column, no page overflows ten lines, and no
 * page ever begins or ends inside a codepoint. */
static void assert_page_shape(const char *text)
{
    unsigned pages=comp_page_count(text);
    char page[COMP_PAGE_BYTES];
    size_t walked=0;
    for(unsigned i=0;i<pages;++i) {
        size_t offset=0,n=comp_page_bounds(text,i,&offset);
        assert(n>0&&offset==walked&&offset+n<=strlen(text));
        /* A boundary on 0x80..0xBF would draw a tofu box for half a glyph. */
        assert((((unsigned char)text[offset])&0xc0)!=0x80);
        comp_page_text(text,i,page,sizeof(page));
        size_t raw=0;unsigned lines=1,line_start=0;
        for(size_t b=0;b<strlen(page);++b) {
            if(page[b]=='\n') {
                assert(line_px(page+line_start)<=(unsigned)COMP_LINE_PX);
                assert(++lines<=(unsigned)COMP_PAGE_LINES);
                line_start=b+1;continue;
            }
            char want=text[offset+raw];
            assert(page[b]==(want=='\n'||want=='\r'?' ':want));
            ++raw;
        }
        assert(line_px(page+line_start)<=(unsigned)COMP_LINE_PX);
        assert(raw==n);
        walked=offset+n;
    }
    assert(walked==strlen(text));
}
int main(void) {
    companion_model_handle_t m=companion_model_create();
    companion_snapshot_t *s=malloc(sizeof(*s));assert(m&&s);
    companion_model_snapshot(m,s);
    assert(s->mode==COMP_IDLE&&s->volume==55&&s->brightness==70);
    assert(s->sleep_seconds==15&&s->page_seconds==7&&s->battery==-1&&!s->reply[0]);
    /* Offline and unpaired both refuse the turn instead of recording silence. */
    unsigned effects=companion_model_action(m,COMP_PRESS,0);
    companion_model_snapshot(m,s);
    assert((effects&COMP_EFFECT_BEGIN)==0&&effects==COMP_EFFECT_CANCEL);
    assert(s->mode==COMP_ERROR&&strcmp(s->error,"网络未连接，请稍后重试")==0);
    assert(!(companion_model_action(m,COMP_RELEASE,0)&COMP_EFFECT_END));
    companion_model_connection(m,true,false,-1);
    companion_model_action(m,COMP_PRESS,0);companion_model_snapshot(m,s);
    assert(s->mode==COMP_ERROR&&strcmp(s->error,"Octop 还没就绪，请检查配置或网络")==0);
    assert(!(companion_model_action(m,COMP_RELEASE,0)&COMP_EFFECT_END));
    companion_model_connection(m,true,true,82);
    assert(companion_model_action(m,COMP_PRESS,0)&COMP_EFFECT_BEGIN);
    companion_model_snapshot(m,s);uint32_t old=s->generation;
    companion_model_action(m,COMP_CANCEL,0);
    assert(!(companion_model_action(m,COMP_RELEASE,0)&COMP_EFFECT_END));
    companion_model_reply(m,old,"stale",true);companion_model_snapshot(m,s);assert(!s->reply[0]);
    assert(companion_model_action(m,COMP_PRESS,0)&COMP_EFFECT_BEGIN);
    companion_model_snapshot(m,s);uint32_t gen=s->generation;
    assert(companion_model_action(m,COMP_RELEASE,0)&COMP_EFFECT_END);
    /* A finished answer can be paged, and a new turn restarts at page one. */
    companion_model_reply(m,gen,"first",false);
    companion_model_action(m,COMP_PAGE_NEXT,0);
    assert(companion_model_action(m,COMP_PRESS,0)&COMP_EFFECT_BEGIN);
    companion_model_snapshot(m,s);assert(s->page==0&&!s->reply[0]&&s->generation==gen+1);
    gen=s->generation;
    assert(companion_model_action(m,COMP_RELEASE,0)&COMP_EFFECT_END);
    companion_model_snapshot(m,s);assert(s->mode==COMP_STT);
    /* Octop still has to return the transcript: a stale event leaves the wait
     * alone, and only the current turn's transcript starts the thinking pose. */
    companion_model_heard(m,gen-1,"上一轮遗留的文字");companion_model_snapshot(m,s);
    assert(s->mode==COMP_STT&&!s->question[0]);
    companion_model_heard(m,gen,"今天的流量情况怎么样");companion_model_snapshot(m,s);
    assert(s->mode==COMP_THINKING&&strcmp(s->question,"今天的流量情况怎么样")==0);
    companion_model_reply(m,gen,"正在流式返回",true);companion_model_snapshot(m,s);
    assert(s->mode==COMP_RECEIVING&&s->page==0);
    /* Timer paging only reads finished answers, never a half-written page. */
    companion_model_action(m,COMP_AUTO_PAGE_NEXT,0);companion_model_snapshot(m,s);assert(s->page==0);
    companion_model_reply(m,gen,"完整的最终回答",false);companion_model_snapshot(m,s);
    assert(s->mode==COMP_IDLE&&strcmp(s->reply,"完整的最终回答")==0);
    companion_model_action(m,COMP_PAGE_NEXT,0);companion_model_action(m,COMP_AUTO_PAGE_NEXT,0);
    companion_model_snapshot(m,s);assert(s->page==2);
    companion_model_action(m,COMP_PAGE_PREV,0);companion_model_action(m,COMP_PAGE_PREV,0);
    companion_model_snapshot(m,s);assert(s->page==0);
    char huge[COMP_REPLY_CAP+1024];memset(huge,'a',sizeof(huge)-1);huge[sizeof(huge)-1]='\0';
    companion_model_reply(m,gen,huge,false);companion_model_snapshot(m,s);
    assert(strlen(s->reply)==COMP_REPLY_CAP-1);
    companion_model_action(m,COMP_SETTINGS,0);
    companion_model_action(m,COMP_AUTO_PAGE_NEXT,0);companion_model_snapshot(m,s);assert(s->page==0);
    companion_model_action(m,COMP_BACK,0);companion_model_snapshot(m,s);assert(!s->settings);
    /* Reading keeps the page bright while it dims, and never becomes sleepy. */
    companion_model_action(m,COMP_WAKE,0);
    companion_model_tick(m,12500);companion_model_snapshot(m,s);
    assert(s->display_brightness==70&&!s->asleep);
    companion_model_action(m,COMP_AUTO_PAGE_NEXT,0);
    companion_model_tick(m,1000);companion_model_snapshot(m,s);
    assert(s->page==1&&s->display_brightness<70&&s->display_brightness>8&&s->mode==COMP_IDLE);
    companion_model_tick(m,1500);companion_model_snapshot(m,s);assert(s->asleep&&s->display_brightness==0);
    companion_model_action(m,COMP_AUTO_PAGE_NEXT,0);companion_model_snapshot(m,s);assert(s->page==1);
    companion_model_action(m,COMP_WAKE,0);companion_model_snapshot(m,s);
    assert(!s->asleep&&s->display_brightness==70);
    const unsigned timeouts[]={5,10,15,20,30,60};
    for(unsigned i=0;i<sizeof(timeouts)/sizeof(timeouts[0]);++i){
        unsigned seconds=timeouts[i];
        companion_model_action(m,COMP_SLEEP_TIMEOUT,seconds);
        companion_model_tick(m,seconds*1000-2100);companion_model_snapshot(m,s);assert(!s->asleep&&s->display_brightness==70);
        companion_model_tick(m,1100);companion_model_snapshot(m,s);assert(!s->asleep&&s->display_brightness<70);
        companion_model_tick(m,1000);companion_model_snapshot(m,s);assert(s->asleep&&s->display_brightness==0);
        companion_model_action(m,COMP_WAKE,0);companion_model_snapshot(m,s);assert(!s->asleep&&s->display_brightness==70);
    }
    companion_model_options(m,1,5,3);companion_model_snapshot(m,s);
    assert(s->language==1&&s->sleep_seconds==5&&s->page_seconds==3);
    companion_model_action(m,COMP_PAGE_INTERVAL,16);companion_model_action(m,COMP_SLEEP_TIMEOUT,7);
    companion_model_snapshot(m,s);assert(s->page_seconds==7&&s->sleep_seconds==15);
    companion_model_options(m,0,999,999);companion_model_snapshot(m,s);
    assert(s->language==0&&s->sleep_seconds==15&&s->page_seconds==7);
    companion_model_action(m,COMP_PRESS,0);companion_model_snapshot(m,s);
    assert(!s->asleep&&s->display_brightness==70);
    companion_model_action(m,COMP_VOLUME,130);companion_model_action(m,COMP_BRIGHTNESS,2);
    companion_model_snapshot(m,s);assert(s->volume==100&&s->brightness==5);
    companion_model_error(m,s->generation,"Octop 连不上，请检查配置");
    companion_model_snapshot(m,s);assert(s->mode==COMP_ERROR&&s->error[0]);
    companion_model_reply(m,s->generation,"回答仍然覆盖错误",true);companion_model_snapshot(m,s);
    assert(s->mode==COMP_RECEIVING&&!s->error[0]);
    /* An error for a turn the user already abandoned must not flash on screen. */
    companion_model_error(m,s->generation-1,"过期的错误");
    companion_model_snapshot(m,s);assert(s->mode==COMP_RECEIVING);
    /* Pagination acceptance: 500 hanzi is three pages of 220, proportional
     * Latin packs denser without clipping, and no shape splits a codepoint. */
    {
        char cjk[1600],latin[2100],emoji[1300];
        size_t offset=0;
        glyphs(cjk,"字",500);assert_page_shape(cjk);
        assert(comp_page_count(cjk)==3);
        assert(comp_page_bounds(cjk,0,&offset)==220*3&&offset==0);
        assert(comp_page_bounds(cjk,2,&offset)==60*3&&offset==440*3);
        /* A stale page counter clamps to the last page, never to a blank card. */
        assert(comp_page_bounds(cjk,3,&offset)==60*3&&offset==440*3);
        memset(latin,'a',2000);latin[2000]='\0';assert_page_shape(latin);
        /* 12 px glyphs fit 36 to the line, so a Latin page is never 220 bytes. */
        assert(comp_page_bounds(latin,0,&offset)==360&&comp_page_count(latin)==6);
        glyphs(emoji,"\xf0\x9f\x98\x80",300);assert_page_shape(emoji);
        assert(comp_page_bounds(emoji,0,&offset)==220*4); /* still 220 wide chars */
        char mixed[512];
        strcpy(mixed,"Octop 说：行宽按像素核定，所以 ASCII 排得更密。\n");
        glyphs(mixed+strlen(mixed),"读",120);assert_page_shape(mixed);
        assert(comp_page_count("")==1&&comp_page_count("短")==1);
        char cut[8];glyphs(cut,"字",1);cut[3]='\xf0';cut[4]='\0';
        assert(comp_page_count(cut)==1); /* a truncated tail cannot stall */
    }
    companion_model_delete(m);free(s);
    puts("PASS: link gates, stt-then-thinking, turn paging, streaming vs final, long-reply truncation,");
    puts("      220-hanzi page geometry, dimming, wake, sleep suppression");
    return 0;
}
