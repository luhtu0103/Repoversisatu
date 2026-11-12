#include "speaker_id.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SPEAKER_ID";
static constexpr int SAMPLE_RATE = 16000;
static constexpr int FFT_N = 512;
static constexpr int FRAME_N = 400;
static constexpr int HOP_N = 160;
static constexpr int MFCC_N = 13;
static constexpr int MEL_N = 26;
static constexpr int SEQ_N = 160;
static constexpr int PROFILE_N = 4;
static constexpr float PI_F = 3.14159265358979323846f;
typedef struct { uint32_t magic; uint16_t count; char name[SPEAKER_ID_NAME_MAX]; int16_t feat[SEQ_N][MFCC_N]; } profile_t;
static constexpr uint32_t PROFILE_MAGIC = 0x53504944U;
static profile_t profiles[PROFILE_N];
static size_t profile_count = 0;
static int16_t frame_buf[FRAME_N];
static size_t frame_fill = 0;
static int16_t sample_tail[FRAME_N];
static size_t tail_count = 0;
static int16_t ring_feat[SEQ_N][MFCC_N];
static size_t ring_write = 0, ring_count = 0;
static size_t silence_samples = 0;
static bool was_speech = false;
static bool initialized = false;
static float re[FFT_N], im[FFT_N], mel_edges[MEL_N + 2];

