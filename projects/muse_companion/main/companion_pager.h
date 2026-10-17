// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <string.h>

/* Pagination for the 480x480 reading page.
 *
 * The body label is 440 px wide and 300 px tall, and Noto Sans SC at 20 px has
 * a 30 px line height, so a page is exactly ten lines of 440 px. Lines are
 * budgeted in estimated pixels rather than character counts because the font is
 * proportional: a CJK ideograph is a full 20 px em while the average ASCII
 * glyph is 10.8 px. That keeps 22 hanzi per line (220 per page) and still stops
 * a Latin sentence before the label would clip it.
 *
 * Both the page slicer and the line wrapper use the same width model, so a page
 * can never need an eleventh line. Complete UTF-8 codepoints only: a page never
 * begins inside a codepoint, which is what keeps a truncated tail from turning
 * into a tofu box. */
enum { COMP_LINE_PX=440, COMP_PAGE_LINES=10, COMP_PAGE_BYTES=1024 };

static inline size_t comp_utf8_bytes(unsigned char c)
{ return c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4; }
static inline unsigned comp_char_px(unsigned char c,size_t bytes)
{ return bytes==1?(c==' '?5:12):20; }

/* Bytes of `text` that fill page `page`, its start reported through offset.
 * Only an empty answer yields 0; a page index past the end clamps to the last
 * page so a counter that raced a shorter reply still shows text. */
static inline size_t comp_page_bounds(const char *text,unsigned page,size_t *offset)
{
    size_t start=0,len=strlen(text);
    *offset=start;
    for(unsigned i=0;;++i) {
        size_t end=start;unsigned used=0,lines=1;
        while(end<len) {
            unsigned char c=(unsigned char)text[end];
            size_t n=comp_utf8_bytes(c);
            if(end+n>len)break;
            unsigned w=comp_char_px(c,n);
            if(used+w>(unsigned)COMP_LINE_PX) {
                if(lines>=(unsigned)COMP_PAGE_LINES)break;
                ++lines;used=0;
            }
            used+=w;end+=n;
        }
        if(i==page||end==len) { *offset=start;return end-start; }
        start=end;
    }
}
static inline unsigned comp_page_count(const char *text)
{
    unsigned page=0;size_t offset,n,len=strlen(text);
    do {
        n=comp_page_bounds(text,page,&offset);
        if(!n)break; /* A truncated UTF-8 tail must not stall counting. */
        ++page;
    } while(offset+n<len);
    return page?page:1;
}
/* Wraps page `page` into hard line breaks inside out (COMP_PAGE_BYTES or more). */
static inline void comp_page_text(const char *text,unsigned page,char *out,size_t cap)
{
    size_t offset,n=comp_page_bounds(text,page,&offset);
    size_t in=offset,written=0;unsigned used=0,lines=1;
    out[0]='\0';
    while(in<offset+n) {
        unsigned char c=(unsigned char)text[in];
        size_t b=comp_utf8_bytes(c);
        if(in+b>offset+n || written+b+1>=cap)break;
        unsigned w=comp_char_px(c,b);
        if(used+w>(unsigned)COMP_LINE_PX) {
            if(lines>=(unsigned)COMP_PAGE_LINES || written+2>=cap)break;
            out[written++]='\n';++lines;used=0;
        }
        if(c=='\n'||c=='\r')out[written++]=' ';
        else { memcpy(out+written,text+in,b);written+=b; }
        used+=w;in+=b;
    }
    out[written]='\0';
}
