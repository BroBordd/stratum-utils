#include "Stratum.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

// ── GL helpers ────────────────────────────────────────────────────────────────

static const char* VSH = R"(attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);})";
static const char* FSH = R"(precision mediump float; uniform vec4 color; void main(){gl_FragColor=color;})";
static GLint gPosLoc=-1, gColorLoc=-1;

static GLuint compileShader(GLenum type, const char* src){
    GLuint sh=glCreateShader(type); glShaderSource(sh,1,&src,nullptr); glCompileShader(sh); return sh;
}

static void drawRect(float x0,float y0,float x1,float y1,float r,float g,float b,float a=1.f){
    float nx0=x0*2-1,nx1=x1*2-1,ny0=1-y0*2,ny1=1-y1*2;
    float v[]={nx0,ny0,nx1,ny0,nx0,ny1,nx1,ny1};
    glUniform4f(gColorLoc,r,g,b,a);
    glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,v);
    glEnableVertexAttribArray(gPosLoc);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}

static void drawRoundRect(float x0,float y0,float x1,float y1,
                           float r,float g,float b,float a=1.f,float rad=0.012f){
    drawRect(x0+rad,y0,x1-rad,y1,r,g,b,a);
    drawRect(x0,y0+rad,x1,y1-rad,r,g,b,a);
}

static void drawRingArc(float cx, float cy, float asp,
                         float r_outer, float r_inner,
                         float startAngle, float sweep,
                         float fr, float fg, float fb, float fa,
                         int segments=64){
    if(fabsf(sweep)<0.001f) return;
    float ncx=cx*2-1, ncy=1-cy*2;
    for(int i=0;i<segments;i++){
        float a0=startAngle+(sweep*i)/segments;
        float a1=startAngle+(sweep*(i+1))/segments;
        float qv[]={
            ncx+cosf(a0)*r_inner/asp, ncy+sinf(a0)*r_inner,
            ncx+cosf(a0)*r_outer/asp, ncy+sinf(a0)*r_outer,
            ncx+cosf(a1)*r_inner/asp, ncy+sinf(a1)*r_inner,
            ncx+cosf(a1)*r_outer/asp, ncy+sinf(a1)*r_outer,
        };
        glUniform4f(gColorLoc,fr,fg,fb,fa);
        glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,qv);
        glEnableVertexAttribArray(gPosLoc);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
}

// ── sysinfo reading ───────────────────────────────────────────────────────────

struct CpuStat { long long user,nice,sys,idle,iow,irq,sirq,steal; };

static CpuStat readCpuStat(){
    CpuStat s={};
    FILE* f=fopen("/proc/stat","r");
    if(!f) return s;
    fscanf(f,"cpu %lld %lld %lld %lld %lld %lld %lld %lld",
           &s.user,&s.nice,&s.sys,&s.idle,&s.iow,&s.irq,&s.sirq,&s.steal);
    fclose(f);
    return s;
}

static float cpuUsage(const CpuStat& a, const CpuStat& b){
    long long idle_a=a.idle+a.iow, idle_b=b.idle+b.iow;
    long long total_a=a.user+a.nice+a.sys+a.idle+a.iow+a.irq+a.sirq+a.steal;
    long long total_b=b.user+b.nice+b.sys+b.idle+b.iow+b.irq+b.sirq+b.steal;
    long long dtotal=total_b-total_a, didle=idle_b-idle_a;
    if(dtotal<=0) return 0.f;
    return (float)(dtotal-didle)/(float)dtotal;
}

static int readCpuFreqMHz(){
    for(int i=0;i<16;i++){
        char path[128];
        snprintf(path,sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",i);
        FILE* f=fopen(path,"r");
        if(!f) continue;
        long long khz=0; fscanf(f,"%lld",&khz); fclose(f);
        if(khz>0) return (int)(khz/1000);
    }
    return 0;
}

static int readGpuFreqMHz(){
    const char* paths[]={
        "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "/sys/kernel/gpu/gpu_clock",
        "/sys/devices/platform/mali/clock",
        nullptr
    };
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r");
        if(!f) continue;
        long long hz=0; fscanf(f,"%lld",&hz); fclose(f);
        if(hz>100000) return (int)(hz/1000000);
        if(hz>0)      return (int)(hz/1000);
    }
    return 0;
}

static float readGpuLoad(){
    const char* paths[]={
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/kernel/gpu/gpu_busy",
        "/sys/devices/platform/mali/utilization",
        nullptr
    };
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r");
        if(!f) continue;
        int pct=0; fscanf(f,"%d",&pct); fclose(f);
        return pct/100.f;
    }
    return -1.f;
}

