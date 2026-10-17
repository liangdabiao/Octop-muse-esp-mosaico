// SPDX-License-Identifier: Apache-2.0
#include "muse_character.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
enum { FRAMES=40,PIXELS=MUSE_CHARACTER_SIDE*MUSE_CHARACTER_SIDE,GUARD=64 };
static struct {uint32_t before[GUARD],pixels[PIXELS],after[GUARD];} frame;
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"wb");if(!f)return 3;
    for(unsigned mode=0;mode<CHARACTER_MODE_COUNT;++mode){
        for(unsigned index=0;index<FRAMES;++index){
            for(unsigned i=0;i<GUARD;++i)frame.before[i]=frame.after[i]=0xdeadbeefU;
            for(unsigned i=0;i<PIXELS;++i)frame.pixels[i]=0xffabcdefU;
            muse_character_pose_t pose={.mode=(muse_character_mode_t)mode,
                .t=(float)index*0.1f,.level=0.3f+0.7f*fabsf(sinf((float)index*0.7f)),
                .happy=mode==CHARACTER_HAPPY?1.0f:0.0f};
            muse_character_render(&pose,frame.pixels);
            for(unsigned i=0;i<GUARD;++i){
                if(frame.before[i]!=0xdeadbeefU||frame.after[i]!=0xdeadbeefU){fclose(f);return 4;}
            }
            for(unsigned i=0;i<PIXELS;++i){
                uint32_t c=frame.pixels[i];
                if(c&0xff000000U){fclose(f);return 5;}
                unsigned char bytes[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};
                if(fwrite(bytes,1,3,f)!=3){fclose(f);return 6;}
            }
        }
    }
    if(fclose(f))return 7;
    return 0;
}
