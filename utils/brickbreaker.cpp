#include "Stratum.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mutex>
#include <vector>

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

// ── GL ────────────────────────────────────────────────────────────────────────

static const char* VSH = R"(attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);})";
static const char* FSH = R"(precision mediump float; uniform vec4 color; void main(){gl_FragColor=color;})";
static GLint gPosLoc=-1, gColorLoc=-1;

static GLuint compileShader(GLenum type, const char* src){
    GLuint sh=glCreateShader(type); glShaderSource(sh,1,&src,nullptr); glCompileShader(sh); return sh;
}

static void drawRect(float x0,float y0,float x1,float y1,
                     float r,float g,float b,float a=1.f){
    float nx0=x0*2-1,nx1=x1*2-1,ny0=1-y0*2,ny1=1-y1*2;
    float v[]={nx0,ny0,nx1,ny0,nx0,ny1,nx1,ny1};
    glUniform4f(gColorLoc,r,g,b,a);
    glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,v);
    glEnableVertexAttribArray(gPosLoc);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}

static void drawRoundRect(float x0,float y0,float x1,float y1,
                           float r,float g,float b,float a=1.f,float rad=0.010f){
    drawRect(x0+rad,y0,x1-rad,y1,r,g,b,a);
    drawRect(x0,y0+rad,x1,y1-rad,r,g,b,a);
}

// circle via triangle fan, rx/ry separate to correct for aspect
static void drawEllipse(float cx, float cy, float rx, float ry,
                         float r, float g, float b, float a=1.f, int segs=32){
    std::vector<float> v;
    v.reserve((segs+2)*2);
    float ncx=cx*2-1, ncy=1-cy*2;
    v.push_back(ncx); v.push_back(ncy);
    for(int i=0;i<=segs;i++){
        float angle=2*3.14159265f*i/segs;
        v.push_back(ncx+cosf(angle)*rx*2);
        v.push_back(ncy+sinf(angle)*ry*2);
    }
    glUniform4f(gColorLoc,r,g,b,a);
    glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,v.data());
    glEnableVertexAttribArray(gPosLoc);
    glDrawArrays(GL_TRIANGLE_FAN,0,(GLsizei)(v.size()/2));
}

// ── game constants ────────────────────────────────────────────────────────────

static const int   COLS        = 8;
static const int   ROWS        = 6;
static const float BRICK_GAP   = 0.008f;
static const float BRICK_X0    = 0.03f;
static const float BRICK_X1    = 0.97f;
static const float BRICK_Y0    = 0.10f;
static const float BRICK_H     = 0.055f;
static const float PADDLE_Y    = 0.91f;
static const float PADDLE_H    = 0.022f;
static const float PADDLE_W    = 0.18f;
static const float BALL_R      = 0.014f;
static const float BALL_SPEED  = 0.55f;  // screen units/sec
static const float PLAY_AREA_Y0= 0.08f;
static const float PLAY_AREA_Y1= 0.97f;

// brick colors by row
static const float BRICK_COLORS[ROWS][3] = {
    {1.00f, 0.30f, 0.30f},
    {1.00f, 0.65f, 0.20f},
    {1.00f, 0.95f, 0.20f},
    {0.35f, 0.90f, 0.35f},
    {0.25f, 0.70f, 1.00f},
    {0.75f, 0.35f, 1.00f},
};

// ── game state ────────────────────────────────────────────────────────────────

struct Brick { bool alive; int hits; };  // hits remaining

struct Ball {
    float x, y, vx, vy;
    bool active;
};

struct Particle {
    float x, y, vx, vy, life, maxLife;
    float r, g, b;
};

struct GameState {
    std::mutex mtx;

    Brick  bricks[ROWS][COLS];
    Ball   ball;
    float  paddleX;   // center x
    int    score;
    int    highScore;
    bool   launched;  // ball sitting on paddle waiting for tap
    float  lastT;

    std::vector<Particle> particles;

    // screen flash on ball loss
    float flashT = -1.f;

    GameState(){
        highScore = 0;
        reset();
    }

    void resetBricks(){
        for(int r=0;r<ROWS;r++)
            for(int c=0;c<COLS;c++){
                bricks[r][c].alive = true;
                // top rows take 2 hits, rest take 1
                bricks[r][c].hits  = (r < 2) ? 2 : 1;
            }
    }