struct MemInfo { long long total,available,swapTotal,swapFree; };

static MemInfo readMemInfo(){
    MemInfo m={};
    FILE* f=fopen("/proc/meminfo","r");
    if(!f) return m;
    char key[64]; long long val; char unit[8];
    for(int i=0;i<60;i++){
        if(fscanf(f,"%s %lld %s",key,&val,unit)!=3) break;
        if     (!strcmp(key,"MemTotal:"))     m.total=val;
        else if(!strcmp(key,"MemAvailable:")) m.available=val;
        else if(!strcmp(key,"SwapTotal:"))    m.swapTotal=val;
        else if(!strcmp(key,"SwapFree:"))     m.swapFree=val;
    }
    fclose(f);
    return m;
}

static int readBattery(){
    const char* paths[]={"/sys/class/power_supply/battery/capacity",
                          "/sys/class/power_supply/BAT0/capacity",nullptr};
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r"); if(!f) continue;
        int p=0; fscanf(f,"%d",&p); fclose(f); return p;
    }
    return -1;
}

static bool readBatCharging(){
    const char* paths[]={"/sys/class/power_supply/battery/status",
                          "/sys/class/power_supply/BAT0/status",nullptr};
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r"); if(!f) continue;
        char s[32]={}; fscanf(f,"%31s",s); fclose(f);
        return !strcmp(s,"Charging")||!strcmp(s,"Full");
    }
    return false;
}

static int readBatTempC(){
    const char* paths[]={"/sys/class/power_supply/battery/temp",
                          "/sys/class/power_supply/BAT0/temp",nullptr};
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r"); if(!f) continue;
        int t=0; fscanf(f,"%d",&t); fclose(f); return t/10;
    }
    return -1;
}

static int readCpuTempC(){
    const char* paths[]={"/sys/class/thermal/thermal_zone0/temp",
                          "/sys/class/thermal/thermal_zone1/temp",nullptr};
    for(int i=0;paths[i];i++){
        FILE* f=fopen(paths[i],"r"); if(!f) continue;
        int t=0; fscanf(f,"%d",&t); fclose(f);
        return t>1000?t/1000:t;
    }
    return -1;
}

static std::string readKernelVersion(){
    char buf[256]={};
    FILE* f=fopen("/proc/version","r");
    if(!f) return "unknown";
    fgets(buf,sizeof(buf),f); fclose(f);
    char* p=strstr(buf,"Linux version ");
    if(!p) return buf;
    p+=14;
    char* end=strchr(p,' ');
    if(end)*end=0;
    return p;
}

static std::string readUptime(){
    FILE* f=fopen("/proc/uptime","r");
    if(!f) return "?";
    float secs=0; fscanf(f,"%f",&secs); fclose(f);
    int h=(int)(secs/3600), m=(int)(secs/60)%60, s=(int)secs%60;
    char buf[32];
    if(h>0) snprintf(buf,sizeof(buf),"%dh %02dm %02ds",h,m,s);
    else    snprintf(buf,sizeof(buf),"%dm %02ds",m,s);
    return buf;
}

static int readCpuCores(){
    int cores=0;
    while(true){
        char path[128];
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d",cores);
        struct stat st; if(stat(path,&st)!=0) break;
        cores++;
    }
    return cores>0?cores:1;
}

// ── live data ─────────────────────────────────────────────────────────────────

struct LiveData {
    std::mutex mtx;
    float cpu=0.f, gpu=-1.f;
    int   cpuMHz=0, gpuMHz=0;
    int   batPct=-1, batTemp=-1, cpuTemp=-1;
    bool  batCharging=false;
    MemInfo mem={};
    std::string kernel, uptime;
    int cores=0;
    bool ready=false;
};

static LiveData gData;
static bool gRunPoller=true;

static void pollerThread(){
    CpuStat prev=readCpuStat();
    usleep(300000);
    while(gRunPoller){
        CpuStat cur=readCpuStat();
        float cpuPct=cpuUsage(prev,cur);
        prev=cur;
        {
            std::lock_guard<std::mutex> lk(gData.mtx);
            gData.cpu=cpuPct;
            gData.gpu=readGpuLoad();
            gData.cpuMHz=readCpuFreqMHz();
            gData.gpuMHz=readGpuFreqMHz();
            gData.batPct=readBattery();
            gData.batCharging=readBatCharging();
            gData.batTemp=readBatTempC();
            gData.cpuTemp=readCpuTempC();
            gData.mem=readMemInfo();
            gData.uptime=readUptime();
            gData.ready=true;
        }
        usleep(800000);
    }
}

// ── gauge ─────────────────────────────────────────────────────────────────────

