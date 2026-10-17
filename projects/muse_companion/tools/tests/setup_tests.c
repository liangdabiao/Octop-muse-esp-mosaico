#include "companion_credentials.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static unsigned char stored[2048],pending[2048];
static size_t stored_size,pending_size;
static bool fail_write,fail_commit;

esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h)
{assert(!strcmp(name,"companion_keys"));*h=1;return !stored_size&&mode==NVS_READONLY?ESP_ERR_NVS_NOT_FOUND:ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{(void)h;assert(!strcmp(key,"tokens"));if(*size<stored_size)return ESP_ERR_NVS_INVALID_LENGTH;memcpy(out,stored,stored_size);*size=stored_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size)
{(void)h;assert(!strcmp(key,"tokens")&&size<sizeof(pending));if(fail_write)return 8;memcpy(pending,in,size);pending_size=size;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h)
{(void)h;if(fail_commit)return 9;memcpy(stored,pending,pending_size);stored_size=pending_size;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}

static void credentials_test(void)
{
    companion_credentials_config_t c={"http://octop.home.arpa:8088","mochi","plush yarn",NULL,NULL};
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    assert(companion_credentials_valid(&c));assert(companion_credentials_save(&c)==ESP_OK);
    assert(!companion_credentials_ready()); /* No concurrent snapshot mutation. */
    assert(companion_credentials_init()==ESP_OK&&companion_credentials_ready());
    assert(!strcmp(companion_credentials_octop_url(),c.octop_url));
    assert(!strcmp(companion_credentials_username(),c.username));
    assert(!strcmp(companion_credentials_password(),c.password));
    /* Self-hosted Octop is plain http by default, so http must survive. */
    c.octop_url="https://octop.example.com:8443/base";assert(companion_credentials_valid(&c));
    c.octop_url="ftp://octop.example.com";assert(!companion_credentials_valid(&c));
    c.octop_url="http://";assert(!companion_credentials_valid(&c));
    c.octop_url="http://octop.example.com/proxy@evil";assert(!companion_credentials_valid(&c));
    c.octop_url="http://octop.example.com#frag";assert(!companion_credentials_valid(&c));
    c.octop_url="http://octo p.example.com";assert(!companion_credentials_valid(&c));
    char huge[COMPANION_OCTOP_URL_CAP+1];memset(huge,'a',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
    memcpy(huge,"http://",7);c.octop_url=huge;
    assert(!companion_credentials_valid(&c));
    c.octop_url="http://octop.home.arpa:8088";
    /* Passwords may hold spaces; usernames may not, they key the login form. */
    c.password="hunter2 keep warm";assert(companion_credentials_valid(&c));
    c.password="bad\nnewline";assert(!companion_credentials_valid(&c));
    c.password="";assert(!companion_credentials_valid(&c));
    c.password="plush yarn";c.username="mo chi";assert(!companion_credentials_valid(&c));
    c.username="mochi";
    /* Optional transcription overrides: absent, empty and valid all work. */
    assert(companion_credentials_valid(&c));
    c.stt_url="";c.stt_key="";assert(companion_credentials_valid(&c));
    c.stt_url="http://asr.home.arpa:9000/v1/audio/transcriptions";c.stt_key="sk-test";
    assert(companion_credentials_valid(&c));
    c.stt_url="not a url";assert(!companion_credentials_valid(&c));
    c.stt_url="";c.stt_key="bad key";assert(!companion_credentials_valid(&c));
    c.stt_url=NULL;c.stt_key=NULL;assert(companion_credentials_valid(&c));
    c.octop_url="";assert(companion_credentials_save(&c)==ESP_ERR_INVALID_ARG);
    c.octop_url="http://octop.home.arpa:8088";
    fail_write=true;assert(companion_credentials_save(&c)!=ESP_OK);fail_write=false;
    fail_commit=true;assert(companion_credentials_save(&c)!=ESP_OK);fail_commit=false;
    assert(companion_credentials_init()==ESP_OK);
    assert(!strcmp(companion_credentials_username(),"mochi"));
    c.username="second";assert(companion_credentials_save(&c)==ESP_OK);
    assert(companion_credentials_init()==ESP_OK);
    assert(!strcmp(companion_credentials_username(),"second"));
    /* Leftover records from older firmware are wiped instead of half-trusted. */
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    assert(companion_credentials_ready());
    stored[0]=3;assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    assert(companion_credentials_ready());
    stored[4]='x'; /* Version fits, the URL scheme inside does not. */
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    assert(companion_credentials_save(&c)==ESP_OK&&companion_credentials_init()==ESP_OK);
    memset(stored,'x',24);stored_size=24; /* Truncated record of another shape. */
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
    memset(stored,'x',sizeof(stored));stored_size=sizeof(stored);
    assert(companion_credentials_init()==ESP_OK&&!companion_credentials_ready());
}
int main(void){credentials_test();
    puts("PASS: octop url rules, optional stt overrides, persistent reload, write failures, legacy record wipe");
    return 0;}