    void resetBall(){
        ball.x  = paddleX;
        ball.y  = PADDLE_Y - BALL_R - 0.002f;
        ball.vx = 0.f;
        ball.vy = 0.f;
        ball.active = true;
        launched = false;
    }

    void reset(){
        paddleX = 0.5f;
        score   = 0;
        resetBricks();
        resetBall();
        particles.clear();
        lastT = mono_now();
    }

    bool allCleared(){
        for(int r=0;r<ROWS;r++)
            for(int c=0;c<COLS;c++)
                if(bricks[r][c].alive) return false;
        return true;
    }

    void spawnParticles(float x, float y, float pr, float pg, float pb, int n=12){
        for(int i=0;i<n;i++){
            Particle p;
            p.x=x; p.y=y;
            float angle = (rand()%628)/100.f;
            float speed = 0.15f + (rand()%100)/250.f;
            p.vx = cosf(angle)*speed;
            p.vy = sinf(angle)*speed * 0.5f; // squish vertically
            p.life = p.maxLife = 0.35f + (rand()%100)/400.f;
            p.r=pr; p.g=pg; p.b=pb;
            particles.push_back(p);
        }
    }

    void launch(){
        if(launched) return;
        float angle = -1.15f + (rand()%100)/195.f; // -1.15 to +1.15 rad from straight up
        ball.vx = BALL_SPEED * sinf(angle);
        ball.vy = -BALL_SPEED * cosf(angle);
        // ensure minimum horizontal movement
        if(fabsf(ball.vx) < 0.08f)
            ball.vx = ball.vx < 0 ? -0.08f : 0.08f;
        launched = true;
    }

    void brickRect(int r, int c, float& x0, float& y0, float& x1, float& y1){
        float bw = (BRICK_X1 - BRICK_X0 - BRICK_GAP*(COLS+1)) / COLS;
        x0 = BRICK_X0 + BRICK_GAP + c*(bw+BRICK_GAP);
        x1 = x0 + bw;
        y0 = BRICK_Y0 + BRICK_GAP + r*(BRICK_H+BRICK_GAP);
        y1 = y0 + BRICK_H;
    }

    void update(float now){
        float dt = now - lastT;
        lastT = now;
        if(dt > 0.05f) dt = 0.05f;

        // update particles
        for(auto& p : particles){
            p.x += p.vx*dt;
            p.y += p.vy*dt;
            p.life -= dt;
        }
        particles.erase(
            std::remove_if(particles.begin(),particles.end(),
                [](const Particle& p){ return p.life<=0; }),
            particles.end());

        if(!launched){
            // sit on paddle
            ball.x = paddleX;
            ball.y = PADDLE_Y - BALL_R - 0.002f;
            return;
        }

        // move ball
        float nx = ball.x + ball.vx*dt;
        float ny = ball.y + ball.vy*dt;

        // wall bounce L/R
        if(nx - BALL_R < 0.f){ nx = BALL_R; ball.vx = fabsf(ball.vx); }
        if(nx + BALL_R > 1.f){ nx = 1.f-BALL_R; ball.vx = -fabsf(ball.vx); }

        // ceiling bounce
        if(ny - BALL_R < PLAY_AREA_Y0){
            ny = PLAY_AREA_Y0 + BALL_R;
            ball.vy = fabsf(ball.vy);
        }

        // lost ball
        if(ny - BALL_R > PLAY_AREA_Y1){
            if(score > highScore) highScore = score;
            flashT = now;
            spawnParticles(ball.x, PLAY_AREA_Y1-0.02f, 1.f,0.3f,0.3f, 20);
            resetBall();
            return;
        }

        // paddle collision
        float py0 = PADDLE_Y - PADDLE_H*0.5f;
        float py1 = PADDLE_Y + PADDLE_H*0.5f;
        float px0 = paddleX - PADDLE_W*0.5f;
        float px1 = paddleX + PADDLE_W*0.5f;
        if(ball.vy > 0 && ny+BALL_R >= py0 && ny-BALL_R <= py1
           && nx >= px0 && nx <= px1){
            ny = py0 - BALL_R;
            ball.vy = -fabsf(ball.vy);
            // angle based on hit position relative to paddle center
            float rel = (nx - paddleX) / (PADDLE_W*0.5f); // -1..1
            ball.vx = rel * BALL_SPEED * 0.9f;
            // keep speed constant
            float spd = sqrtf(ball.vx*ball.vx + ball.vy*ball.vy);
            float scale = BALL_SPEED / spd;
            ball.vx *= scale; ball.vy *= scale;
            Stratum::vibrate(8);
        }

        // brick collisions
        for(int r=0;r<ROWS;r++){
            for(int c=0;c<COLS;c++){
                if(!bricks[r][c].alive) continue;
                float bx0,by0,bx1,by1;
                brickRect(r,c,bx0,by0,bx1,by1);

                float closestX = fmaxf(bx0, fminf(nx, bx1));
                float closestY = fmaxf(by0, fminf(ny, by1));
                float dx = nx-closestX, dy = ny-closestY;
                if(dx*dx + dy*dy > BALL_R*BALL_R) continue;

                // which face?
                float overlapL = nx+BALL_R - bx0;
                float overlapR = bx1 - (nx-BALL_R);
                float overlapT = ny+BALL_R - by0;
                float overlapB = by1 - (ny-BALL_R);
                float minH = fminf(overlapL, overlapR);
                float minV = fminf(overlapT, overlapB);

                if(minH < minV){
                    ball.vx = (overlapL < overlapR) ? -fabsf(ball.vx) : fabsf(ball.vx);
                } else {
                    ball.vy = (overlapT < overlapB) ? -fabsf(ball.vy) : fabsf(ball.vy);
                }

                bricks[r][c].hits--;
                if(bricks[r][c].hits <= 0){
                    bricks[r][c].alive = false;
                    score += (ROWS - r) * 10;
                    float mx=(bx0+bx1)*0.5f, my=(by0+by1)*0.5f;
                    spawnParticles(mx,my,
                        BRICK_COLORS[r][0],BRICK_COLORS[r][1],BRICK_COLORS[r][2]);
                    Stratum::vibrate(14);
                } else {
                    Stratum::vibrate(6);
                }

                // only one brick per frame to avoid tunnelling weirdness
                goto done_bricks;
            }
        }
        done_bricks:

        ball.x = nx;
        ball.y = ny;

        // all bricks cleared — respawn
        if(allCleared()){
            resetBricks();
            resetBall();
            score += 500; // bonus
        }
    }
};