static float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float mel_to_hz(float mel) { return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f); }
static void fft512(void) {
    for (int i=1,j=0;i<FFT_N;i++) { int bit=FFT_N>>1; for(;j&bit;bit>>=1) j^=bit; j^=bit; if(i<j){float t=re[i];re[i]=re[j];re[j]=t;t=im[i];im[i]=im[j];im[j]=t;} }
    for(int len=2;len<=FFT_N;len<<=1){float angle=-2.0f*PI_F/len;float wr0=cosf(angle),wi0=sinf(angle);for(int i=0;i<FFT_N;i+=len){float wr=1,wi=0;for(int j=0;j<len/2;j++){int a=i+j,b=a+len/2;float tr=wr*re[b]-wi*im[b],ti=wr*im[b]+wi*re[b];re[b]=re[a]-tr;im[b]=im[a]-ti;re[a]+=tr;im[a]+=ti;float nw=wr*wr0-wi*wi0;wi=wr*wi0+wi*wr0;wr=nw;}}}
}
static void compute_mfcc(const int16_t *pcm, int16_t out[MFCC_N]) {
    for(int i=0;i<FFT_N;i++){if(i<FRAME_N){float w=0.54f-0.46f*cosf(2.0f*PI_F*i/(FRAME_N-1));re[i]=(float)pcm[i]*w/32768.0f;}else re[i]=0;im[i]=0;}
    fft512(); float power[FFT_N/2+1]; for(int i=0;i<=FFT_N/2;i++) power[i]=(re[i]*re[i]+im[i]*im[i]);
    float mel[MEL_N]; for(int m=0;m<MEL_N;m++){float sum=0;float left=mel_edges[m],center=mel_edges[m+1],right=mel_edges[m+2];for(int k=0;k<=FFT_N/2;k++){float hz=(float)k*SAMPLE_RATE/FFT_N;if(hz>=left&&hz<=center&&center>left)sum+=power[k]*(hz-left)/(center-left);else if(hz>center&&hz<=right&&right>center)sum+=power[k]*(right-hz)/(right-center);}mel[m]=logf(sum+1e-10f);}
    for(int c=0;c<MFCC_N;c++){float sum=0;for(int m=0;m<MEL_N;m++)sum+=mel[m]*cosf(PI_F*c*(m+0.5f)/MEL_N);float scaled=sum*100.0f;if(scaled>30000)scaled=30000;if(scaled< -30000)scaled=-30000;out[c]=(int16_t)scaled;}
}
static void push_feature(const int16_t *pcm) { int16_t f[MFCC_N];compute_mfcc(pcm,f);memcpy(ring_feat[ring_write],f,sizeof(f));ring_write=(ring_write+1)%SEQ_N;if(ring_count<SEQ_N)ring_count++; }
static void load_profiles(void) {
    nvs_handle_t h; if(nvs_open("speaker_id",NVS_READONLY,&h)!=ESP_OK)return;
    profile_count=0; for(int i=0;i<PROFILE_N;i++){char key[8];snprintf(key,sizeof(key),"p%d",i);size_t len=sizeof(profile_t);profile_t p={};if(nvs_get_blob(h,key,&p,&len)==ESP_OK&&len==sizeof(p)&&p.magic==PROFILE_MAGIC&&p.count>0&&p.count<=SEQ_N&&p.name[0])profiles[profile_count++]=p;}
    nvs_close(h); ESP_LOGI(TAG,"Profil suara dimuat dari NVS: %u",(unsigned)profile_count);
}
extern "C" void speaker_id_init(void) {
    if(initialized)return; initialized=true;
    for(int i=0;i<MEL_N+2;i++){float mel=hz_to_mel((float)i*hz_to_mel(SAMPLE_RATE/2)/(MEL_N+1));mel_edges[i]=mel_to_hz(mel);}
    load_profiles();
}
extern "C" void speaker_id_process_pcm(const int16_t *pcm,size_t samples) {
    if(!pcm||!samples)return;if(!initialized)speaker_id_init();
    uint64_t energy=0;for(size_t i=0;i<samples;i++){int32_t v=pcm[i];energy+=(uint64_t)(v*v);}
    float rms=sqrtf((float)energy/(float)samples);
    if(rms<220.0f){silence_samples+=samples;if(silence_samples>2400)was_speech=false;return;}
    if(!was_speech && silence_samples>2400){ring_count=0;ring_write=0;frame_fill=0;}
    was_speech=true;silence_samples=0;
    for(size_t i=0;i<samples;i++){int16_t s=pcm[i];frame_buf[frame_fill++]=s;if(frame_fill==FRAME_N){push_feature(frame_buf);memmove(frame_buf,frame_buf+HOP_N,(FRAME_N-HOP_N)*sizeof(int16_t));frame_fill=FRAME_N-HOP_N;}}
}
static void copy_recent(int16_t dst[SEQ_N][MFCC_N],size_t *n) {
    *n=ring_count;size_t start=(ring_write+SEQ_N-ring_count)%SEQ_N;for(size_t i=0;i<ring_count;i++)memcpy(dst[i],ring_feat[(start+i)%SEQ_N],sizeof(dst[i]));
}
extern "C" bool speaker_id_enroll_last(const char *name,char *result,size_t result_len) {
    if(result&&result_len)result[0]=0;if(!name||!name[0]||strlen(name)>=SPEAKER_ID_NAME_MAX){if(result&&result_len)snprintf(result,result_len,"Nama profil tidak valid (maksimum %d karakter).",SPEAKER_ID_NAME_MAX-1);return false;}
    if(!initialized)speaker_id_init();if(ring_count<45){if(result&&result_len)snprintf(result,result_len,"Rekaman terlalu singkat; ucapkan nama dan kalimat selama sekitar 2 detik, lalu daftar ulang.");return false;}
    int idx=-1;for(size_t i=0;i<profile_count;i++)if(strcmp(profiles[i].name,name)==0){idx=(int)i;break;}if(idx<0){if(profile_count>=PROFILE_N){if(result&&result_len)snprintf(result,result_len,"Penyimpanan penuh; maksimum %d profil suara.",PROFILE_N);return false;}idx=(int)profile_count;}
    profile_t p={};p.magic=PROFILE_MAGIC;strlcpy(p.name,name,sizeof(p.name));size_t n=0;copy_recent(p.feat,&n);p.count=(uint16_t)n;
    nvs_handle_t h;if(nvs_open("speaker_id",NVS_READWRITE,&h)!=ESP_OK){if(result&&result_len)snprintf(result,result_len,"NVS tidak bisa dibuka.");return false;}
    char key[8];snprintf(key,sizeof(key),"p%d",idx);esp_err_t e=nvs_set_blob(h,key,&p,sizeof(p));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);
    if(e!=ESP_OK){if(result&&result_len)snprintf(result,result_len,"Gagal menyimpan profil ke flash NVS (kode %d).",(int)e);return false;}
    profiles[idx]=p;if((size_t)(idx+1)>profile_count)profile_count=(size_t)idx+1;
    if(result&&result_len)snprintf(result,result_len,"Profil suara '%s' berhasil disimpan di NVS.",name);ESP_LOGI(TAG,"Profil tersimpan: %s, frames=%u",name,(unsigned)p.count);return true;
}
static float dtw_distance(const int16_t a[SEQ_N][MFCC_N],size_t na,const int16_t b[SEQ_N][MFCC_N],size_t nb) {
    float prev[SEQ_N+1],cur[SEQ_N+1];for(int j=0;j<=SEQ_N;j++)prev[j]=1e30f;prev[0]=0;
    for(size_t i=1;i<=na;i++){cur[0]=1e30f;for(size_t j=1;j<=nb;j++){float d=0;for(int c=1;c<MFCC_N;c++){float x=(float)a[i-1][c]-(float)b[j-1][c];d+=x*x;}d=sqrtf(d/(MFCC_N-1));float best=prev[j];if(cur[j-1]<best)best=cur[j-1];if(prev[j-1]<best)best=prev[j-1];cur[j]=d+best;}memcpy(prev,cur,sizeof(prev));}
    return prev[nb]/(float)(na+nb);
}
extern "C" bool speaker_id_identify(char *name,size_t name_len,float *distance_out) {
    if(name&&name_len)name[0]=0;if(distance_out)*distance_out=0;if(!initialized)speaker_id_init();
    if(ring_count<45||profile_count==0)return false;int16_t current[SEQ_N][MFCC_N];size_t n=0;copy_recent(current,&n);float best=1e30f;int winner=-1;
    /* Compare every second frame to bound DTW cost on the ESP32-S3. */
    int16_t down[80][MFCC_N];size_t dn=0;for(size_t i=0;i<n;i+=2){memcpy(down[dn++],current[i],sizeof(down[0]));}
    for(size_t p=0;p<profile_count;p++){int16_t ref[80][MFCC_N];size_t rn=0;for(size_t i=0;i<profiles[p].count;i+=2){if(rn<80)memcpy(ref[rn++],profiles[p].feat[i],sizeof(ref[0]));}
        if(rn<10||dn<10)continue;float d=dtw_distance((const int16_t (*)[MFCC_N])down,dn,(const int16_t (*)[MFCC_N])ref,rn);if(d<best){best=d;winner=(int)p;}}
    if(distance_out)*distance_out=best;
    /* Conservative initial threshold; must be calibrated using real recordings. */
    if(winner<0||best>1800.0f)return false;if(name&&name_len)strlcpy(name,profiles[winner].name,name_len);return true;
}
extern "C" size_t speaker_id_profile_count(void){if(!initialized)speaker_id_init();return profile_count;}
