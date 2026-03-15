#include "Stratum.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>

// ── pty ───────────────────────────────────────────────────────────────────────

static int   gMasterFd = -1;
static pid_t gShellPid = -1;

static void pty_write(const char* s, int len = -1) {
    if (gMasterFd < 0 || !s) return;
    if (len < 0) len = strlen(s);
    write(gMasterFd, s, len);
}

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

// ── unified terminal core ─────────────────────────────────────────────────────

struct Terminal {
    std::mutex mtx;
    int cols = 80, rows = 24;
    int cx = 0, cy = 0;
    bool wrap_pending = false;

    std::vector<std::string> screen;
    std::deque<std::string> history;
    int scroll_off = 0;
    const int MAX_HISTORY = 2000;

    // Alt screen
    bool in_alt = false;
    std::vector<std::string> alt_screen;
    int alt_cx = 0, alt_cy = 0;

    // Esc parser
    int esc_state = 0;
    char esc_buf[64];
    int esc_len = 0;
    bool esc_priv = false;

    Terminal() {
        screen.assign(rows, std::string(cols, ' '));
        alt_screen.assign(rows, std::string(cols, ' '));
    }

    void resize(int c, int r) {
        std::lock_guard<std::mutex> lk(mtx);
        if (c == cols && r == rows) return;

        std::vector<std::string> new_screen(r, std::string(c, ' '));
        int copy_rows = std::min((int)screen.size(), r);
        for (int i = 0; i < copy_rows; i++) {
            int copy_cols = std::min((int)screen[i].size(), c);
            for (int j = 0; j < copy_cols; j++)
                new_screen[i][j] = screen[i][j];
        }
        screen = std::move(new_screen);
        cx = std::min(cx, c - 1);
        cy = std::min(cy, r - 1);

        if (in_alt) {
            std::vector<std::string> new_alt(r, std::string(c, ' '));
            int cr = std::min((int)alt_screen.size(), r);
            for (int i = 0; i < cr; i++) {
                int cc = std::min((int)alt_screen[i].size(), c);
                for (int j = 0; j < cc; j++)
                    new_alt[i][j] = alt_screen[i][j];
            }
            alt_screen = std::move(new_alt);
        } else {
            alt_screen.assign(r, std::string(c, ' '));
        }
        alt_cx = std::min(alt_cx, c - 1);
        alt_cy = std::min(alt_cy, r - 1);

        cols = c;
        rows = r;

        if (gMasterFd >= 0) {
            struct winsize ws;
            ws.ws_col = cols; ws.ws_row = rows;
            ws.ws_xpixel = 0; ws.ws_ypixel = 0;
            ioctl(gMasterFd, TIOCSWINSZ, &ws);
            if (gShellPid > 0) kill(gShellPid, SIGWINCH);
        }
    }

    void scroll_up() {
        if (!in_alt) {
            history.push_back(screen[0]);
            if ((int)history.size() > MAX_HISTORY) history.pop_front();
        }
        for (int i = 1; i < rows; i++) screen[i - 1] = screen[i];
        screen[rows - 1] = std::string(cols, ' ');
    }

    void put_char(char c) {
        if (wrap_pending) {
            cx = 0;
            cy++;
            if (cy >= rows) { cy = rows - 1; scroll_up(); }
            wrap_pending = false;
        }
        screen[cy][cx] = c;
        if (cx < cols - 1) {
            cx++;
        } else {
            wrap_pending = true;
        }
    }

    void clear_line(int mode) {
        if (mode == 0) {
            for (int i = cx; i < cols; i++) screen[cy][i] = ' ';
        } else if (mode == 1) {
            for (int i = 0; i <= cx; i++) screen[cy][i] = ' ';
        } else if (mode == 2) {
            screen[cy] = std::string(cols, ' ');
        }
    }

    void clear_screen(int mode) {
        if (mode == 0) {
            clear_line(0);
            for (int r = cy + 1; r < rows; r++) screen[r] = std::string(cols, ' ');
        } else if (mode == 1) {
            for (int r = 0; r < cy; r++) screen[r] = std::string(cols, ' ');
            clear_line(1);
        } else if (mode == 2 || mode == 3) {
            for (int r = 0; r < rows; r++) screen[r] = std::string(cols, ' ');
            if (mode == 3 && !in_alt) history.clear();
            cx = cy = 0;
        }
    }

