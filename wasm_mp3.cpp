// WASM MP3 player core -- all in C/C++:
//   * MP3 decoding   : minimp3 (public domain, C)
//   * Two level displays, both drawn by C++ into an RGBA framebuffer that JS
//     blits to a <canvas>:
//       - VU  : analog needle + arc scale + red zone, RMS + needle ballistics,
//               with peak-hold marker and a clip LED per channel.
//       - LED : multi-band LED spectrum analyser (FFT -> log-spaced bands ->
//               green/yellow/red LED columns with floating peak dots).
//     JS toggles between them via meter_set_mode().
// JS only does what the browser requires: file input, Web Audio playback, blit.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"
#include <emscripten.h>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>

static mp3dec_file_info_t g_info;   // g_info.buffer = interleaved float PCM
static int   g_ch = 2, g_hz = 44100;
static int   g_mode = 0;             // 0 = VU meter, 1 = LED spectrum

// ---- VU state, per channel (normalised 0..1 meter units) ----
static float g_needle[2]   = {0.f, 0.f};
static float g_peak[2]     = {0.f, 0.f};   // held peak position of the needle
static float g_peakHold[2] = {0.f, 0.f};   // frames left before the peak marker falls
static float g_clip[2]     = {0.f, 0.f};   // clip-LED brightness (1 = just clipped, fades out)
static const float PEAK_HOLD_FRAMES = 48.f;  // ~0.8s hold at 60fps
static const float PEAK_FALL        = 0.010f;
static const float CLIP_FADE        = 0.94f;

// scale layout: 0 VU / red-zone start as a fraction of the arc sweep (0..1).
static const float RED = 0.76f;

// ---- LED spectrum state ----
static const int   NB = 20;          // number of frequency bands (LED columns)
static const int   FN = 2048;        // FFT size
static float g_band[NB]     = {0.f}; // smoothed level per band (0..1)
static float g_bandPeak[NB] = {0.f}; // floating peak dot per band
static float g_bandHold[NB] = {0.f};

// ---------------- framebuffer ----------------
static const int W = 570, Hh = 188;
static std::vector<uint8_t> FB;

static inline void px(int x,int y,uint8_t r,uint8_t g,uint8_t b,float a=1.f){
    if(x<0||y<0||x>=W||y>=Hh) return; int i=(y*W+x)*4;
    FB[i]  =(uint8_t)(FB[i]*(1-a)+r*a); FB[i+1]=(uint8_t)(FB[i+1]*(1-a)+g*a);
    FB[i+2]=(uint8_t)(FB[i+2]*(1-a)+b*a); FB[i+3]=255;
}
static void rect(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b){
    for(int j=0;j<h;++j) for(int i=0;i<w;++i) px(x+i,y+j,r,g,b);
}
static void disk(float cx,float cy,float rad,uint8_t r,uint8_t g,uint8_t b){
    int ri=(int)std::ceil(rad);
    for(int j=-ri;j<=ri;++j) for(int i=-ri;i<=ri;++i) if(i*i+j*j<=rad*rad) px((int)cx+i,(int)cy+j,r,g,b);
}
static void line(float x0,float y0,float x1,float y1,float th,uint8_t r,uint8_t g,uint8_t b){
    float dx=x1-x0, dy=y1-y0, len=std::sqrt(dx*dx+dy*dy); if(len<1e-3f) return;
    int n=(int)(len*2)+1;
    for(int k=0;k<=n;++k){ float t=(float)k/n; disk(x0+dx*t, y0+dy*t, th, r,g,b); }
}

