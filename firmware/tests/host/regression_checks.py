"""Compile current firmware C functions with host transport/codec/storage stubs.
The assertions exercise production function bodies; this is not an ESP-IDF or
hardware simulation. Generated fixtures and binaries go to firmware/tests/host/out/
"""
from pathlib import Path
import json
import os
import re
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[3]
# 输出目录（编译产物与结果 JSON，不入库）；可用 SC_HOST_TEST_OUT 覆盖
OUT = ROOT / os.environ.get('SC_HOST_TEST_OUT', 'firmware/tests/host/out')
OUT.mkdir(parents=True, exist_ok=True)
(OUT/'tmp').mkdir(exist_ok=True)
shutil.rmtree(OUT/'tmp/storage', ignore_errors=True)


def function(path, signature):
    source = (ROOT/path).read_text()
    start = source.index(signature)
    # Preserve positions while masking comments and literals for brace matching.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ' ' * len(m.group()), source, flags=re.S)
    begin = masked.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[start:end] + '\n'

COMMON = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "cJSON.h"
typedef int esp_err_t;
enum {ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_RESPONSE=2, ESP_ERR_INVALID_STATE=3,
ESP_ERR_NOT_FOUND=4, ESP_ERR_INVALID_SIZE=5, ESP_ERR_NO_MEM=6, ESP_ERR_INVALID_CRC=7,
ESP_ERR_NOT_SUPPORTED=8,ESP_ERR_INVALID_ARG=9};
static size_t copy_str(char *d,const char *s,size_t n) {
 size_t len=strlen(s); if(n){size_t k=len<n-1?len:n-1;memcpy(d,s,k);d[k]=0;}return len;
}
#define strlcpy copy_str
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr,...) do {int err=(expr);if(err)return err;}while(0)
'''

API = r'''
typedef struct { char job_id[48], operation_id[33], status[24], stage[24], error_code[32];
 int attempt,revision,poll_after_ms;bool has_error,error_retryable; } api_job_status_t;