static const float PI = 3.14159265f;

static void drawGauge(float cx, float cy, float asp,
                       float outerR, float innerR,
                       float value,
                       float fr, float fg, float fb,
                       float startDeg, float sweepDeg){
    float startRad = startDeg * PI / 180.f;
    float sweepRad = sweepDeg * PI / 180.f;
    // bg ring
    drawRingArc(cx,cy,asp,outerR,innerR,startRad,sweepRad,
                0.10f,0.11f,0.14f,0.60f);
    // fill
    float fillSweep=sweepRad*fmaxf(0.f,fminf(1.f,value));
    if(fillSweep>0.001f)
        drawRingArc(cx,cy,asp,outerR,innerR,startRad,fillSweep,fr,fg,fb,0.95f);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int,char**){
    std::thread poller(pollerThread);
    {
        std::lock_guard<std::mutex> lk(gData.mtx);
        gData.kernel=readKernelVersion();
        gData.cores=readCpuCores();
    }

    Stratum s;
    if(!s.init()){ gRunPoller=false; poller.join(); return 1; }
    Text::init(s.aspect());

    GLuint vs=compileShader(GL_VERTEX_SHADER,VSH);
    GLuint fs=compileShader(GL_FRAGMENT_SHADER,FSH);
    GLuint prog=glCreateProgram();
    glAttachShader(prog,vs); glAttachShader(prog,fs);
    glLinkProgram(prog); glUseProgram(prog);
    gPosLoc=glGetAttribLocation(prog,"pos");
    gColorLoc=glGetUniformLocation(prog,"color");
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    s.onKey([&](const KeyEvent& e){
        if(e.action!=KeyAction::DOWN) return;
        if(e.code==KEY_POWER){ gRunPoller=false; s.stop(); }
    });

    s.onFrame([&](float){
        float asp=s.aspect();

        float cpu,gpu; int cpuMHz,gpuMHz,batPct,batTemp,cpuTemp,cores;
        bool batCharging,ready; MemInfo mem; std::string uptime,kernel;
        {
            std::lock_guard<std::mutex> lk(gData.mtx);
            cpu=gData.cpu; gpu=gData.gpu;
            cpuMHz=gData.cpuMHz; gpuMHz=gData.gpuMHz;
            batPct=gData.batPct; batCharging=gData.batCharging;
            batTemp=gData.batTemp; cpuTemp=gData.cpuTemp;
            mem=gData.mem; uptime=gData.uptime;
            kernel=gData.kernel; cores=gData.cores;
            ready=gData.ready;
        }

        glClearColor(0.04f,0.04f,0.06f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        // ── header ────────────────────────────────────────────────────────
        drawRect(0.f,0.f,1.f,0.004f,0.25f,0.50f,1.0f,0.9f);

        float hdrS = fminf(0.042f, 0.55f*asp/11);
        float hdrY = 0.018f;
        Text::draw("SYSTEM INFO", 0.05f, hdrY, hdrS, 0.40f,0.70f,1.00f);

        // kernel and uptime sit on two lines to the right, sized to not collide
        float metaS = fminf(0.014f, 0.50f*asp/24);
        float metaX = 0.05f + 11*hdrS/asp + 0.025f; // start after title
        Text::draw(kernel.c_str(),  metaX, hdrY,              metaS, 0.22f,0.35f,0.52f);
        Text::draw(uptime.c_str(), metaX, hdrY+metaS+0.004f, metaS, 0.20f,0.32f,0.48f);

        float divY = hdrY + hdrS + 0.012f;
        drawRect(0.05f,divY,0.95f,divY+0.002f,0.18f,0.35f,0.70f,0.6f);

        if(!ready){
            float ws=fminf(0.026f,0.7f*asp/14);
            Text::draw("READING...",0.5f-7*ws/asp*0.5f,0.45f,ws,0.30f,0.35f,0.45f);
            drawRect(0.f,0.996f,1.f,1.f,0.25f,0.50f,1.0f,0.7f);
            return;
        }

        // ── gauges ────────────────────────────────────────────────────────

        float gaugeTopY = divY + 0.018f;
        float gOuter    = 0.115f;
        float gInner    = 0.076f;
        float gCY       = gaugeTopY + gOuter + 0.01f;
        const float gCX[3] = {0.20f, 0.50f, 0.80f};

        // helper: centered text inside gauge
        auto gaugeLabel = [&](float cx, float cy, float value, bool avail,
                               float fr, float fg, float fb,
                               const char* name, int mhz){
            float vs2 = fminf(0.028f, 0.65f*asp/4);
            float ls  = fminf(0.015f, 0.55f*asp/3);
            float ms  = fminf(0.013f, 0.50f*asp/6);

            if(avail){
                char pct[8]; snprintf(pct,sizeof(pct),"%d%%",(int)(value*100));
                float pw = strlen(pct)*vs2/asp;
                Text::draw(pct, cx-pw*0.5f, cy-vs2*0.5f, vs2, fr,fg,fb);
            } else {
                float nw = 3*ls/asp;
                Text::draw("N/A", cx-nw*0.5f, cy-ls*0.5f, ls, 0.28f,0.28f,0.35f);
            }
            float nw = strlen(name)*ls/asp;
            Text::draw(name, cx-nw*0.5f, gCY+gInner*0.52f+0.006f, ls, fr*0.75f,fg*0.75f,fb*0.75f);
            if(avail && mhz>0){
                char buf[12]; snprintf(buf,sizeof(buf),"%dMHz",mhz);
                float mw = strlen(buf)*ms/asp;
                Text::draw(buf, cx-mw*0.5f, gCY+gInner*0.52f+0.006f+ls+0.003f, ms, 0.22f,0.30f,0.42f);
            }
        };

        // CPU
        drawGauge(gCX[0],gCY,asp,gOuter,gInner,cpu,
                  0.30f,0.85f,1.00f,200.f,320.f);
        gaugeLabel(gCX[0],gCY,cpu,true,0.30f,0.85f,1.00f,"CPU",cpuMHz);

        // GPU
        {
            bool av=(gpu>=0.f);
            float gv=av?gpu:0.f;
            drawGauge(gCX[1],gCY,asp,gOuter,gInner,gv,
                      0.75f,0.35f,1.00f,200.f,320.f);
            gaugeLabel(gCX[1],gCY,gv,av,0.75f,0.35f,1.00f,"GPU",gpuMHz);
        }

        // Battery
        {
            bool av=(batPct>=0);
            float bv=av?(batPct/100.f):0.f;
            float br=bv<0.20f?1.00f:bv<0.50f?1.00f:0.25f;
            float bg=bv<0.20f?0.25f:bv<0.50f?0.72f:0.90f;
            float bb=bv<0.20f?0.25f:bv<0.50f?0.10f:0.35f;
            drawGauge(gCX[2],gCY,asp,gOuter,gInner,bv,br,bg,bb,200.f,320.f);
            gaugeLabel(gCX[2],gCY,bv,av,br,bg,bb,"BAT",0);
            if(av&&batCharging){
                float cs=fminf(0.013f,0.5f*asp/5);
                float cw=3*cs/asp;
                Text::draw("CHG",gCX[2]-cw*0.5f,
                           gCY+gInner*0.52f+0.006f+fminf(0.015f,0.55f*asp/3)+0.003f,
                           cs,0.40f,1.00f,0.40f);
            }
        }

        // ── info panels ───────────────────────────────────────────────────

        float panelTopY = gCY + gOuter + 0.030f;
        float margin    = 0.03f;
        float totalW    = 1.f - margin*2;
        float gap       = 0.010f;
        float pw4       = (totalW - gap*3) / 4.f;
        float panelH    = 0.082f;

        // label/value sizes fitted to panel width
        float lblS = fminf(0.013f, 0.60f*asp*pw4/10);
        float valS = fminf(0.020f, 0.65f*asp*pw4/8);

        auto drawPanel = [&](float px0, float py0, float px1,
                              const char* title, const char* value,
                              float tr, float tg, float tb){
            drawRoundRect(px0,py0,px1,py0+panelH,0.07f,0.07f,0.09f,0.70f,0.008f);
            drawRect(px0+0.005f,py0+0.008f,px0+0.010f,py0+panelH-0.008f,tr,tg,tb,0.80f);
            float pw2=strlen(title)*lblS/asp;
            Text::draw(title, px0+(px1-px0-pw2)*0.5f, py0+0.008f, lblS, 0.32f,0.35f,0.42f);
            float vw=strlen(value)*valS/asp;
            // clamp value text so it doesn't overflow panel
            float fvalS=valS;
            float maxW=(px1-px0)-0.018f;
            if(vw>maxW) fvalS*=maxW/vw;
            float fvw=strlen(value)*fvalS/asp;
            Text::draw(value, px0+(px1-px0-fvw)*0.5f,
                       py0+0.008f+lblS+0.005f, fvalS, tr,tg,tb);
        };

        // Row 1
        float px=margin;

        // RAM used
        {
            long long used=mem.total-mem.available;
            float usedGB=used/(1024.f*1024.f);
            float ratio=mem.total>0?(float)used/mem.total:0.f;
            char val[16];
            if(mem.total>0) snprintf(val,sizeof(val),"%.1f GB",usedGB);
            else strcpy(val,"?");
            float mr=ratio>0.8f?1.f:ratio>0.5f?1.f:0.35f;
            float mg=ratio>0.8f?0.3f:ratio>0.5f?0.7f:0.85f;
            float mb=ratio>0.8f?0.3f:ratio>0.5f?0.1f:0.55f;
            drawPanel(px,panelTopY,px+pw4,"RAM USED",val,mr,mg,mb);
        }
        px+=pw4+gap;

        // RAM total
        {
            char val[16];
            float tGB=mem.total/(1024.f*1024.f);
            if(mem.total>0) snprintf(val,sizeof(val),"%.1f GB",tGB);
            else strcpy(val,"?");
            drawPanel(px,panelTopY,px+pw4,"RAM TOTAL",val,0.35f,0.60f,0.90f);
        }
        px+=pw4+gap;

        // CPU temp
        {
            char val[16];
            if(cpuTemp>=0) snprintf(val,sizeof(val),"%d C",cpuTemp);
            else strcpy(val,"?");
            float tr=cpuTemp>80?1.f:cpuTemp>60?1.f:0.40f;
            float tg=cpuTemp>80?0.3f:cpuTemp>60?0.7f:0.85f;
            float tb=cpuTemp>80?0.3f:cpuTemp>60?0.1f:0.60f;
            drawPanel(px,panelTopY,px+pw4,"CPU TEMP",val,tr,tg,tb);
        }
        px+=pw4+gap;

        // BAT temp
        {
            char val[16];
            if(batTemp>=0) snprintf(val,sizeof(val),"%d C",batTemp);
            else strcpy(val,"?");
            float tr=batTemp>45?1.f:batTemp>35?1.f:0.40f;
            float tg=batTemp>45?0.3f:batTemp>35?0.7f:0.85f;
            float tb=batTemp>45?0.3f:batTemp>35?0.1f:0.60f;
            drawPanel(px,panelTopY,px+pw4,"BAT TEMP",val,tr,tg,tb);
        }

        // Row 2
        float row2Y = panelTopY + panelH + gap;
        px = margin;

        // Cores
        {
            char val[8]; snprintf(val,sizeof(val),"%d",cores);
            drawPanel(px,row2Y,px+pw4,"CORES",val,0.50f,0.80f,1.00f);
        }
        px+=pw4+gap;

        // Swap
        {
            char val[24];
            if(mem.swapTotal>0){
                float used=(mem.swapTotal-mem.swapFree)/(1024.f*1024.f);
                float tot=mem.swapTotal/(1024.f*1024.f);
                snprintf(val,sizeof(val),"%.1f/%.1fG",used,tot);
            } else {
                strcpy(val,"NONE");
            }
            drawPanel(px,row2Y,px+pw4,"SWAP",val,0.40f,0.55f,0.75f);
        }
        px+=pw4+gap;

        // Uptime spans remaining two columns
        {
            float upW = pw4*2+gap;
            drawPanel(px,row2Y,px+upW,"UPTIME",uptime.c_str(),0.55f,0.85f,0.55f);
        }

        // ── memory bar ────────────────────────────────────────────────────
        float barY = row2Y + panelH + gap;
        if(mem.total>0){
            float used=(float)(mem.total-mem.available)/mem.total;
            float bx0=margin, bx1=1.f-margin, bh=0.020f;
            drawRoundRect(bx0,barY,bx1,barY+bh,0.08f,0.09f,0.10f,0.80f,0.008f);
            drawRoundRect(bx0,barY,bx0+(bx1-bx0)*used,barY+bh,
                          0.25f,0.70f,0.45f,0.90f,0.008f);
            float ms2=fminf(0.012f,0.55f*asp/30);
            char memLbl[48];
            snprintf(memLbl,sizeof(memLbl),"%.0f MB / %.0f MB",
                     (mem.total-mem.available)/1024.f, mem.total/1024.f);
            float mw=strlen(memLbl)*ms2/asp;
            Text::draw(memLbl,(bx0+bx1-mw)*0.5f,barY+bh+0.004f,ms2,0.28f,0.52f,0.38f);
        }

        // ── footer ────────────────────────────────────────────────────────
        float hintS=fminf(0.016f,0.70f*asp/14);
        Text::draw("PWR  exit",0.05f,0.963f,hintS,0.18f,0.20f,0.28f);
        drawRect(0.f,0.996f,1.f,1.f,0.25f,0.50f,1.0f,0.7f);
    });

    s.run();
    gRunPoller=false;
    poller.join();
    return 0;
}