// tiny 5x7 font for labels/scale
static const char* GK="0123456789-+VULRdB ";
static const uint8_t GLYPH[][7]={
 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
 {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
 {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
 {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
 {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},{0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
 {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
 {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
 {0x01,0x01,0x07,0x09,0x11,0x11,0x0F},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
 {0x00,0x00,0x00,0x00,0x00,0x00,0x00}};
static void ch(int x,int y,char c,int s,uint8_t r,uint8_t g,uint8_t b){
    int gi=-1; for(int k=0;GK[k];++k) if(GK[k]==c){gi=k;break;} if(gi<0) return;
    for(int row=0;row<7;++row) for(int col=0;col<5;++col)
        if(GLYPH[gi][row]&(1<<(4-col))) rect(x+col*s,y+row*s,s,s,r,g,b);
}
static void text(int x,int y,const char* str,int s,uint8_t r,uint8_t g,uint8_t b){
    for(int k=0;str[k];++k){ ch(x,y,str[k],s,r,g,b); x+=6*s; }
}

// draw one realistic analog VU meter into a WIDE landscape rectangle (x0,y0,w,h).
// The needle pivot sits BELOW the panel (hidden), so only the arc shows near the
// top and the needle emerges from the bottom edge -- the classic VU look.
// `peak` is the held peak position (0..1); `clip` is the clip-LED brightness.
static void draw_meter(int x0,int y0,int w,int h,float level,float peak,float clip,const char* label){
    // ivory face with a soft vertical gradient
    for(int j=0;j<h;++j){ int s=248-(j*22)/h; rect(x0,y0+j,w,1,(uint8_t)s,(uint8_t)(s-6),(uint8_t)(s-32)); }
    for(int t=0;t<4;++t){ uint8_t c=(uint8_t)(28+t*11);   // bezel
        rect(x0+t,y0+t,w-2*t,1,c,c,c); rect(x0+t,y0+h-1-t,w-2*t,1,c,c,c);
        rect(x0+t,y0+t,1,h-2*t,c,c,c); rect(x0+w-1-t,y0+t,1,h-2*t,c,c,c); }

    // geometry: ~96deg sweep; pivot sits just below the panel's bottom edge so the
    // needle is (almost) fully visible while its hub/centre stays hidden.
    const float m=14.f, topm=16.f;
    const float A=48.f*3.14159265f/180.f;                   // half sweep
    float half=w*0.5f-m;
    float R=half/std::sin(A);
    float cx=x0+w*0.5f, cy=y0+topm+R;                        // pivot just below bottom edge

    // solid scale arc, red from 0 VU to +3
    for(int k=0;k<=360;++k){ float t=k/360.f, th=-A+t*2*A; bool red=t>=RED-0.001f;
        for(float rr=R-1.3f; rr<=R+1.3f; rr+=0.7f)
            disk(cx+rr*std::sin(th), cy-rr*std::cos(th), 0.7f, red?190:32, red?28:30, red?28:28); }

    struct Tk{ float t; const char* s; };
    Tk major[]={{0.00f,"-20"},{0.25f,"-10"},{0.43f,"-5"},{0.55f,"-3"},{0.68f,"-1"},{RED,"0"},{0.88f,"+2"},{1.00f,"+3"}};
    float minor[]={0.12f,0.35f,0.49f,0.61f,0.72f,0.82f,0.94f};
    for(float t:minor){ float th=-A+t*2*A; bool red=t>=RED;
        line(cx+(R-9)*std::sin(th),cy-(R-9)*std::cos(th), cx+R*std::sin(th),cy-R*std::cos(th),0.8f,
             red?190:64,red?28:58,red?28:52); }
    for(auto&mk:major){ float th=-A+mk.t*2*A; bool red=mk.t>=RED-0.001f;
        uint8_t r=red?188:24,g=red?28:20,b=red?28:18;
        line(cx+(R-15)*std::sin(th),cy-(R-15)*std::cos(th), cx+R*std::sin(th),cy-R*std::cos(th),1.3f,r,g,b);
        int tw=(int)strlen(mk.s)*6;
        text((int)(cx+(R-26)*std::sin(th))-tw/2,(int)(cy-(R-26)*std::cos(th))-3,mk.s,1,r,g,b); }

    text(x0+11,y0+9,label,2, 55,50,45);                     // L / R (top-left)
    text(x0+w-30,y0+9,"VU",2, 55,50,45);                    // VU (top-right)

    // clip LED just under the VU text (dark red when idle, bright red when clipping)
    float cb=std::min(1.f,std::max(0.f,clip));
    int lx=x0+w-19, ly=y0+30;
    disk(lx,ly,5.2f, 20,14,14);                             // dark socket
    disk(lx,ly,4.0f, (uint8_t)(70+185*cb), (uint8_t)(18*(1-cb)+10), (uint8_t)(18*(1-cb)+10));

    // peak-hold marker: a short bright tick sitting on the arc at the peak angle
    float pk=std::min(1.f,std::max(0.f,peak));
    if(pk>0.02f){ float thp=-A+pk*2*A; bool pr=pk>=RED;
        line(cx+(R-4.5f)*std::sin(thp),cy-(R-4.5f)*std::cos(thp), cx+(R+2.5f)*std::sin(thp),cy-(R+2.5f)*std::cos(thp),1.5f,
             pr?255:250, pr?45:170, pr?45:35); }

    // needle: only the tip portion is visible; the pivot is hidden below the
    // panel, so we walk from the tip toward the pivot and stop at the panel edge
    // (tapered: thin at the tip, thicker toward the base).
    float lv=std::min(1.f,std::max(0.f,level)), th=-A+lv*2*A;
    float sn=std::sin(th), cs=std::cos(th);
    for(float r=R-3.f; r>0.f; r-=0.8f){
        float nx=cx+r*sn, ny=cy-r*cs;
        if(nx<x0+4||nx>x0+w-4||ny<y0+4){ continue; }
        if(ny>y0+h-4){ break; }                                  // exited the bottom edge
        float thick=0.8f + (R-r)/R*1.7f;                         // taper
        disk(nx,ny,thick, 15,15,18);
    }

    // faint glass highlight across the top
    int hb=(int)(h*0.34f);
    for(int j=0;j<hb;++j){ float a=0.09f*(1.f-(float)j/hb); for(int i=0;i<w-8;++i) px(x0+4+i,y0+4+j,255,255,255,a); }
}

// ---- iterative radix-2 FFT (in place, complex float) ----
static void fft(float* re,float* im,int n){
    for(int i=1,j=0;i<n;++i){ int bit=n>>1; for(; j&bit; bit>>=1) j^=bit; j^=bit;
        if(i<j){ std::swap(re[i],re[j]); std::swap(im[i],im[j]); } }
    for(int len=2; len<=n; len<<=1){
        float ang=-2.f*3.14159265358979f/len, wr=std::cos(ang), wi=std::sin(ang);
        for(int i=0;i<n;i+=len){ float cwr=1.f, cwi=0.f;
            for(int k=0;k<len/2;++k){ int a=i+k, b=a+len/2;
                float xr=re[b]*cwr-im[b]*cwi, xi=re[b]*cwi+im[b]*cwr;
                re[b]=re[a]-xr; im[b]=im[a]-xi; re[a]+=xr; im[a]+=xi;
                float nwr=cwr*wr-cwi*wi; cwi=cwr*wi+cwi*wr; cwr=nwr; } }
    }
}

// draw the LED spectrum: NB columns of green/yellow/red LED segments plus a
// floating peak dot on each column. Uses the smoothed g_band / g_bandPeak state.
static void draw_leds(){
    const int marginX=18, marginTop=16, marginBot=16;
    const int SEG=14;
    int gridW=W-2*marginX, gridH=Hh-marginTop-marginBot;
    float colW=(float)gridW/NB;
    float segGap=2.f, segH=(gridH-(SEG-1)*segGap)/SEG;
    for(int b=0;b<NB;++b){
        int cx0=marginX+(int)(b*colW);
        int cw=(int)colW-3; if(cw<3) cw=3;
        int lit    =(int)std::ceil(g_band[b]*SEG);
        int peakSeg=(int)std::ceil(g_bandPeak[b]*SEG);
        for(int s=0;s<SEG;++s){
            int yy=marginTop+gridH-(int)((s+1)*segH+s*segGap);
            float frac=(float)s/(SEG-1);
            uint8_t r,g,bl;
            if(frac>0.82f){ r=232; g=44;  bl=32; }        // red (top)
            else if(frac>0.58f){ r=236; g=200; bl=44; }   // yellow (upper-mid)
            else { r=42; g=212; bl=74; }                  // green (lower)
            bool on=s<lit, pk=(peakSeg>0 && s==peakSeg-1);
            if(on||pk) rect(cx0,yy,cw,(int)segH, r,g,bl);
            else       rect(cx0,yy,cw,(int)segH, (uint8_t)(r/7+7),(uint8_t)(g/7+7),(uint8_t)(bl/7+7));
        }
    }
    // small "dB" legend in the corner
    text(W-2*marginX-6, marginTop-2, "dB", 1, 120,120,130);
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int mp3_load(const uint8_t* data,int len){
    if(g_info.buffer){ free(g_info.buffer); std::memset(&g_info,0,sizeof(g_info)); }
    mp3dec_t dec;
    if(mp3dec_load_buf(&dec, data, (size_t)len, &g_info, 0, 0)) return 0;
    if(!g_info.samples) return 0;
    g_ch=g_info.channels>0?g_info.channels:2; g_hz=g_info.hz>0?g_info.hz:44100;
    g_needle[0]=g_needle[1]=0.f; g_peak[0]=g_peak[1]=0.f;
    g_peakHold[0]=g_peakHold[1]=0.f; g_clip[0]=g_clip[1]=0.f;
    for(int b=0;b<NB;++b){ g_band[b]=g_bandPeak[b]=g_bandHold[b]=0.f; }
    FB.assign((size_t)W*Hh*4, 0);
    return 1;
}
EMSCRIPTEN_KEEPALIVE float* pcm_ptr(){ return g_info.buffer; }
EMSCRIPTEN_KEEPALIVE int pcm_frames(){ return g_ch? (int)(g_info.samples/g_ch):0; }
EMSCRIPTEN_KEEPALIVE int pcm_channels(){ return g_ch; }
EMSCRIPTEN_KEEPALIVE int sample_rate(){ return g_hz; }
EMSCRIPTEN_KEEPALIVE void meter_set_mode(int m){ g_mode = m?1:0; }
EMSCRIPTEN_KEEPALIVE int  meter_mode(){ return g_mode; }

// compute levels ending at time `sec`: L/R RMS -> VU needle (+ peak/clip), and,
// in LED mode, an FFT-based log-band spectrum with per-band peak hold.
EMSCRIPTEN_KEEPALIVE
void meter_set_time(double sec){
    if(!g_info.buffer||!g_ch) return;
    int frames=pcm_frames();
    int idx=(int)(sec*g_hz); if(idx>frames) idx=frames;

    // --- VU needle (RMS over a 50ms window) ---
    int win=(int)(0.05*g_hz); int start=idx-win; if(start<0) start=0;
    for(int c=0;c<2;++c){
        int cc = c<g_ch? c : g_ch-1;
        double acc=0; int n=0;
        for(int f=start; f<idx; ++f){ float v=g_info.buffer[(size_t)f*g_ch+cc]; acc+=(double)v*v; ++n; }
        float rms = n? (float)std::sqrt(acc/n) : 0.f;
        float db = 20.f*std::log10(rms+1e-6f);
        float norm = (db-(-45.f))/(0.f-(-45.f));            // -45..0 dBFS -> 0..1
        norm = std::min(1.f,std::max(0.f,norm));
        float k = norm>g_needle[c]? 0.35f : 0.15f;          // attack faster than decay
        g_needle[c] += (norm-g_needle[c])*k;
        // peak-hold marker
        if(g_needle[c]>=g_peak[c]){ g_peak[c]=g_needle[c]; g_peakHold[c]=PEAK_HOLD_FRAMES; }
        else if(g_peakHold[c]>0.f) g_peakHold[c]-=1.f;
        else g_peak[c]=std::max(0.f, g_peak[c]-PEAK_FALL);
        // clip LED: light when the signal reaches the red zone (>= 0 VU)
        if(norm>=RED) g_clip[c]=1.f; else g_clip[c]*=CLIP_FADE;
    }

    // --- LED spectrum (only compute the FFT when it is being shown) ---
    if(g_mode==1){
        static float re[FN], im[FN];
        int s0=idx-FN;
        for(int i=0;i<FN;++i){
            float s=0.f; int f=s0+i;
            if(f>=0 && f<frames){ for(int c=0;c<g_ch;++c) s+=g_info.buffer[(size_t)f*g_ch+c]; s/=g_ch; }
            float wnd=0.5f-0.5f*std::cos(2.f*3.14159265358979f*i/(FN-1));  // Hann
            re[i]=s*wnd; im[i]=0.f;
        }
        fft(re,im,FN);
        const float flo=40.f, fhi=16000.f;
        for(int b=0;b<NB;++b){
            float f0=flo*std::pow(fhi/flo,(float)b/NB), f1=flo*std::pow(fhi/flo,(float)(b+1)/NB);
            int k0=std::max(1,(int)(f0*FN/g_hz)), k1=std::min(FN/2,(int)(f1*FN/g_hz)); if(k1<=k0) k1=k0+1;
            double acc=0; for(int k=k0;k<k1;++k) acc+=(double)re[k]*re[k]+(double)im[k]*im[k];
            float mag=(float)std::sqrt(acc/(k1-k0))/(FN*0.5f);
            float db=20.f*std::log10(mag+1e-9f);
            db += b*(18.f/NB);                              // gentle high-freq tilt so highs show
            float norm=(db-(-66.f))/(0.f-(-66.f)); norm=std::min(1.f,std::max(0.f,norm));
            float k = norm>g_band[b]? 0.5f : 0.22f;         // fast attack, medium decay
            g_band[b]+=(norm-g_band[b])*k;
            if(g_band[b]>=g_bandPeak[b]){ g_bandPeak[b]=g_band[b]; g_bandHold[b]=PEAK_HOLD_FRAMES; }
            else if(g_bandHold[b]>0.f) g_bandHold[b]-=1.f;
            else g_bandPeak[b]=std::max(0.f, g_bandPeak[b]-PEAK_FALL);
        }
    }
}

// let the meters fall slowly toward rest (used when paused/stopped).
// returns 1 while anything is still moving, 0 once fully settled.
EMSCRIPTEN_KEEPALIVE
int meter_release(){
    int moving=0;
    for(int c=0;c<2;++c){ g_needle[c]+=(0.f-g_needle[c])*0.06f;     // slow VU-style return
        if(g_needle[c]>0.003f) moving=1; else g_needle[c]=0.f;
        if(g_peakHold[c]>0.f) g_peakHold[c]-=1.f; else g_peak[c]=std::max(0.f,g_peak[c]-PEAK_FALL);
        if(g_peak[c]>0.003f) moving=1;
        g_clip[c]*=CLIP_FADE; if(g_clip[c]>0.01f) moving=1; else g_clip[c]=0.f; }
    for(int b=0;b<NB;++b){ g_band[b]+=(0.f-g_band[b])*0.12f;        // LED columns fall
        if(g_band[b]>0.004f) moving=1; else g_band[b]=0.f;
        if(g_bandHold[b]>0.f) g_bandHold[b]-=1.f; else g_bandPeak[b]=std::max(0.f,g_bandPeak[b]-PEAK_FALL);
        if(g_bandPeak[b]>0.004f) moving=1; }
    return moving;
}

EMSCRIPTEN_KEEPALIVE
uint8_t* meter_render(){
    if((int)FB.size()!=W*Hh*4) FB.assign((size_t)W*Hh*4,0);
    rect(0,0,W,Hh, 22,24,30);                              // backdrop
    if(g_mode==1){
        draw_leds();
    } else {
        draw_meter(10,10, 270,168, g_needle[0], g_peak[0], g_clip[0], "L");  // two landscape meters
        draw_meter(290,10,270,168, g_needle[1], g_peak[1], g_clip[1], "R");
    }
    return FB.data();
}
EMSCRIPTEN_KEEPALIVE void dbg_needle(float a,float b){ g_needle[0]=a; g_needle[1]=b; }  // test only
EMSCRIPTEN_KEEPALIVE int meter_w(){ return W; }
EMSCRIPTEN_KEEPALIVE int meter_h(){ return Hh; }

}  // extern "C"