static GameState gGame;

// ── main ──────────────────────────────────────────────────────────────────────

int main(int,char**){
    srand((unsigned)time(nullptr));

    Stratum s;
    if(!s.init()) return 1;
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
        if(e.code==KEY_POWER) s.stop();
    });

    s.onTouch([&](const TouchEvent& e){
        std::lock_guard<std::mutex> lk(gGame.mtx);
        if(e.action==TouchAction::DOWN || e.action==TouchAction::MOVE){
            // drag paddle
            float newX = fmaxf(PADDLE_W*0.5f+0.01f,
                               fminf(1.f-PADDLE_W*0.5f-0.01f, e.x));
            gGame.paddleX = newX;
            // tap to launch
            if(e.action==TouchAction::DOWN && !gGame.launched)
                gGame.launch();
        }
    });

    s.onFrame([&](float){
        float now = mono_now();
        float asp = s.aspect();

        {
            std::lock_guard<std::mutex> lk(gGame.mtx);
            gGame.update(now);
        }

        // snapshot
        float paddleX, ballX, ballY, flashT;
        bool  launched;
        int   score, highScore;
        Brick bricks[ROWS][COLS];
        std::vector<Particle> parts;
        {
            std::lock_guard<std::mutex> lk(gGame.mtx);
            paddleX   = gGame.paddleX;
            ballX     = gGame.ball.x;
            ballY     = gGame.ball.y;
            launched  = gGame.launched;
            score     = gGame.score;
            highScore = gGame.highScore;
            flashT    = gGame.flashT;
            memcpy(bricks, gGame.bricks, sizeof(bricks));
            parts     = gGame.particles;
        }

        // background
        float flashAge = now - flashT;
        float flashA   = (flashT > 0 && flashAge < 0.25f)
                         ? (1.f - flashAge/0.25f) * 0.35f : 0.f;
        glClearColor(0.04f+flashA, 0.04f, 0.06f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        // top bar
        drawRect(0.f,0.f,1.f,0.004f,0.25f,0.50f,1.0f,0.9f);

        // score — each gets 50% of screen width, scales down if too wide
        char scoreBuf[32]; snprintf(scoreBuf,sizeof(scoreBuf),"SCORE  %d",score);
        char hiBuf[32];    snprintf(hiBuf,sizeof(hiBuf),"BEST  %d",highScore);
        float maxW    = 0.46f; // half screen minus margins
        float baseSS  = fminf(0.026f, 0.55f*asp/10);
        float scoreS  = fminf(baseSS, maxW*asp/(float)strlen(scoreBuf));
        float hiS     = fminf(baseSS, maxW*asp/(float)strlen(hiBuf));
        Text::draw(scoreBuf, 0.03f, 0.016f, scoreS, 0.40f,0.70f,1.00f);
        float hiW = strlen(hiBuf)*hiS/asp;
        Text::draw(hiBuf, 0.97f-hiW, 0.016f, hiS, 0.30f,0.50f,0.75f);

        // play area border
        drawRect(0.f, PLAY_AREA_Y0-0.002f, 1.f, PLAY_AREA_Y0, 0.15f,0.30f,0.55f,0.5f);

        // bricks
        for(int r=0;r<ROWS;r++){
            for(int c=0;c<COLS;c++){
                if(!bricks[r][c].alive) continue;
                float bx0,by0,bx1,by1;
                gGame.brickRect(r,c,bx0,by0,bx1,by1);
                float br=BRICK_COLORS[r][0];
                float bg=BRICK_COLORS[r][1];
                float bb=BRICK_COLORS[r][2];
                float dim = bricks[r][c].hits==2 ? 1.0f : 0.55f;
                drawRoundRect(bx0,by0,bx1,by1,br*dim,bg*dim,bb*dim,1.f,0.006f);
                // highlight top edge
                drawRect(bx0+0.006f,by0+0.004f,bx1-0.006f,by0+0.008f,
                         1.f,1.f,1.f,0.18f*dim);
                // crack indicator for 2-hit bricks at 1 hit left
                if(bricks[r][c].hits==1 && r<2){
                    float mx=(bx0+bx1)*0.5f;
                    drawRect(mx-0.002f,by0+0.010f,mx+0.002f,by1-0.010f,
                             0.f,0.f,0.f,0.35f);
                }
            }
        }

        // particles
        for(auto& p : parts){
            float u = p.life/p.maxLife;
            float sz = 0.010f*u;
            drawRect(p.x-sz,p.y-sz,p.x+sz,p.y+sz,p.r,p.g,p.b,u*0.9f);
        }

        // paddle
        float px0 = paddleX - PADDLE_W*0.5f;
        float px1 = paddleX + PADDLE_W*0.5f;
        float py0 = PADDLE_Y - PADDLE_H*0.5f;
        float py1 = PADDLE_Y + PADDLE_H*0.5f;
        drawRoundRect(px0,py0,px1,py1,0.30f,0.70f,1.00f,1.f,0.009f);
        // paddle shine
        drawRect(px0+0.010f,py0+0.003f,px1-0.010f,py0+0.007f,1.f,1.f,1.f,0.25f);

        // ball — proper circle corrected for aspect
        float brad = BALL_R;
        drawEllipse(ballX, ballY, brad, brad*asp, 0.95f,0.95f,1.00f, 1.f);
        // inner highlight
        drawEllipse(ballX-brad*0.25f, ballY-brad*0.3f,
                    brad*0.38f, brad*0.38f*asp, 1.f,1.f,1.f, 0.55f, 16);
        // glow
        drawEllipse(ballX, ballY, brad*1.9f, brad*1.9f*asp, 0.50f,0.70f,1.00f, 0.14f);

        // launch hint
        if(!launched){
            float hs = fminf(0.018f,0.65f*asp/16);
            float pulse = 0.6f + 0.4f*sinf(now*4.f);
            Text::draw("TAP TO LAUNCH",
                       0.5f - 6.5f*hs/asp, PADDLE_Y+0.040f,
                       hs, 0.55f*pulse,0.75f*pulse,1.00f*pulse);
        }

        // hint footer
        float fs2 = fminf(0.015f,0.60f*asp/14);
        Text::draw("DRAG  move paddle     PWR  exit",
                   0.05f, 0.975f, fs2, 0.18f,0.20f,0.28f);

        drawRect(0.f,0.996f,1.f,1.f,0.25f,0.50f,1.0f,0.7f);
    });

    s.run();
    return 0;
}