typedef void *esp_http_client_handle_t;
#define RESP_MAX (32*1024)
static int expected, available, offset, fail_read;
static bool chunked;
static char wire[RESP_MAX+16];
static int64_t esp_http_client_fetch_headers(void *h){return expected;}
static int esp_http_client_get_status_code(void *h){return 200;}
static int64_t esp_http_client_get_content_length(void *h){return expected;}
static bool esp_http_client_is_chunked_response(void *h){return chunked;}
static bool esp_http_client_is_complete_data_received(void *h){return offset==available && (chunked || offset==expected);}
static int esp_http_client_read(void *h,char *dst,int cap){
 if(fail_read && offset>=available)return -1;
 int n=available-offset; if(n>cap)n=cap; if(n>3)n=3;
 memcpy(dst,wire+offset,n);offset+=n;return n;
}
'''
api_path = Path('firmware/components/api_client/src/api_client.c')
API += function(api_path, 'static esp_err_t fetch_body(')
API += function(api_path, 'static esp_err_t parse_status_body(')
API += function(api_path, 'static esp_err_t classify_response(')
API += r'''
int main(void) {
 api_job_status_t out; char *body=NULL; int http=0;
 assert(parse_status_body("{\"error\":{\"code\":\"DEVICE_BUSY\",\"retryable\":true}}",&out)==ESP_OK);
 assert(classify_response(409,"",&out,ESP_OK)==ESP_ERR_INVALID_STATE);
 assert(!strcmp(out.error_code,"DEVICE_BUSY") && out.error_retryable);
 strcpy(out.error_code,"QUOTA_EXCEEDED");
 assert(classify_response(429,"",&out,ESP_OK)==ESP_ERR_INVALID_STATE);
 assert(!strcmp(out.error_code,"QUOTA_EXCEEDED"));
 memset(&out,0,sizeof(out)); assert(classify_response(200,"{}",&out,ESP_OK)!=ESP_OK);
 strcpy(out.job_id,"job");strcpy(out.operation_id,"op");strcpy(out.status,"running");out.attempt=1;out.revision=2;
 assert(classify_response(202,"",&out,ESP_OK)==ESP_OK);
 assert(classify_response(401,"",&out,ESP_ERR_INVALID_RESPONSE)==ESP_ERR_INVALID_STATE);
 assert(!strcmp(out.error_code,"AUTH_FAILED"));
 strcpy(wire,"{\"ok\":true}");expected=available=strlen(wire);offset=0;
 assert(fetch_body(NULL,&body,&http)==ESP_OK && !strcmp(body,wire));free(body);
 available=expected-2;offset=0;
 assert(fetch_body(NULL,&body,&http)!=ESP_OK && !body);
 expected=RESP_MAX+1;available=0;offset=0;
 assert(fetch_body(NULL,&body,&http)==ESP_ERR_INVALID_SIZE);
 expected=-1;chunked=true;available=12;offset=0;memset(wire,'x',12);
 assert(fetch_body(NULL,&body,&http)==ESP_OK && strlen(body)==12);free(body);
 available=RESP_MAX+1;offset=0;memset(wire,'x',available);
 assert(fetch_body(NULL,&body,&http)==ESP_ERR_INVALID_SIZE);
 available=0;offset=0;fail_read=1;
 assert(fetch_body(NULL,&body,&http)!=ESP_OK);
 puts("HTTP: parsed errors preserved; success fields required; short/chunked/oversize/failed reads checked");
}
'''

STORAGE = r'''
#define STORAGE_AUDIO_MAX_BYTES (2*1024*1024)
#define STORAGE_MAX_VERSIONS 10
typedef struct {uint32_t generation,capture_seq;int current_version;char versions_csv[64];} meta_slot_t;
static meta_slot_t a={.generation=1,.capture_seq=1,.current_version=1,.versions_csv="1"};
static meta_slot_t b={.generation=2,.capture_seq=1,.current_version=2,.versions_csv="1,2"};
static bool deleting;
static unsigned hash_calls;
static const char *base="tmp/storage";
static void story_dir(char *dst,size_t n,const char *sid){snprintf(dst,n,"%s",base);}
static void join2(char *d,size_t n,const char *a,const char *b){snprintf(d,n,"%s/%s",a,b);}
static void join3(char *d,size_t n,const char *a,const char *b,const char *c){snprintf(d,n,"%s/%s/%s",a,b,c);}
static bool file_exists(const char *p){struct stat st;return !stat(p,&st)&&S_ISREG(st.st_mode);}
static bool read_file_cap(const char *p,uint8_t *buf,size_t cap,size_t *len){
 FILE *f=fopen(p,"rb");if(!f)return false;*len=fread(buf,1,cap,f);int extra=fgetc(f);fclose(f);return extra==EOF;
}
static int storage_file_sha256(const char *p,char *hex){
 ++hash_calls;
 FILE *f=fopen(p,"rb");if(!f)return ESP_FAIL;int first=fgetc(f);fclose(f);
 if(first==EOF)return ESP_FAIL;memset(hex,first,64);hex[64]=0;return ESP_OK;
}
static bool storage_has_delete_intent(const char *s){return deleting;}
static int read_metadata_slot(const char *s,char slot,meta_slot_t *out){*out=slot=='a'?a:b;return ESP_OK;}
static int write_metadata_slot(const char *s,char slot,uint32_t seq,int ver,uint32_t gen,const char *csv){
 meta_slot_t *out=slot=='a'?&a:&b;out->generation=gen;out->current_version=ver;out->capture_seq=seq;strlcpy(out->versions_csv,csv,64);return ESP_OK;
}
static int atomic_write(const char *p,const uint8_t *data,size_t len,size_t cap){
 FILE *f=fopen(p,"wb");if(!f)return ESP_FAIL;bool ok=fwrite(data,1,len,f)==len;fclose(f);return ok?ESP_OK:ESP_FAIL;
}
static int ensure_dir(const char *p){return !mkdir(p,0775)||errno==EEXIST?ESP_OK:ESP_FAIL;}
static int storage_stage_discard(const char *s,const char *op){return ESP_OK;}
'''
storage_path = Path('firmware/components/storage_service/src/storage_service.c')
for signature in ['static bool version_complete(', 'static bool read_valid_metadata(', 'esp_err_t storage_get_current_version(',
                  'static void csv_append(', 'esp_err_t storage_commit_version(', 'esp_err_t storage_get_playback_info(']:
    # version_complete has a forward declaration: select the definition.
    if signature == 'static bool version_complete(':
        signature = 'static bool version_complete(const char *story_id, int version)\n{'
    STORAGE += function(storage_path, signature)
STORAGE += r'''
static void write_byte(const char *p,char v){FILE *f=fopen(p,"wb");assert(f);fputc(v,f);fclose(f);}
static void manifest(int version,const char *op){
 char p[128];snprintf(p,sizeof(p),"%s/versions/%d/version.json",base,version);
 FILE *f=fopen(p,"wb");assert(f);char ah[65],sh[65];memset(ah,'a',64);ah[64]=0;memset(sh,'b',64);sh[64]=0;
 fprintf(f,"{\"audio_bytes\":1,\"audio_sha256\":\"%s\",\"story_sha256\":\"%s\",\"duration_ms\":1000,\"operation_id\":\"%s\"}",ah,sh,op);fclose(f);
}
int main(void){
 mkdir(base,0775);mkdir("tmp/storage/versions",0775);mkdir("tmp/storage/versions/1",0775);mkdir("tmp/storage/versions/2",0775);
 write_byte("tmp/storage/versions/1/audio.mp3",'a');write_byte("tmp/storage/versions/1/story.txt",'b');manifest(1,"old");
 write_byte("tmp/storage/versions/2/audio.mp3",'c');write_byte("tmp/storage/versions/2/story.txt",'b');manifest(2,"orphan");
 assert(storage_get_current_version("id")==1); /* Newer same-size corruption falls back. */
 int version=0;assert(storage_commit_version("id","old","title",1,1000,&version)==ESP_OK&&version==1);
 mkdir("tmp/storage/staging",0775);mkdir("tmp/storage/staging/new",0775);
 write_byte("tmp/storage/staging/new/audio.mp3",'a');write_byte("tmp/storage/staging/new/story.txt",'b');
 assert(storage_commit_version("id","new","title",1,1000,&version)==ESP_OK&&version==3);
 assert(storage_get_current_version("id")==3); /* Orphan v2 was preserved. */
 char audio_path[256];uint32_t duration=0;hash_calls=0;
 assert(storage_get_playback_info("id",audio_path,sizeof(audio_path),&duration)==ESP_OK);
 assert(duration==1000 && strstr(audio_path,"/3/audio.mp3") && hash_calls==2);
 assert(storage_commit_version("id","new","title",1,1000,&version)==ESP_OK&&version==3);
 write_byte("tmp/storage/versions/3/audio.mp3",'c');assert(storage_get_current_version("id")==1);
 write_byte("tmp/storage/versions/1/audio.mp3",'c');assert(storage_get_current_version("id")<0);
 deleting=true;assert(!read_valid_metadata("id",&a));
 puts("Storage: same-size corruption, old-slot fallback, negative errors, idempotent commits, orphan preservation, tombstones checked");
}
'''

AUDIO = r'''
#define PCM_BYTES (1152*2*2*2)
#define ESP_AUDIO_ERR_OK 0
#define ESP_CODEC_DEV_OK 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static int s_lock;static uint64_t s_samples;static uint32_t s_total_ms;static uint8_t s_volume=30;static uint32_t s_serial=1;static uint16_t s_pcm_peak;
typedef void *esp_codec_dev_handle_t;typedef void *esp_audio_simple_dec_handle_t;
typedef struct {uint32_t sample_rate;uint8_t channel,bits_per_sample;} esp_audio_simple_dec_info_t;
static esp_audio_simple_dec_info_t info={24000,1,16};static int write_status;static int volume_status;static int calls;
static bool checkpoint(uint32_t serial){return serial==s_serial;}
static int esp_audio_simple_dec_get_info(void *d,esp_audio_simple_dec_info_t *out){*out=info;return 0;}
static int esp_codec_dev_set_out_vol(void *d,int volume){return volume_status;}
static int esp_codec_dev_write(void *d,void *data,int len){calls++;return write_status;}
'''
audio_path=Path('firmware/components/audio_service/src/audio_service.c')
AUDIO+=function(audio_path,'static bool output_pcm(')
AUDIO+=function(audio_path,'int audio_progress_permille(')
AUDIO+=r'''
int main(void){
 uint8_t pcm[PCM_BYTES]={0};s_total_ms=1000;
 assert(output_pcm(1,NULL,NULL,pcm,2400));assert(calls==5 && s_samples==1200);
 assert(audio_progress_permille()==50);
 volume_status=1;int before=calls;assert(!output_pcm(1,NULL,NULL,pcm,100));assert(calls==before && s_samples==1200);
 volume_status=0;
 write_status=1;assert(!output_pcm(1,NULL,NULL,pcm,100));assert(s_samples==1200);
 write_status=-1;assert(!output_pcm(1,NULL,NULL,pcm,100));
 write_status=0;info.channel=2;assert(!output_pcm(1,NULL,NULL,pcm,100));
 info.channel=1;info.sample_rate=16000;assert(!output_pcm(1,NULL,NULL,pcm,100));
 info.sample_rate=24000;assert(!output_pcm(1,NULL,NULL,pcm,PCM_BYTES));
 assert(output_pcm(2,NULL,NULL,pcm,100));assert(s_samples==1200);
 s_samples=24000;assert(audio_progress_permille()==1000);s_total_ms=0;assert(audio_progress_permille()==0);
 puts("Audio: status codes, format/buffer limits, cancelled output and sample progress checked");
}
'''

SETTINGS = r'''
#define ESP_ERR_NVS_NOT_FOUND 100
#define ESP_ERR_TIMEOUT 101
#define NVS_READWRITE 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef int nvs_handle_t;
static int mutex=1,held,commits,read_error,commit_error;
static void *s_seq_lock=&mutex;
static const char *NS="storybox", *K_SEQ="cap_seq";
static uint32_t stored,staged;
static bool exists;
static int xSemaphoreTake(void *s,int ticks){assert(!held);held=1;return pdTRUE;}
static void xSemaphoreGive(void *s){assert(held);held=0;}
static int nvs_open(const char *ns,int mode,nvs_handle_t *h){*h=1;staged=stored;return 0;}
static int nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *out){
 if(read_error)return ESP_FAIL;if(!exists)return ESP_ERR_NVS_NOT_FOUND;*out=stored;return 0;
}
static int nvs_set_u32(nvs_handle_t h,const char *key,uint32_t value){staged=value;return 0;}
static int nvs_commit(nvs_handle_t h){commits++;if(commit_error)return ESP_FAIL;stored=staged;exists=true;return 0;}
static void nvs_close(nvs_handle_t h){}
'''
settings_path=Path('firmware/components/settings_service/src/settings_service.c')
SETTINGS+=function(settings_path,'esp_err_t settings_reserve_capture_seq(')
SETTINGS+=function(settings_path,'esp_err_t settings_ensure_capture_seq(')
SETTINGS+=r'''
int main(void){
 uint32_t first=999;
 assert(settings_ensure_capture_seq(1000)==ESP_OK && stored==1001);
 assert(settings_reserve_capture_seq(3,&first)==ESP_OK && first==1001 && stored==1004);
 int before=commits;
 assert(settings_ensure_capture_seq(20)==ESP_OK && stored==1004 && commits==before);
 commit_error=1;first=999;
 assert(settings_reserve_capture_seq(1,&first)==ESP_FAIL && stored==1004 && first==999 && !held);
 commit_error=0;read_error=1;
 assert(settings_ensure_capture_seq(2000)==ESP_FAIL && stored==1004 && !held);
 assert(settings_reserve_capture_seq(1,&first)==ESP_FAIL && first==999 && !held);
 read_error=0;
 assert(settings_reserve_capture_seq(1,&first)==ESP_OK && first==1004 && stored==1005);
 assert(settings_reserve_capture_seq(0,&first)==ESP_ERR_INVALID_ARG);
 assert(settings_reserve_capture_seq(1,NULL)==ESP_ERR_INVALID_ARG);
 stored=UINT32_MAX-1;assert(settings_reserve_capture_seq(2,&first)==ESP_ERR_NOT_SUPPORTED && !held);
 assert(settings_ensure_capture_seq(UINT32_MAX)==ESP_ERR_NOT_SUPPORTED && !held);
 puts("Settings: TF/NVS sequence reconciliation, monotonic reservations, commit/read failures and overflow checked");
}
'''

TOUCH = '#include "' + str(ROOT/'firmware/components/board_adapter/include/board_adapter.h') + '"\n'
TOUCH += r'''
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_STATE_RELEASED 0
typedef int lv_indev_t;
typedef struct {struct {int x,y;} point;int state;bool continue_reading;} lv_indev_data_t;
static void *s_tp=(void *)1;
static int s_touch_lock;
static board_touch_status_t s_touch_status;
static bool s_touch_trace;
static uint32_t s_touch_trace_ms;
static int read_error,reads;
static bool wire_touched;
static uint16_t wire_x,wire_y;
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(void){return clock_us;}
static int esp_lcd_touch_read_data(void *tp){reads++;return read_error;}
static bool esp_lcd_touch_get_coordinates(void *tp,uint16_t *x,uint16_t *y,
 void *strength,uint8_t *cnt,int max){
 *cnt=wire_touched?1:0;*x=wire_x;*y=wire_y;return wire_touched;
}
'''
touch_path=Path('firmware/components/board_adapter/src/board_adapter.c')
_tsrc=(ROOT/'firmware/components/board_adapter/src/board_adapter.c').read_text()
_a=_tsrc.index('#define TOUCH_EVENT_SLOTS');_b=_tsrc.index('static void touch_event_push(')
TOUCH += _tsrc[_a:_b]
for signature in ['static bool touch_map_native_to_display(',
                  'board_touch_status_t board_adapter_touch_snapshot(',
                  'bool board_adapter_touch_poll(',
                  'void board_adapter_touch_trace(',
                  'static void touch_event_push(',
                  'static bool touch_event_pop(',
                  'static void touch_sample(',
                  'static void touch_read_cb(']:
    TOUCH += function(touch_path,signature)
TOUCH += r'''
static void read_once(lv_indev_data_t *d){touch_sample();touch_read_cb(NULL,d);}
int main(void){
 uint16_t x=999,y=999;
 assert(touch_map_native_to_display(0,0,&x,&y) && x==479 && y==0);
 assert(touch_map_native_to_display(319,0,&x,&y) && x==479 && y==319);
 assert(touch_map_native_to_display(0,479,&x,&y) && x==0 && y==0);
 assert(touch_map_native_to_display(319,479,&x,&y) && x==0 && y==319);
 assert(touch_map_native_to_display(159,239,&x,&y) && x==240 && y==159);
 x=y=999;assert(!touch_map_native_to_display(320,0,&x,&y) && x==999 && y==999);
 assert(!touch_map_native_to_display(0,480,&x,&y));
 assert(!touch_map_native_to_display(UINT16_MAX,UINT16_MAX,&x,&y));
 assert(!touch_map_native_to_display(0,0,NULL,&y));
 assert(!touch_map_native_to_display(0,0,&x,NULL));
 for(unsigned a=0;a<320;a++)for(unsigned b=0;b<480;b++){
   assert(touch_map_native_to_display(a,b,&x,&y) && x<480 && y<320);
 }
 lv_indev_data_t data={0};wire_touched=true;wire_x=319;wire_y=479;
 read_once(&data);
 assert(data.state==LV_INDEV_STATE_PRESSED && data.point.x==0 && data.point.y==319);
 int before=reads;assert(board_adapter_touch_poll(&x,&y) && x==0 && y==319);
 board_touch_status_t state=board_adapter_touch_snapshot();
 assert(state.pressed && state.polls==1 && reads==before);
 board_adapter_touch_trace(true);assert(s_touch_trace);
 read_error=ESP_FAIL;clock_us+=30000;read_once(&data);
 assert(data.state==LV_INDEV_STATE_RELEASED && s_touch_status.read_errors==1);
 assert(s_touch_status.last_error==ESP_FAIL && !board_adapter_touch_poll(&x,&y));
 read_error=0;wire_x=320;read_once(&data);
 assert(data.state==LV_INDEV_STATE_RELEASED && s_touch_status.invalid_points==1);
 wire_x=159;wire_y=239;read_once(&data);
 assert(data.state==LV_INDEV_STATE_PRESSED && data.point.x==240 && data.point.y==159);
 wire_touched=false;read_once(&data);
 assert(data.state==LV_INDEV_STATE_RELEASED && data.point.x==240 && data.point.y==159);
 board_adapter_touch_trace(false);assert(!s_touch_trace);
  /* 两次 LVGL 读取之间完成的整次短按：按序回放 按下→抬起，不丢 */
 wire_touched=true;wire_x=100;wire_y=100;touch_sample();wire_touched=false;touch_sample();
 touch_read_cb(NULL,&data);assert(data.state==LV_INDEV_STATE_PRESSED && data.continue_reading);
 touch_read_cb(NULL,&data);assert(data.state==LV_INDEV_STATE_RELEASED && !data.continue_reading);
 touch_read_cb(NULL,&data);assert(data.state==LV_INDEV_STATE_RELEASED);
 /* 队列满丢最旧，保留最新转换 */
 for(int i=0;i<40;i++){wire_touched=(i%2)==0;touch_sample();}
 int n=0;bool last=true;while(touch_event_pop(&(touch_event_t){0},&last)) n++;
 assert(n==TOUCH_EVENT_SLOTS);
 puts("Touch: queued taps between reads, all native pixels in range, corner/center mapping, valid zero point, invalid/error release and non-consuming diagnostics checked");
}
'''

# FatFs 语义：f_rename 目标已存在时返回 FR_EXIST（不覆盖）。用真实
# atomic_write/read_file_cap 函数体验证覆盖写与断电残留 .bak 的恢复。
FATFS = r'''
static int fat_rename(const char *src,const char *dst){
 struct stat st;if(!stat(dst,&st)){errno=EEXIST;return -1;}return rename(src,dst);
}
#define rename fat_rename
'''
for signature in ['static esp_err_t atomic_write(', 'static bool read_file_cap(']:
    FATFS += function(storage_path, signature)
FATFS += r'''
static void slurp(const char *p,char *out){size_t n=0;assert(read_file_cap(p,(uint8_t*)out,63,&n));out[n]=0;}
int main(void){
 mkdir("tmp",0775);mkdir("tmp/fatfs",0775);
 const char *p="tmp/fatfs/op.json";char buf[64];
 unlink(p);
 assert(atomic_write(p,(const uint8_t*)"one",3,64)==ESP_OK);slurp(p,buf);assert(!strcmp(buf,"one"));
 assert(atomic_write(p,(const uint8_t*)"two",3,64)==ESP_OK);slurp(p,buf);assert(!strcmp(buf,"two"));
 assert(atomic_write(p,(const uint8_t*)"three",5,64)==ESP_OK);slurp(p,buf);assert(!strcmp(buf,"three"));
 struct stat st;assert(stat("tmp/fatfs/op.json.bak",&st)!=0&&stat("tmp/fatfs/op.json.part",&st)!=0);
 /* 两次 rename 之间断电：目标缺失、.bak 为旧内容、.part 为新内容 → 读回旧内容 */
 assert(fat_rename(p,"tmp/fatfs/op.json.bak")==0);
 FILE *f=fopen("tmp/fatfs/op.json.part","wb");fputs("four",f);fclose(f);
 slurp(p,buf);assert(!strcmp(buf,"three"));
 assert(atomic_write(p,(const uint8_t*)"five",4,64)==ESP_OK);slurp(p,buf);assert(!strcmp(buf,"five"));
 assert(atomic_write(p,(const uint8_t*)"x",0,64)==ESP_ERR_INVALID_SIZE);
 puts("FatFs: non-overwriting rename, repeated overwrite, interrupted-replace .bak recovery checked");
}
'''

results={}
for name,code in [('http',API),('storage',STORAGE),('fatfs',FATFS),('audio',AUDIO),('settings',SETTINGS),('touch',TOUCH)]:
    source=OUT/f'{name}_regression.c'
    binary=OUT/'tmp'/f'{name}_regression'
    source.write_text(COMMON+code)
    cjson=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
    cmd=['cc','-std=c11','-Wno-unused-variable',f'-I{cjson}',str(source),str(cjson/'cJSON.c'),'-o',str(binary)]
    built=subprocess.run(cmd,capture_output=True,text=True)
    if built.returncode: raise RuntimeError(built.stderr)
    run=subprocess.run([str(binary)],cwd=OUT,capture_output=True,text=True)
    if run.returncode: raise RuntimeError(run.stdout+run.stderr)
    results[name]=run.stdout.strip()
(OUT/'firmware-host-tests.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(results,ensure_ascii=False,indent=2))