    void handle_csi(char fin, const char* params) {
        int p[4] = {0,0,0,0}; int np = 0;
        char tmp[64]; strncpy(tmp, params, 63); tmp[63] = 0;
        char* tok = strtok(tmp, ";");
        while (tok && np < 4) { p[np++] = atoi(tok); tok = strtok(nullptr, ";"); }
        auto P = [&](int i, int def=1) { return (i < np && p[i] > 0) ? p[i] : def; };

        if (esc_priv && (fin == 'h' || fin == 'l')) {
            if (p[0] == 1049 || p[0] == 47) {
                if (fin == 'h' && !in_alt) {
                    in_alt = true;
                    alt_screen = screen; alt_cx = cx; alt_cy = cy;
                    for (int r = 0; r < rows; r++) screen[r] = std::string(cols, ' ');
                    cx = cy = 0;
                } else if (fin == 'l' && in_alt) {
                    in_alt = false;
                    screen = alt_screen; cx = alt_cx; cy = alt_cy;
                }
            }
            return;
        }

        switch (fin) {
            case 'A': cy = std::max(0, cy - P(0)); wrap_pending = false; break;
            case 'B': cy = std::min(rows - 1, cy + P(0)); wrap_pending = false; break;
            case 'C': cx = std::min(cols - 1, cx + P(0)); wrap_pending = false; break;
            case 'D': cx = std::max(0, cx - P(0)); wrap_pending = false; break;
            case 'H': case 'f':
                cy = std::min(rows - 1, std::max(0, P(0, 1) - 1));
                cx = std::min(cols - 1, std::max(0, P(1, 1) - 1));
                wrap_pending = false;
                break;
            case 'J': clear_screen(p[0]); break;
            case 'K': clear_line(p[0]); break;
        }
    }

    void feed(char c) {
        std::lock_guard<std::mutex> lk(mtx);

        if (esc_state == 0) {
            if (c == '\x1b') { esc_state = 1; esc_len = 0; esc_priv = false; return; }
            if (c == '\r') { cx = 0; wrap_pending = false; return; }
            if (c == '\n') {
                cy++;
                if (cy >= rows) { cy = rows - 1; scroll_up(); }
                wrap_pending = false;
                return;
            }
            if (c == '\b') { if (cx > 0) cx--; wrap_pending = false; return; }
            if (c == '\t') { cx = std::min(cols - 1, (cx / 8 + 1) * 8); wrap_pending = false; return; }
            if ((unsigned char)c >= 32) put_char(c);
            return;
        }

        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; return; }
            if (c == '(') { esc_state = 3; return; }
            if (c == 'c') { clear_screen(2); esc_state = 0; return; }
            esc_state = 0; return;
        }
        if (esc_state == 3) { esc_state = 0; return; }

        if (c == '?' && esc_len == 0) { esc_priv = true; return; }
        if ((c >= '0' && c <= '9') || c == ';') { if (esc_len < 62) esc_buf[esc_len++] = c; return; }
        esc_buf[esc_len] = 0;
        esc_state = 0;
        handle_csi(c, esc_buf);
    }
};

static Terminal gTerm;
static float gCursorBlinkT = 0.f;

// ── shaders ───────────────────────────────────────────────────────────────────

static const char* VSH=R"(attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);})";
static const char* FSH=R"(precision mediump float;uniform vec4 color;void main(){gl_FragColor=color;})";
static GLint gPosLoc=-1,gColorLoc=-1;

static GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    return sh;
}

static void drawRect(float x0,float y0,float x1,float y1,float r,float g,float b,float a=1.f){
    float nx0=x0*2-1,nx1=x1*2-1,ny0=1-y0*2,ny1=1-y1*2;
    float v[]={nx0,ny0,nx1,ny0,nx0,ny1,nx1,ny1};
    glUniform4f(gColorLoc,r,g,b,a);
    glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,v);
    glEnableVertexAttribArray(gPosLoc);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}

static void drawRoundRect(float x0,float y0,float x1,float y1,float r,float g,float b,float a=1.f,float rad=0.008f){
    drawRect(x0+rad,y0,x1-rad,y1,r,g,b,a);
    drawRect(x0,y0+rad,x1,y1-rad,r,g,b,a);
}

// ── key flash ─────────────────────────────────────────────────────────────────

struct KeyFlash{float kx0,ky0,kx1,ky1,t;};
static std::vector<KeyFlash> gFlashes;
static std::mutex gFlashMtx;
static const float FLASH_DUR=0.28f;

