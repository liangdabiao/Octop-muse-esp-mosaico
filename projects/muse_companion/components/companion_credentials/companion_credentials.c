// SPDX-License-Identifier: Apache-2.0
#include "companion_credentials.h"
#include "nvs.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CREDENTIALS_VERSION=2 };
typedef struct {
    uint32_t version;
    char url[COMPANION_OCTOP_URL_CAP], user[COMPANION_USERNAME_CAP],
         pass[COMPANION_PASSWORD_CAP], stt_url[COMPANION_STT_URL_CAP],
         stt_key[COMPANION_STT_KEY_CAP];
} credentials_t;
static credentials_t credentials;

static bool text_valid(const char *text,size_t cap,bool allow_space)
{
    if(!text)return false;
    size_t n=strnlen(text,cap);
    if(!n||n>=cap)return false;
    for(size_t i=0;i<n;++i){
        unsigned char c=(unsigned char)text[i];
        if(allow_space){if(c<32||c>126)return false;}
        else if(c<33||c>126)return false;
    }
    return true;
}
/* Self-hosted Octop is served over plain http:// by default (port 8088), so
 * http is accepted here; '@' and '#' stay banned because they change how a
 * URL is parsed and can smuggle credentials into the host part. */
static bool url_valid(const char *url,size_t cap,bool required)
{
    if(!url)return !required;
    if(!url[0])return !required;
    if(!text_valid(url,cap,false))return false;
    bool https=!strncmp(url,"https://",8);
    size_t prefix=https?8:7;
    if(!https&&strncmp(url,"http://",7))return false;
    return url[prefix]&&!strchr(url,'@')&&!strchr(url,'#');
}
bool companion_credentials_valid(const companion_credentials_config_t *c)
{
    if(!c)return false;
    /* Empty optional fields mean "use Octop's defaults"; the persisted
     * snapshot spells those as "" rather than NULL, so both forms validate. */
    return url_valid(c->octop_url,COMPANION_OCTOP_URL_CAP,true)&&
        text_valid(c->username,COMPANION_USERNAME_CAP,false)&&
        text_valid(c->password,COMPANION_PASSWORD_CAP,true)&&
        url_valid(c->stt_url,COMPANION_STT_URL_CAP,false)&&
        (!c->stt_key||!c->stt_key[0]||text_valid(c->stt_key,COMPANION_STT_KEY_CAP,false));
}
static companion_credentials_config_t snapshot_of(const credentials_t *v)
{
    companion_credentials_config_t c;
    c.octop_url=v->url;c.username=v->user;c.password=v->pass;
    c.stt_url=v->stt_url;c.stt_key=v->stt_key;
    return c;
}
bool companion_credentials_ready(void)
{
    if(credentials.version!=CREDENTIALS_VERSION)return false;
    companion_credentials_config_t c=snapshot_of(&credentials);
    return companion_credentials_valid(&c);
}
esp_err_t companion_credentials_init(void)
{
    memset(&credentials,0,sizeof(credentials));
    nvs_handle_t h;
    esp_err_t err=nvs_open("companion_keys",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(err!=ESP_OK)return err;
    size_t size=sizeof(credentials);
    err=nvs_get_blob(h,"tokens",&credentials,&size);
    nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND||err==ESP_ERR_NVS_INVALID_LENGTH){
        memset(&credentials,0,sizeof(credentials));return ESP_OK;}
    if(err!=ESP_OK)return err;
    /* A v1 record (Muse token + TTS fields) has a different size and version,
     * so it falls through to the wipe below: the device asks for a fresh
     * Octop setup instead of half-trusting leftover values. */
    /* A v1 record (Muse token + TTS fields) has a different size and version,
     * so it falls through to the wipe below: the device asks for a fresh
     * Octop setup instead of half-trusting leftover values. */
    companion_credentials_config_t found=snapshot_of(&credentials);
    if(size!=sizeof(credentials)||credentials.version!=CREDENTIALS_VERSION||
        !companion_credentials_valid(&found))
        memset(&credentials,0,sizeof(credentials));
    return ESP_OK;
}
static void store(char *dst,size_t cap,const char *src)
{
    size_t n=strnlen(src,cap-1);
    memcpy(dst,src,n);dst[n]='\0';
}
esp_err_t companion_credentials_save(const companion_credentials_config_t *c)
{
    if(!companion_credentials_valid(c))return ESP_ERR_INVALID_ARG;
    credentials_t *next=calloc(1,sizeof(*next));
    if(!next)return ESP_ERR_NO_MEM;
    next->version=CREDENTIALS_VERSION;
    store(next->url,sizeof(next->url),c->octop_url);
    store(next->user,sizeof(next->user),c->username);
    store(next->pass,sizeof(next->pass),c->password);
    if(c->stt_url)store(next->stt_url,sizeof(next->stt_url),c->stt_url);
    if(c->stt_key)store(next->stt_key,sizeof(next->stt_key),c->stt_key);
    nvs_handle_t h;
    esp_err_t err=nvs_open("companion_keys",NVS_READWRITE,&h);
    if(err==ESP_OK){
        err=nvs_set_blob(h,"tokens",next,sizeof(*next));
        if(err==ESP_OK)err=nvs_commit(h);
        nvs_close(h);
    }
    /* Live readers keep the old snapshot. New values are loaded after reboot. */
    memset(next,0,sizeof(*next));free(next);
    return err;
}
const char *companion_credentials_octop_url(void)
{return companion_credentials_ready()?credentials.url:NULL;}
const char *companion_credentials_username(void)
{return companion_credentials_ready()?credentials.user:NULL;}
const char *companion_credentials_password(void)
{return companion_credentials_ready()?credentials.pass:NULL;}
const char *companion_credentials_stt_url(void)
{return companion_credentials_ready()?credentials.stt_url:NULL;}
const char *companion_credentials_stt_key(void)
{return companion_credentials_ready()?credentials.stt_key:NULL;}