// ── keyboard layout ───────────────────────────────────────────────────────────

#define K_SHIFT "\x01"
#define K_DEL   "\x7f"
#define K_ENTER "\r"
#define K_SPACE " "
#define K_SYM   "\x10"
#define K_ABC   "\x11"
#define K_PGUP  "\x12"
#define K_TAB   "\t"
#define K_ESC   "\x1b"
#define K_CTRL  "\x13"
#define K_ALT   "\x14"
#define K_UP    "\x1b[A"
#define K_DOWN  "\x1b[B"
#define K_LEFT  "\x1b[D"
#define K_RIGHT "\x1b[C"
#define K_HOME  "\x1b[H"
#define K_END   "\x1b[F"
#define K_EXIT  "\x15"

struct Key{const char* label;const char* send;float w;};

static const Key SP0[]={{"CTRL",K_CTRL,1.5f},{"ALT",K_ALT,1.5f},{"ESC",K_ESC,1.2f},{"TAB",K_TAB,1.2f},{"HOME",K_HOME,1.2f},{"END",K_END,1.2f},{"EXIT",K_EXIT,1.5f}};
static const Key SP1[]={{"  ^  ",K_UP,2.5f},{"  v  ",K_DOWN,2.5f},{"  <  ",K_LEFT,2.5f},{"  >  ",K_RIGHT,2.5f}};
static const Key L0[]={{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},{"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1}};
static const Key L1[]={{"q","q",1},{"w","w",1},{"e","e",1},{"r","r",1},{"t","t",1},{"y","y",1},{"u","u",1},{"i","i",1},{"o","o",1},{"p","p",1}};
static const Key L2[]={{"a","a",1},{"s","s",1},{"d","d",1},{"f","f",1},{"g","g",1},{"h","h",1},{"j","j",1},{"k","k",1},{"l","l",1}};
static const Key L3[]={{"SHF",K_SHIFT,1.5f},{"z","z",1},{"x","x",1},{"c","c",1},{"v","v",1},{"b","b",1},{"n","n",1},{"m","m",1},{"DEL",K_DEL,1.5f}};
static const Key L4[]={{"!#1",K_SYM,1.5f},{",",",",1},{"SPACE",K_SPACE,4.0f},{".",  ".",1},{"ENT",K_ENTER,1.5f}};
static const Key S0[]={{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},{"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1}};
static const Key S1[]={{"+","+",1},{"=","=",1},{"/","/",1},{"_","_",1},{"\\","\\",1},{"|","|",1},{"<","<",1},{">",">",1},{"[","[",1},{"]","]",1}};
static const Key S2[]={{"!","!",1},{"@","@",1},{"#","#",1},{"$","$",1},{"%","%",1},{"^","^",1},{"&","&",1},{"*","*",1},{"(","(",1},{")",  ")",1}};
static const Key S3[]={{"1/2",K_PGUP,1.5f},{"-","-",1},{"'","'",1},{"\"","\"",1},{":",  ":",1},{";",";",1},{",",",",1},{"?","?",1},{"DEL",K_DEL,1.5f}};
static const Key S4[]={{"ABC",K_ABC,1.5f},{",",",",1},{"SPACE",K_SPACE,4.0f},{".",  ".",1},{"ENT",K_ENTER,1.5f}};
static const Key T0[]={{"1","1",1},{"2","2",1},{"3","3",1},{"4","4",1},{"5","5",1},{"6","6",1},{"7","7",1},{"8","8",1},{"9","9",1},{"0","0",1}};
static const Key T1[]={{"~","~",1},{"`","`",1},{"{","{",1},{"}","}",1},{"(",  "(",1},{")",  ")",1},{"$","$",1},{"\\","\\",1},{"|","|",1},{"&","&",1}};
static const Key T2[]={{"&&","&&",2},{"||","||",2},{">>",">>",2},{"<<","<<",2},{"$()","$()",2}};
static const Key T3[]={{"2/2",K_SYM,1.5f},{"../","../",1.5f},{"~/","~/",1.2f},{" -"," -",1.2f},{"| ","| ",1.2f},{"DEL",K_DEL,1.5f}};
static const Key T4[]={{"ABC",K_ABC,1.5f},{",",",",1},{"SPACE",K_SPACE,4.0f},{".",  ".",1},{"ENT",K_ENTER,1.5f}};

#define ROW(r) {r,(int)(sizeof(r)/sizeof(r[0]))}
struct KbRow{const Key* keys;int count;};
static const KbRow ROWS_L[]={ROW(SP0),ROW(SP1),ROW(L0),ROW(L1),ROW(L2),ROW(L3),ROW(L4)};
static const KbRow ROWS_S[]={ROW(SP0),ROW(SP1),ROW(S0),ROW(S1),ROW(S2),ROW(S3),ROW(S4)};
static const KbRow ROWS_T[]={ROW(SP0),ROW(SP1),ROW(T0),ROW(T1),ROW(T2),ROW(T3),ROW(T4)};
static const int KB_ROWS=7,KB_SPECIAL_ROWS=2;

// ── keyboard state ────────────────────────────────────────────────────────────

enum class KbPage{LETTERS,SYM1,SYM2};
enum class ScreenStatus{TERM,CONFIRM_EXIT};
static KbPage gPage=KbPage::LETTERS;
static bool   gShift=false,gCtrl=false,gAlt=false;
static ScreenStatus gScreen=ScreenStatus::TERM;
static bool   gKbVisible=true;
static float  gKbY0=0.50f,gKbRowH=0.0f;

static void handleKey(const char* send) {
    if (!send) return;
    gCursorBlinkT = mono_now();
    if (strcmp(send,K_SHIFT)==0) { gShift=!gShift; return; }
    if (strcmp(send,K_CTRL)==0)  { gCtrl=!gCtrl;   return; }
    if (strcmp(send,K_ALT)==0)   { gAlt=!gAlt;     return; }
    if (strcmp(send,K_SYM)==0)   { gPage=(gPage==KbPage::SYM1)?KbPage::SYM2:KbPage::SYM1; return; }
    if (strcmp(send,K_PGUP)==0)  { gPage=KbPage::SYM2;    return; }
    if (strcmp(send,K_ABC)==0)   { gPage=KbPage::LETTERS;  return; }
    if (strcmp(send,K_EXIT)==0)  { gScreen=ScreenStatus::CONFIRM_EXIT; return; }
    if (gCtrl) {
        char c=send[0]; if(c>='a'&&c<='z') c-=32;
        if (c>='@'&&c<='_') { char ctrl=c-'@'; pty_write(&ctrl,1); }
        gCtrl=false; return;
    }
    if (gAlt) { char buf[4]={'\x1b',send[0],0}; pty_write(buf,2); gAlt=false; return; }
    if (gShift&&send[0]>='a'&&send[0]<='z'&&send[1]==0) {
        char up=send[0]-32; pty_write(&up,1); gShift=false; return;
    }
    pty_write(send);
    if (gShift) gShift=false;
}

// ── hit test ──────────────────────────────────────────────────────────────────

static const Key* hitTest(float tx, float ty,
                           float* ox0=nullptr, float* oy0=nullptr,
                           float* ox1=nullptr, float* oy1=nullptr) {
    const KbRow* rows=gPage==KbPage::LETTERS?ROWS_L:gPage==KbPage::SYM1?ROWS_S:ROWS_T;
    for (int r=0; r<KB_ROWS; r++) {
        float ry0=gKbY0+r*gKbRowH;
        if (ty<ry0||ty>ry0+gKbRowH) continue;
        const KbRow& row=rows[r];
        float totalW=0; for(int i=0;i<row.count;i++) totalW+=row.keys[i].w;
        float x=0.03f, scale=0.94f/totalW;
        for (int i=0; i<row.count; i++) {
            float kw=row.keys[i].w*scale;
            if (tx>=x&&tx<=x+kw) {
                if(ox0)*ox0=x+0.002f; if(ox1)*ox1=x+kw-0.002f;
                if(oy0)*oy0=ry0+0.002f; if(oy1)*oy1=ry0+gKbRowH-0.002f;
                return &row.keys[i];
            }
            x+=kw;
        }
    }
    return nullptr;
}

// ── floating button bounds ────────────────────────────────────────────────────

static void kbBtnBounds(float& bx0,float& by0,float& bx1,float& by1) {
    float base=gKbVisible?(gKbY0-0.008f):0.985f;
    bx0=0.84f; bx1=0.97f; by1=base; by0=by1-0.038f;
}
static void scrollBtnBounds(bool up, float& bx0, float& by0, float& bx1, float& by1) {
    float base=gKbVisible?(gKbY0-0.008f):0.985f;
    bx0=0.84f; bx1=0.97f;
    if (up) { by1=base-0.088f; by0=by1-0.038f; }
    else    { by1=base-0.044f; by0=by1-0.038f; }
}

// ── touch state ───────────────────────────────────────────────────────────────

static float      gTouchStartY    = -1.f;
static float      gTouchStartX    = -1.f;
static float      gTouchLastY     = -1.f;
static bool       gTouchScrolling = false;
static bool       gTouchWasBtn    = false;
static const Key* gHeldKey        = nullptr;
static float      gRepeatNext     = -1.f;
static const float REPEAT_DELAY    = 0.45f;
static const float REPEAT_INTERVAL = 0.06f;

// ── main ──────────────────────────────────────────────────────────────────────

int main(int,char**) {
    struct winsize ws;
    ws.ws_col=80; ws.ws_row=24; ws.ws_xpixel=0; ws.ws_ypixel=0;
    int master;
    gShellPid=forkpty(&master,nullptr,nullptr,&ws);
    if (gShellPid<0) return 1;
    if (gShellPid==0) {
        setenv("TERM","xterm-256color",1);
        unsetenv("TERMINFO");
        unsetenv("TERMINFO_DIRS");
        execl("/system/bin/sh","sh",nullptr);
        exit(1);
    }
    gMasterFd=master;
    fcntl(master,F_SETFL,O_NONBLOCK);

    // Kick the prompt in case CWD wrap pushed it off-screen
    usleep(120000);
    pty_write("\r", 1);

    Stratum s;
    if (!s.init()) return 1;
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

    float asp=s.aspect();
    gKbRowH=(1.0f-gKbY0-0.002f)/KB_ROWS;

    s.onTouch([&](const TouchEvent& e) {
        if (gScreen==ScreenStatus::CONFIRM_EXIT) {
            if (e.action!=TouchAction::DOWN) return;
            gTouchWasBtn=true;
            if (e.y>=0.51f&&e.y<=0.61f) {
                bool hitYes=e.x>=0.10f&&e.x<=0.44f;
                bool hitNo =e.x>=0.56f&&e.x<=0.90f;
                if (hitYes) {
                    if (gMasterFd>=0) { close(gMasterFd); gMasterFd=-1; }
                    kill(gShellPid,SIGKILL); s.stop();
                } else if (hitNo) { gScreen=ScreenStatus::TERM; }
            }
            return;
        }

        if (e.action==TouchAction::DOWN) {
            gTouchStartY=e.y; gTouchStartX=e.x;
            gTouchLastY=e.y; gTouchScrolling=false;
            gTouchWasBtn=false;
            gHeldKey=nullptr;
            // dialog covers 0.36–0.66; any tap anywhere dismisses to TERM
            // unless it lands squarely on YES
            bool hitYes=e.x>=0.10f&&e.x<=0.44f&&e.y>=0.51f&&e.y<=0.61f;
            bool hitNo =e.x>=0.56f&&e.x<=0.90f&&e.y>=0.51f&&e.y<=0.61f;
            if (hitYes) {
                if (gMasterFd>=0) { close(gMasterFd); gMasterFd=-1; }
                kill(gShellPid,SIGKILL); s.stop();
            } else {
                // NO button or anywhere outside YES → cancel
                gScreen=ScreenStatus::TERM;
            }
            return;
        }

        if (e.action==TouchAction::DOWN) {
            gTouchStartY=e.y; gTouchStartX=e.x;
            gTouchLastY=e.y; gTouchScrolling=false;
            gTouchWasBtn=false;
            gHeldKey=nullptr;

            float bx0,by0,bx1,by1;
            kbBtnBounds(bx0,by0,bx1,by1);
            if (e.x>=bx0&&e.x<=bx1&&e.y>=by0&&e.y<=by1) {
                gKbVisible=!gKbVisible;
                gKbRowH=(1.0f-gKbY0-0.002f)/KB_ROWS;
                gTouchWasBtn=true;
                return;
            }
            float sbx0,sby0,sbx1,sby1;
            scrollBtnBounds(true,sbx0,sby0,sbx1,sby1);
            if (e.x>=sbx0&&e.x<=sbx1&&e.y>=sby0&&e.y<=sby1) {
                std::lock_guard<std::mutex> lk(gTerm.mtx);
                gTerm.scroll_off=std::min(gTerm.scroll_off+4,(int)gTerm.history.size());
                gTouchWasBtn=true;
                return;
            }
            scrollBtnBounds(false,sbx0,sby0,sbx1,sby1);
            if (e.x>=sbx0&&e.x<=sbx1&&e.y>=sby0&&e.y<=sby1) {
                std::lock_guard<std::mutex> lk(gTerm.mtx);
                gTerm.scroll_off=std::max(gTerm.scroll_off-4,0);
                gTouchWasBtn=true;
                return;
            }

            if (gKbVisible&&e.y>=gKbY0) {
                float kx0,ky0,kx1,ky1;
                const Key* k=hitTest(e.x,e.y,&kx0,&ky0,&kx1,&ky1);
                if (k) {
                    bool rep=strcmp(k->send,K_DEL)==0||strcmp(k->send,K_UP)==0||
                             strcmp(k->send,K_DOWN)==0||strcmp(k->send,K_LEFT)==0||
                             strcmp(k->send,K_RIGHT)==0;
                    if (rep) { gHeldKey=k; gRepeatNext=mono_now()+REPEAT_DELAY; }
                }
            }
        } else if (e.action==TouchAction::MOVE) {
            float termBottom=gKbVisible?gKbY0:1.0f;
            if (gTouchStartY<termBottom-0.05f) {
                float dy=e.y-gTouchLastY;
                if (fabsf(dy)>0.008f) {
                    gTouchScrolling=true;
                    std::lock_guard<std::mutex> lk(gTerm.mtx);
                    int delta=(int)(dy/0.010f);
                    gTerm.scroll_off=std::max(0,std::min((int)gTerm.history.size(),gTerm.scroll_off+delta));
                    gTouchLastY=e.y;
                }
            }
        } else if (e.action==TouchAction::UP) {
            float dy=e.y-gTouchStartY;
            gHeldKey=nullptr;

            float termBottom=gKbVisible?gKbY0:1.0f;
            if (!gTouchScrolling&&gTouchStartY<termBottom-0.05f&&fabsf(dy)>0.03f) {
                std::lock_guard<std::mutex> lk(gTerm.mtx);
                int delta=(int)(dy/0.010f);
                gTerm.scroll_off=std::max(0,std::min((int)gTerm.history.size(),gTerm.scroll_off+delta));
                gTouchScrolling=false;
                return;
            }
            gTouchScrolling=false;

            if (!gTouchWasBtn&&gKbVisible&&e.y>=gKbY0&&gTouchStartY>=gKbY0&&fabsf(dy)<0.025f) {
                float kx0,ky0,kx1,ky1;
                const Key* k=hitTest(gTouchStartX,gTouchStartY,&kx0,&ky0,&kx1,&ky1);
                if (k) {
                    handleKey(k->send); Stratum::vibrate(16);
                    std::lock_guard<std::mutex> lk(gFlashMtx);
                    gFlashes.push_back({kx0,ky0,kx1,ky1,mono_now()});
                }
            }
        }
    });

    s.onKey([&](const KeyEvent& e) {
        if (e.action!=KeyAction::DOWN&&e.action!=KeyAction::REPEAT) return;
        if (gScreen==ScreenStatus::CONFIRM_EXIT) {
            if (e.code==KEY_POWER) {
                if (gMasterFd>=0) { close(gMasterFd); gMasterFd=-1; }
                kill(gShellPid,SIGKILL); s.stop();
            }
            if (e.code==KEY_VOLUMEUP||e.code==KEY_VOLUMEDOWN) gScreen=ScreenStatus::TERM;
            return;
        }
        if (e.code==KEY_VOLUMEUP)   { std::lock_guard<std::mutex> lk(gTerm.mtx); gTerm.scroll_off=std::min(gTerm.scroll_off+3,(int)gTerm.history.size()); }
        if (e.code==KEY_VOLUMEDOWN) { std::lock_guard<std::mutex> lk(gTerm.mtx); gTerm.scroll_off=std::max(gTerm.scroll_off-3,0); }
        if (e.code==KEY_POWER) gScreen=ScreenStatus::CONFIRM_EXIT;
    });

    s.onFrame([&](float) {
        float now=mono_now();

        if (gHeldKey&&now>=gRepeatNext) {
            handleKey(gHeldKey->send); Stratum::vibrate(10);
            gRepeatNext=now+REPEAT_INTERVAL;
        }

        char buf[512]; int n;
        while ((n=read(gMasterFd,buf,sizeof(buf)))>0)
            for (int i=0; i<n; i++) gTerm.feed(buf[i]);

        asp=s.aspect();
        glClearColor(0.02f,0.02f,0.03f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        float termY1=gKbVisible?(gKbY0-0.003f):0.99f;
        float txtS=fminf(0.020f,0.85f*asp/44);
        float lineH=txtS*1.25f;
        float charW=txtS/asp;

        int new_cols=std::max(1,(int)((0.94f-0.02f)/charW));
        int new_rows=std::max(1,(int)((termY1-0.004f)/lineH));
        gTerm.resize(new_cols,new_rows);

        {
            std::lock_guard<std::mutex> lk(gTerm.mtx);

            // Draw lines
            int draw_r=0;
            int hist_sz=(int)gTerm.history.size();
            for (int r=0; r<gTerm.rows; r++) {
                float y=0.004f+draw_r*lineH;
                if (y+lineH>termY1) break;

                std::string line_str;
                if (gTerm.scroll_off>0) {
                    int hist_idx=hist_sz-gTerm.scroll_off+r;
                    if (hist_idx>=0&&hist_idx<hist_sz) {
                        line_str=gTerm.history[hist_idx];
                    } else if (hist_idx>=hist_sz) {
                        int screen_row=hist_idx-hist_sz;
                        if (screen_row<gTerm.rows)
                            line_str=gTerm.screen[screen_row];
                    }
                    // hist_idx < 0 → above history window, leave blank
                } else {
                    line_str=gTerm.screen[r];
                }

                if (!line_str.empty())
                    Text::draw(line_str.c_str(),0.02f,y,txtS,0.72f,0.92f,0.62f);
                draw_r++;
            }

            // Draw cursor
            if (gTerm.scroll_off==0) {
                float age=now-gCursorBlinkT;
                float blink=age<0.5f?1.0f:0.35f+0.65f*sinf((age-0.5f)*3.14f*1.4f+1.57f);
                float cx=0.02f+gTerm.cx*charW;
                float cy=0.004f+gTerm.cy*lineH;
                if (cy+txtS<=termY1)
                    drawRect(cx,cy,cx+charW,cy+txtS,0.72f,0.92f,0.62f,std::max(0.f,blink)*0.45f);
            }

            // Draw scroll hint
            if (gTerm.scroll_off>0) {
                char sbuf[32]; snprintf(sbuf,sizeof(sbuf),"^ +%d",gTerm.scroll_off);
                Text::draw(sbuf,0.03f,0.004f,fminf(0.015f,0.7f*asp/22),0.80f,0.65f,0.20f);
            }
        }

        if (gKbVisible) drawRect(0.f,termY1,1.f,termY1+0.003f,0.15f,0.35f,0.55f);

        // ── keyboard ──────────────────────────────────────────────────────
        if (gKbVisible) {
            const KbRow* rows=gPage==KbPage::LETTERS?ROWS_L:gPage==KbPage::SYM1?ROWS_S:ROWS_T;
            for (int r=0; r<KB_ROWS; r++) {
                bool special=r<KB_SPECIAL_ROWS;
                const KbRow& row=rows[r];
                float ry0=gKbY0+r*gKbRowH;
                float totalW=0; for(int i=0;i<row.count;i++) totalW+=row.keys[i].w;
                float scale=0.94f/totalW, x=0.03f;
                for (int i=0; i<row.count; i++) {
                    const Key& k=row.keys[i];
                    float kw=k.w*scale;
                    float kx0=x+0.002f, kx1=x+kw-0.002f;
                    float ky0=ry0+0.002f, ky1=ry0+gKbRowH-0.002f;
                    bool isCtrl=strcmp(k.send,K_CTRL)==0&&gCtrl;
                    bool isAlt =strcmp(k.send,K_ALT)==0&&gAlt;
                    bool isShift=strcmp(k.send,K_SHIFT)==0&&gShift;
                    bool active=isCtrl||isAlt||isShift;
                    float br,bg,bb;
                    if (active)       { br=0.10f; bg=0.30f; bb=0.15f; }
                    else if (special) { br=0.08f; bg=0.10f; bb=0.20f; }
                    else              { br=0.09f; bg=0.09f; bb=0.11f; }
                    drawRoundRect(kx0,ky0,kx1,ky1,br,bg,bb,1.f,0.007f);
                    int lblLen=strlen(k.label);
                    float ks=fminf(gKbRowH*0.44f,(kx1-kx0)*asp/std::max(1,lblLen)*0.72f);
                    float lx=kx0+((kx1-kx0)-lblLen*ks/asp)*0.5f;
                    float ly=ky0+((ky1-ky0)-ks)*0.5f;
                    float tr=active?0.50f:(special?0.50f:0.80f);
                    float tg=active?1.00f:(special?0.72f:0.83f);
                    float tb2=active?0.50f:(special?1.00f:0.86f);
                    Text::draw(k.label,lx,ly,ks,tr,tg,tb2);
                    x+=kw;
                }
            }
            std::lock_guard<std::mutex> lk(gFlashMtx);
            for (auto it=gFlashes.begin(); it!=gFlashes.end();) {
                float age=now-it->t;
                if (age>FLASH_DUR) { it=gFlashes.erase(it); continue; }
                float u=age/FLASH_DUR;
                float a=(1.f-u)*(1.f-u);
                drawRoundRect(it->kx0,it->ky0,it->kx1,it->ky1,0.55f,0.85f,0.65f,a*0.55f,0.007f);
                ++it;
            }
        }

        // ── floating buttons ──────────────────────────────────────────────
        float bx0,by0,bx1,by1;
        float bs=fminf(0.018f,0.6f*asp/3);

        scrollBtnBounds(true,bx0,by0,bx1,by1);
        drawRoundRect(bx0,by0,bx1,by1,0.10f,0.12f,0.18f,0.85f,0.008f);
        Text::draw("^",bx0+((bx1-bx0)-bs/asp)*0.5f,by0+((by1-by0)-bs)*0.5f,bs,0.50f,0.65f,1.00f);

        scrollBtnBounds(false,bx0,by0,bx1,by1);
        drawRoundRect(bx0,by0,bx1,by1,0.10f,0.12f,0.18f,0.85f,0.008f);
        Text::draw("v",bx0+((bx1-bx0)-bs/asp)*0.5f,by0+((by1-by0)-bs)*0.5f,bs,0.50f,0.65f,1.00f);

        kbBtnBounds(bx0,by0,bx1,by1);
        drawRoundRect(bx0,by0,bx1,by1,0.10f,0.15f,0.10f,0.85f,0.008f);
        float kbs=fminf(0.016f,0.6f*asp/4);
        const char* kblbl=gKbVisible?"[KB]":"[kb]";
        Text::draw(kblbl,bx0+((bx1-bx0)-4*kbs/asp)*0.5f,by0+((by1-by0)-kbs)*0.5f,kbs,
                   gKbVisible?0.40f:0.25f,gKbVisible?0.90f:0.55f,gKbVisible?0.40f:0.25f);

        // ── confirm exit ──────────────────────────────────────────────────
        if (gScreen==ScreenStatus::CONFIRM_EXIT) {
            drawRect(0.f,0.f,1.f,1.f,0.f,0.f,0.f,0.65f);
            drawRoundRect(0.08f,0.36f,0.92f,0.66f,0.08f,0.09f,0.11f,0.97f,0.016f);
            float qs=fminf(0.026f,0.75f*asp/16);
            float qw=14*qs/asp;
            Text::draw("EXIT TERMINAL?",0.5f-qw*0.5f,0.41f,qs,0.90f,0.92f,1.00f);
            drawRoundRect(0.10f,0.51f,0.44f,0.61f,0.12f,0.28f,0.12f,0.95f,0.012f);
            float ys=fminf(0.028f,0.45f*asp/3);
            Text::draw("YES",0.27f-1.5f*ys/asp,0.535f,ys,1.f,1.f,1.f);
            drawRoundRect(0.56f,0.51f,0.90f,0.61f,0.28f,0.10f,0.10f,0.95f,0.012f);
            float ns=fminf(0.028f,0.45f*asp/2);
            Text::draw("NO",0.73f-ns/asp,0.535f,ns,1.f,1.f,1.f);
            float hs=fminf(0.018f,0.75f*asp/24);
            Text::draw("VOL to cancel",0.5f-6.5f*hs/asp,0.635f,hs,0.30f,0.32f,0.42f);
        }
    });

    s.run();
    if (gMasterFd>=0) close(gMasterFd);
    kill(gShellPid,SIGKILL);
    waitpid(gShellPid,nullptr,WNOHANG);
    return 0;
}
