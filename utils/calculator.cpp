#include "Stratum.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <functional>

static float mono_now(){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec+ts.tv_nsec/1e9f;
}

// ── GL ────────────────────────────────────────────────────────────────────────

static const char* VSH=R"(attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);})";
static const char* FSH=R"(precision mediump float;uniform vec4 color;void main(){gl_FragColor=color;})";
static GLint gPosLoc=-1,gColorLoc=-1;

static GLuint compileShader(GLenum t,const char* src){
    GLuint sh=glCreateShader(t);glShaderSource(sh,1,&src,nullptr);glCompileShader(sh);return sh;
}
static void drawRect(float x0,float y0,float x1,float y1,float r,float g,float b,float a=1.f){
    float v[]={x0*2-1,1-y0*2,x1*2-1,1-y0*2,x0*2-1,1-y1*2,x1*2-1,1-y1*2};
    glUniform4f(gColorLoc,r,g,b,a);
    glVertexAttribPointer(gPosLoc,2,GL_FLOAT,GL_FALSE,0,v);
    glEnableVertexAttribArray(gPosLoc);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
static void drawRR(float x0,float y0,float x1,float y1,float r,float g,float b,float a=1.f,float rad=0.010f){
    drawRect(x0+rad,y0,x1-rad,y1,r,g,b,a);
    drawRect(x0,y0+rad,x1,y1-rad,r,g,b,a);
}

// ── expression evaluator ──────────────────────────────────────────────────────
// Simple recursive descent: handles +,-,*,/,^,unary minus,
// functions: sin cos tan asin acos atan sqrt log ln abs floor ceil round factorial

static bool gDeg=true; // degree mode for trig

static double toRad(double x){ return gDeg?x*M_PI/180.0:x; }
static double fromRad(double x){ return gDeg?x*180.0/M_PI:x; }

static double factorial(double n){
    if(n<0||n!=floor(n)) return NAN;
    if(n>20) return INFINITY;
    double r=1; for(int i=2;i<=(int)n;i++) r*=i; return r;
}

struct Parser {
    const char* s; int pos; bool err;
    Parser(const char* str):s(str),pos(0),err(false){}
    char peek(){ while(s[pos]==' ')pos++; return s[pos]; }
    char get() { while(s[pos]==' ')pos++; return s[pos++]; }
    bool eat(char c){ if(peek()==c){get();return true;}return false; }

    double parseExpr();
    double parseTerm();
    double parsePow();
    double parseUnary();
    double parsePostfix();
    double parsePrimary();
};

double Parser::parseExpr(){
    double v=parseTerm();
    while(!err){
        char c=peek();
        if(c=='+'){ get(); v+=parseTerm(); }
        else if(c=='-'){ get(); v-=parseTerm(); }
        else break;
    }
    return v;
}
double Parser::parseTerm(){
    double v=parsePow();
    while(!err){
        char c=peek();
        if(c=='*'){ get(); v*=parsePow(); }
        else if(c=='/'){ get(); double d=parsePow(); v=(d==0?NAN:v/d); }
        else if(c=='%'){ get(); double d=parsePow(); v=(d==0?NAN:fmod(v,d)); }
        else break;
    }
    return v;
}
double Parser::parsePow(){
    double v=parseUnary();
    if(peek()=='^'){ get(); double e=parseUnary(); v=pow(v,e); }
    return v;
}
double Parser::parseUnary(){
    if(peek()=='-'){ get(); return -parsePostfix(); }
    if(peek()=='+'){ get(); return  parsePostfix(); }
    return parsePostfix();
}
double Parser::parsePostfix(){
    double v=parsePrimary();
    if(peek()=='!'){ get(); v=factorial(v); }
    return v;
}
double Parser::parsePrimary(){
    char c=peek();
    // number
    if((c>='0'&&c<='9')||c=='.'){
        char buf[64]; int i=0;
        while((s[pos]>='0'&&s[pos]<='9')||s[pos]=='.') buf[i++]=s[pos++];
        buf[i]=0; return atof(buf);
    }
    // parentheses
    if(c=='('){ get(); double v=parseExpr(); eat(')'); return v; }
    // constants
    if(s[pos]=='p'&&s[pos+1]=='i'){ pos+=2; return M_PI; }
    if(s[pos]=='e'&&!(s[pos+1]>='a'&&s[pos+1]<='z')){ pos++; return M_E; }
    // functions
    char fn[16]=""; int fi=0;
    while(s[pos]>='a'&&s[pos]<='z'&&fi<15) fn[fi++]=s[pos++];
    fn[fi]=0;
    if(fi>0){
        double arg=0;
        if(peek()=='('){ get(); arg=parseExpr(); eat(')'); }
        else arg=parseExpr();
        if(!strcmp(fn,"sin"))   return sin(toRad(arg));
        if(!strcmp(fn,"cos"))   return cos(toRad(arg));
        if(!strcmp(fn,"tan"))   return tan(toRad(arg));
        if(!strcmp(fn,"asin"))  return fromRad(asin(arg));
        if(!strcmp(fn,"acos"))  return fromRad(acos(arg));
        if(!strcmp(fn,"atan"))  return fromRad(atan(arg));
        if(!strcmp(fn,"sqrt"))  return sqrt(arg);
        if(!strcmp(fn,"log"))   return log10(arg);
        if(!strcmp(fn,"ln"))    return log(arg);
        if(!strcmp(fn,"abs"))   return fabs(arg);
        if(!strcmp(fn,"floor")) return floor(arg);
        if(!strcmp(fn,"ceil"))  return ceil(arg);
        if(!strcmp(fn,"round")) return round(arg);
        err=true; return 0;
    }
    err=true; return 0;
}

static std::string evaluate(const std::string& expr){
    if(expr.empty()) return "";
    Parser p(expr.c_str());
    double v=p.parseExpr();
    if(p.err||isnan(v)) return "ERROR";
    if(isinf(v)) return v>0?"INF":"-INF";
    // format nicely
    char buf[64];
    if(v==(long long)v&&fabs(v)<1e15) snprintf(buf,sizeof(buf),"%lld",(long long)v);
    else snprintf(buf,sizeof(buf),"%.10g",v);
    return buf;
}

// ── unit converter ────────────────────────────────────────────────────────────

struct UnitCat {
    const char* name;
    struct Unit { const char* name; double toBase; };
    std::vector<Unit> units;
};

static const UnitCat UNIT_CATS[] = {
    {"LENGTH", {
        {"mm",0.001},{"cm",0.01},{"m",1.0},{"km",1000.0},
        {"in",0.0254},{"ft",0.3048},{"yd",0.9144},{"mi",1609.344}
    }},
    {"WEIGHT", {
        {"mg",1e-6},{"g",0.001},{"kg",1.0},{"t",1000.0},
        {"oz",0.0283495},{"lb",0.453592}
    }},
    {"DATA", {
        {"bit",0.125},{"B",1.0},{"KB",1024.0},{"MB",1048576.0},
        {"GB",1073741824.0},{"TB",1099511627776.0}
    }},
    {"TEMP", {  // special case handled separately
        {"C",0},{"F",1},{"K",2}
    }},
    {"TIME", {
        {"ns",1e-9},{"us",1e-6},{"ms",0.001},{"s",1.0},
        {"min",60.0},{"hr",3600.0},{"day",86400.0}
    }},
    {"FREQ", {
        {"Hz",1.0},{"kHz",1e3},{"MHz",1e6},{"GHz",1e9}
    }},
    {"ANGLE", {
        {"deg",1.0},{"rad",180.0/M_PI},{"grad",0.9}
    }},
};
static const int UNIT_CAT_COUNT=7;

static double convertTemp(double v,int from,int to){
    // to Celsius first
    double c=v;
    if(from==1) c=(v-32)*5/9;
    else if(from==2) c=v-273.15;
    if(to==0) return c;
    if(to==1) return c*9/5+32;
    return c+273.15;
}

// ── programmer mode ───────────────────────────────────────────────────────────

enum class Base { DEC, HEX, OCT, BIN };

static std::string toBase(long long v, Base b){
    char buf[128];
    switch(b){
        case Base::HEX: snprintf(buf,sizeof(buf),"0x%llX",v); break;
        case Base::OCT: snprintf(buf,sizeof(buf),"0%llo",v);  break;
        case Base::BIN: {
            if(v==0){ return "0b0"; }
            int bits=64;
            while(bits>1&&!((v>>(bits-1))&1)) bits--;
            std::string s="0b";
            for(int i=bits-1;i>=0;i--) s+=(char)('0'+((v>>i)&1));
            return s;
        }
        default: snprintf(buf,sizeof(buf),"%lld",v); break;
    }
    return buf;
}

static long long parseBase(const std::string& s){
    if(s.size()>2&&s[0]=='0'&&s[1]=='x') return strtoll(s.c_str()+2,nullptr,16);
    if(s.size()>2&&s[0]=='0'&&s[1]=='b') return strtoll(s.c_str()+2,nullptr,2);
    if(s.size()>1&&s[0]=='0')            return strtoll(s.c_str()+1,nullptr,8);
    return strtoll(s.c_str(),nullptr,10);
}

// ── app state ─────────────────────────────────────────────────────────────────

enum class Mode { SCIENTIFIC, PROGRAMMER, CONVERTER };

struct CalcState {
    std::mutex mtx;
    Mode mode = Mode::SCIENTIFIC;

    // scientific
    std::string expr;
    std::string result;
    std::deque<std::string> history; // "expr = result"
    static const int MAX_HIST = 6;

    // programmer
    std::string progInput;
    Base progBase = Base::DEC;

    // converter
    int  convCat  = 0;
    int  convFrom = 0;
    int  convTo   = 1;
    std::string convInput;
    std::string convResult;

    void sciPress(const char* lbl){
        // special keys
        if(!strcmp(lbl,"CLR"))  { expr=""; result=""; return; }
        if(!strcmp(lbl,"DEL"))  { if(!expr.empty()) expr.pop_back(); result=evaluate(expr); return; }
        if(!strcmp(lbl,"="))    {
            std::string r=evaluate(expr);
            if(!r.empty()&&r!="ERROR"){
                std::string entry=expr+" = "+r;
                history.push_front(entry);
                if((int)history.size()>MAX_HIST) history.pop_back();
                expr=r; result="";
            } else { result=r; }
            return;
        }
        if(!strcmp(lbl,"DEG")||!strcmp(lbl,"RAD")){ gDeg=!gDeg; return; }
        if(!strcmp(lbl,"ANS")){ if(!history.empty()){ auto eq=history[0]; auto p=eq.rfind('='); if(p!=std::string::npos) expr+=eq.substr(p+2); } return; }
        // append
        if(!strcmp(lbl,"pi"))  expr+="pi";
        else if(!strcmp(lbl,"e"))   expr+="e";
        else if(!strcmp(lbl,"x^y")) expr+="^";
        else if(!strcmp(lbl,"x^2")) expr+="^2";
        else if(!strcmp(lbl,"1/x")) { expr="1/("+expr+")"; }
        else if(!strcmp(lbl,"sqrt"))expr+="sqrt(";
        else if(!strcmp(lbl,"log")) expr+="log(";
        else if(!strcmp(lbl,"ln"))  expr+="ln(";
        else if(!strcmp(lbl,"sin")) expr+="sin(";
        else if(!strcmp(lbl,"cos")) expr+="cos(";
        else if(!strcmp(lbl,"tan")) expr+="tan(";
        else if(!strcmp(lbl,"asin"))expr+="asin(";
        else if(!strcmp(lbl,"acos"))expr+="acos(";
        else if(!strcmp(lbl,"atan"))expr+="atan(";
        else if(!strcmp(lbl,"n!"))  expr+="!";
        else if(!strcmp(lbl,"mod")) expr+="%";
        else if(!strcmp(lbl,"abs")) expr+="abs(";
        else expr+=lbl;
        result=evaluate(expr);
    }

    void progPress(const char* lbl){
        if(!strcmp(lbl,"CLR"))  { progInput=""; return; }
        if(!strcmp(lbl,"DEL"))  { if(!progInput.empty()) progInput.pop_back(); return; }
        if(!strcmp(lbl,"DEC"))  { progBase=Base::DEC; return; }
        if(!strcmp(lbl,"HEX"))  { progBase=Base::HEX; return; }
        if(!strcmp(lbl,"OCT"))  { progBase=Base::OCT; return; }
        if(!strcmp(lbl,"BIN"))  { progBase=Base::BIN; return; }
        // bitwise ops — evaluate immediately
        auto getVal=[&](){ return progInput.empty()?0LL:parseBase(progInput); };
        if(!strcmp(lbl,"AND")||!strcmp(lbl,"OR")||!strcmp(lbl,"XOR")||
           !strcmp(lbl,"NOT")||!strcmp(lbl,"SHL")||!strcmp(lbl,"SHR")){
            // store op in input as text operator for next operand — simple approach:
            // just append symbol, eval on =
            if(!strcmp(lbl,"NOT")){ progInput=toBase(~getVal(),progBase); return; }
            progInput+=std::string(" ")+lbl+" ";
            return;
        }
        if(!strcmp(lbl,"=")){
            // parse "val OP val"
            long long v=0; char op[8]=""; long long v2=0;
            char buf[256]; strncpy(buf,progInput.c_str(),255);
            char* tok=strtok(buf," ");
            std::vector<std::string> tokens;
            while(tok){ tokens.push_back(tok); tok=strtok(nullptr," "); }
            if(tokens.size()==3){
                v=parseBase(tokens[0]);
                v2=parseBase(tokens[2]);
                const char* o=tokens[1].c_str();
                if(!strcmp(o,"AND")) v&=v2;
                else if(!strcmp(o,"OR"))  v|=v2;
                else if(!strcmp(o,"XOR")) v^=v2;
                else if(!strcmp(o,"SHL")) v<<=v2;
                else if(!strcmp(o,"SHR")) v>>=v2;
                else v=v2;
            } else if(tokens.size()==1) v=parseBase(tokens[0]);
            progInput=toBase(v,progBase);
            return;
        }
        // digit input — filter by base
        if(progBase==Base::BIN&&(lbl[0]<'0'||lbl[0]>'1'||lbl[1]!=0)) return;
        if(progBase==Base::OCT&&(lbl[0]<'0'||lbl[0]>'7'||lbl[1]!=0)) return;
        if(progBase==Base::DEC){
            if(!((lbl[0]>='0'&&lbl[0]<='9')&&lbl[1]==0)) return;
        }
        if(progBase==Base::HEX){
            bool ok=(lbl[0]>='0'&&lbl[0]<='9')||(lbl[0]>='A'&&lbl[0]<='F');
            if(!ok||lbl[1]!=0) return;
        }
        // strip leading base prefix before appending digit
        if(!progInput.empty()&&progInput.find(' ')==std::string::npos){
            // if currently showing a base-converted result, reset
            if((progInput[0]=='0'&&progInput.size()>1&&
               (progInput[1]=='x'||progInput[1]=='b'||
               (progInput[1]>='0'&&progInput[1]<='7'))))
                progInput="";
        }
        progInput+=lbl;
    }

    void convPress(const char* lbl){
        if(!strcmp(lbl,"CLR"))  { convInput=""; convResult=""; return; }
        if(!strcmp(lbl,"DEL"))  { if(!convInput.empty()) convInput.pop_back(); updateConv(); return; }
        if(lbl[0]>='0'&&lbl[0]<='9'&&lbl[1]==0){ convInput+=lbl; updateConv(); return; }
        if(!strcmp(lbl,".")){ if(convInput.find('.')==std::string::npos) convInput+='.'; updateConv(); return; }
        if(!strcmp(lbl,"FROM-")){ convFrom=(convFrom-1+(int)UNIT_CATS[convCat].units.size())%(int)UNIT_CATS[convCat].units.size(); updateConv(); return; }
        if(!strcmp(lbl,"FROM+")){ convFrom=(convFrom+1)%(int)UNIT_CATS[convCat].units.size(); updateConv(); return; }
        if(!strcmp(lbl,"TO-"))  { convTo=(convTo-1+(int)UNIT_CATS[convCat].units.size())%(int)UNIT_CATS[convCat].units.size(); updateConv(); return; }
        if(!strcmp(lbl,"TO+"))  { convTo=(convTo+1)%(int)UNIT_CATS[convCat].units.size(); updateConv(); return; }
        if(!strcmp(lbl,"CAT-")) { convCat=(convCat-1+UNIT_CAT_COUNT)%UNIT_CAT_COUNT; convFrom=0; convTo=1; convInput=""; convResult=""; return; }
        if(!strcmp(lbl,"CAT+")) { convCat=(convCat+1)%UNIT_CAT_COUNT; convFrom=0; convTo=1; convInput=""; convResult=""; return; }
        if(!strcmp(lbl,"SWAP")) { int t=convFrom; convFrom=convTo; convTo=t; updateConv(); return; }
    }

    void updateConv(){
        if(convInput.empty()){ convResult=""; return; }
        double v=atof(convInput.c_str());
        const UnitCat& cat=UNIT_CATS[convCat];
        double result2;
        if(convCat==3){ // TEMP
            result2=convertTemp(v,convFrom,convTo);
        } else {
            double base=v*cat.units[convFrom].toBase;
            result2=base/cat.units[convTo].toBase;
        }
        char buf[64];
        if(result2==(long long)result2&&fabs(result2)<1e12)
            snprintf(buf,sizeof(buf),"%lld",(long long)result2);
        else snprintf(buf,sizeof(buf),"%.8g",result2);
        convResult=buf;
    }
};

static CalcState gCalc;

// ── button layout ─────────────────────────────────────────────────────────────

struct Btn {
    const char* lbl;
    float r,g,b;  // bg color
    float tr,tg,tb; // text color
    float w; // relative width
};

// scientific layout — 5 columns, 7 rows
static const Btn SCI_BTNS[][5] = {
    {{"sin",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"cos",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"tan",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"asin",0.08f,0.10f,0.18f,0.35f,0.55f,0.85f,1},{"acos",0.08f,0.10f,0.18f,0.35f,0.55f,0.85f,1}},
    {{"atan",0.08f,0.10f,0.18f,0.35f,0.55f,0.85f,1},{"log",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"ln",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"sqrt",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"n!",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1}},
    {{"x^y",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"x^2",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"1/x",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"abs",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"mod",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1}},
    {{"pi",0.08f,0.12f,0.15f,0.55f,0.85f,0.55f,1},{"e",0.08f,0.12f,0.15f,0.55f,0.85f,0.55f,1},{"(",0.09f,0.09f,0.11f,0.80f,0.83f,0.86f,1},{")",0.09f,0.09f,0.11f,0.80f,0.83f,0.86f,1},{"ANS",0.08f,0.12f,0.15f,0.55f,0.85f,0.55f,1}},
    {{"7",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"8",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"9",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"DEL",0.20f,0.08f,0.08f,1.0f,0.45f,0.45f,1},{"CLR",0.22f,0.06f,0.06f,1.0f,0.35f,0.35f,1}},
    {{"4",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"5",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"6",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"*",0.10f,0.12f,0.20f,0.55f,0.70f,1.0f,1},{"/",0.10f,0.12f,0.20f,0.55f,0.70f,1.0f,1}},
    {{"1",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"2",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"3",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"+",0.10f,0.12f,0.20f,0.55f,0.70f,1.0f,1},{"-",0.10f,0.12f,0.20f,0.55f,0.70f,1.0f,1}},
    {{"DEG",0.08f,0.14f,0.10f,0.40f,1.0f,0.40f,1},{"0",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{".",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"=",0.10f,0.28f,0.10f,0.40f,1.0f,0.40f,2}},
};
static const int SCI_ROWS=8;
static const int SCI_COLS[]={5,5,5,5,5,5,5,4};

// programmer layout
static const Btn PROG_BTNS[][5]={
    {{"HEX",0.08f,0.10f,0.20f,0.40f,0.60f,1.0f,1},{"DEC",0.08f,0.10f,0.20f,0.40f,0.60f,1.0f,1},{"OCT",0.08f,0.10f,0.20f,0.40f,0.60f,1.0f,1},{"BIN",0.08f,0.10f,0.20f,0.40f,0.60f,1.0f,1},{"NOT",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1}},
    {{"AND",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1},{"OR",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1},{"XOR",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1},{"SHL",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1},{"SHR",0.10f,0.10f,0.18f,0.75f,0.55f,1.0f,1}},
    {{"A",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1},{"B",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1},{"C",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1},{"D",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1},{"E",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1}},
    {{"F",0.09f,0.09f,0.14f,0.75f,0.85f,1.0f,1},{"7",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"8",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"9",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"DEL",0.20f,0.08f,0.08f,1.0f,0.45f,0.45f,1}},
    {{"CLR",0.22f,0.06f,0.06f,1.0f,0.35f,0.35f,1},{"4",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"5",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"6",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"=",0.10f,0.28f,0.10f,0.40f,1.0f,0.40f,1}},
    {{"1",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"2",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"3",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"0",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"",0,0,0,0,0,0,0}},
};
static const int PROG_ROWS=6;
static const int PROG_COLS[]={5,5,5,5,5,5};

// converter layout
static const Btn CONV_BTNS[][4]={
    {{"CAT-",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"CAT+",0.08f,0.10f,0.18f,0.45f,0.65f,1.0f,1},{"FROM-",0.08f,0.10f,0.18f,0.55f,0.75f,0.55f,1},{"FROM+",0.08f,0.10f,0.18f,0.55f,0.75f,0.55f,1}},
    {{"TO-",0.08f,0.10f,0.18f,1.0f,0.70f,0.35f,1},{"TO+",0.08f,0.10f,0.18f,1.0f,0.70f,0.35f,1},{"SWAP",0.10f,0.10f,0.18f,1.0f,0.80f,0.20f,1},{"CLR",0.22f,0.06f,0.06f,1.0f,0.35f,0.35f,1}},
    {{"7",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"8",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"9",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"DEL",0.20f,0.08f,0.08f,1.0f,0.45f,0.45f,1}},
    {{"4",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"5",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"6",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{".",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1}},
    {{"1",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"2",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"3",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1},{"0",0.09f,0.09f,0.12f,0.90f,0.90f,0.95f,1}},
};
static const int CONV_ROWS=5;
static const int CONV_COLS[]={4,4,4,4,4};

// ── flash ─────────────────────────────────────────────────────────────────────

struct Flash{ float x0,y0,x1,y1,t; };
static std::vector<Flash> gFlashes;
static std::mutex gFlashMtx;
static const float FLASH_DUR=0.22f;

// ── hit test ──────────────────────────────────────────────────────────────────

static bool hitBtn(float tx,float ty,
                   const Btn* rows[],const int* colCounts,int nRows,
                   float gridY0,float gridY1,
                   int& outRow,int& outCol,
                   float& bx0,float& by0,float& bx1,float& by1){
    float rowH=(gridY1-gridY0)/nRows;
    int r=(int)((ty-gridY0)/rowH);
    if(r<0||r>=nRows) return false;
    by0=gridY0+r*rowH+0.004f;
    by1=by0+rowH-0.008f;
    const Btn* row=rows[r];
    int nc=colCounts[r];
    float totalW=0; for(int i=0;i<nc;i++) totalW+=row[i].w;
    float x=0.02f,scale=0.96f/totalW;
    for(int c=0;c<nc;c++){
        float kw=row[c].w*scale;
        if(tx>=x&&tx<x+kw){
            bx0=x+0.003f; bx1=x+kw-0.003f;
            outRow=r; outCol=c; return true;
        }
        x+=kw;
    }
    return false;
}

// ── draw grid ─────────────────────────────────────────────────────────────────

static void drawGrid(float asp,
                     const Btn* rows[],const int* colCounts,int nRows,
                     float gridY0,float gridY1,
                     const char* activeBase=nullptr){
    float rowH=(gridY1-gridY0)/nRows;
    for(int r=0;r<nRows;r++){
        const Btn* row=rows[r];
        int nc=colCounts[r];
        float totalW=0; for(int i=0;i<nc;i++) totalW+=row[i].w;
        float x=0.02f,scale=0.96f/totalW;
        float ry0=gridY0+r*rowH;
        for(int c=0;c<nc;c++){
            const Btn& b=row[c];
            if(!b.lbl||!b.lbl[0]){ x+=b.w*scale; continue; }
            float kw=b.w*scale;
            float kx0=x+0.003f,kx1=x+kw-0.003f;
            float ky0=ry0+0.004f,ky1=ry0+rowH-0.004f;
            bool active=(activeBase&&!strcmp(b.lbl,activeBase));
            float br=active?b.r*2.5f:b.r;
            float bg=active?b.g*2.5f:b.g;
            float bb2=active?b.b*2.5f:b.b;
            drawRR(kx0,ky0,kx1,ky1,
                   fminf(br,0.5f),fminf(bg,0.5f),fminf(bb2,0.5f),1.f,0.007f);
            // top highlight
            drawRect(kx0+0.005f,ky0+0.003f,kx1-0.005f,ky0+0.006f,1.f,1.f,1.f,0.10f);
            int lblLen=strlen(b.lbl);
            float ks=fminf(rowH*0.42f,(kx1-kx0)*asp/fmaxf(1,lblLen)*0.72f);
            float lx=kx0+((kx1-kx0)-lblLen*ks/asp)*0.5f;
            float ly=ky0+((ky1-ky0)-ks)*0.5f;
            Text::draw(b.lbl,lx,ly,ks,b.tr,b.tg,b.tb);
            x+=kw;
        }
    }

    // draw flashes
    std::lock_guard<std::mutex> lk(gFlashMtx);
    float now=mono_now();
    for(auto it=gFlashes.begin();it!=gFlashes.end();){
        float age=now-it->t;
        if(age>FLASH_DUR){it=gFlashes.erase(it);continue;}
        float u=age/FLASH_DUR;
        float a=(1.f-u)*(1.f-u)*0.55f;
        drawRR(it->x0,it->y0,it->x1,it->y1,0.60f,0.90f,0.70f,a,0.007f);
        ++it;
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int,char**){
    Stratum s;
    if(!s.init()) return 1;
    Text::init(s.aspect());

    GLuint vs=compileShader(GL_VERTEX_SHADER,VSH);
    GLuint fs=compileShader(GL_FRAGMENT_SHADER,FSH);
    GLuint prog=glCreateProgram();
    glAttachShader(prog,vs);glAttachShader(prog,fs);
    glLinkProgram(prog);glUseProgram(prog);
    gPosLoc=glGetAttribLocation(prog,"pos");
    gColorLoc=glGetUniformLocation(prog,"color");
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    // layout constants
    const float HDR_H    = 0.048f;  // mode tab bar height
    const float DISP_H   = 0.165f;  // display area height
    const float DISP_Y0  = HDR_H;
    const float DISP_Y1  = DISP_Y0+DISP_H;
    const float GRID_Y0  = DISP_Y1+0.006f;
    const float GRID_Y1  = 0.988f;

    s.onKey([&](const KeyEvent& e){
        if(e.action!=KeyAction::DOWN&&e.action!=KeyAction::REPEAT) return;
        if(e.code==KEY_POWER){ s.stop(); return; }
        std::lock_guard<std::mutex> lk(gCalc.mtx);
        if(e.code==KEY_VOLUMEUP){
            int m=(int)gCalc.mode; m=(m-1+3)%3;
            gCalc.mode=(Mode)m;
        }
        if(e.code==KEY_VOLUMEDOWN){
            int m=(int)gCalc.mode; m=(m+1)%3;
            gCalc.mode=(Mode)m;
        }
    });

    s.onTouch([&](const TouchEvent& e){
        if(e.action!=TouchAction::DOWN) return;
        float asp=s.aspect();

        // mode tab tap
        if(e.y>=0.f&&e.y<=HDR_H){
            float tw=1.f/3;
            int tab=(int)(e.x/tw);
            std::lock_guard<std::mutex> lk(gCalc.mtx);
            gCalc.mode=(Mode)tab;
            return;
        }

        if(e.y<GRID_Y0) return;

        Mode mode; int convCat; Base progBase;
        {
            std::lock_guard<std::mutex> lk(gCalc.mtx);
            mode=gCalc.mode; convCat=gCalc.convCat;
            progBase=gCalc.progBase;
        }

        int row,col;
        float bx0,by0,bx1,by1;
        bool hit=false;
        const char* lbl=nullptr;

        if(mode==Mode::SCIENTIFIC){
            const Btn* rows[SCI_ROWS];
            for(int i=0;i<SCI_ROWS;i++) rows[i]=SCI_BTNS[i];
            hit=hitBtn(e.x,e.y,rows,SCI_COLS,SCI_ROWS,GRID_Y0,GRID_Y1,row,col,bx0,by0,bx1,by1);
            if(hit){
                int nc=SCI_COLS[row];
                float totalW=0; for(int i=0;i<nc;i++) totalW+=SCI_BTNS[row][i].w;
                // recalc col — already done in hitBtn via col
                lbl=SCI_BTNS[row][col].lbl;
                // special: DEG label toggles
                if(!strcmp(lbl,"DEG")||!strcmp(lbl,"RAD")) lbl="DEG";
            }
        } else if(mode==Mode::PROGRAMMER){
            const Btn* rows[PROG_ROWS];
            for(int i=0;i<PROG_ROWS;i++) rows[i]=PROG_BTNS[i];
            hit=hitBtn(e.x,e.y,rows,PROG_COLS,PROG_ROWS,GRID_Y0,GRID_Y1,row,col,bx0,by0,bx1,by1);
            if(hit) lbl=PROG_BTNS[row][col].lbl;
        } else {
            const Btn* rows[CONV_ROWS];
            for(int i=0;i<CONV_ROWS;i++) rows[i]=CONV_BTNS[i];
            hit=hitBtn(e.x,e.y,rows,CONV_COLS,CONV_ROWS,GRID_Y0,GRID_Y1,row,col,bx0,by0,bx1,by1);
            if(hit) lbl=CONV_BTNS[row][col].lbl;
        }

        if(hit&&lbl&&lbl[0]){
            Stratum::vibrate(14);
            {
                std::lock_guard<std::mutex> lk(gFlashMtx);
                gFlashes.push_back({bx0,by0,bx1,by1,mono_now()});
            }
            std::lock_guard<std::mutex> lk(gCalc.mtx);
            if(mode==Mode::SCIENTIFIC)  gCalc.sciPress(lbl);
            else if(mode==Mode::PROGRAMMER) gCalc.progPress(lbl);
            else gCalc.convPress(lbl);
        }
    });

    s.onFrame([&](float){
        float asp=s.aspect();
        glClearColor(0.04f,0.04f,0.06f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        // snapshot
        Mode mode; std::string expr,result; bool deg;
        std::string progInput; Base progBase;
        int convCat,convFrom,convTo; std::string convInput,convResult;
        std::deque<std::string> history;
        {
            std::lock_guard<std::mutex> lk(gCalc.mtx);
            mode=gCalc.mode; expr=gCalc.expr; result=gCalc.result;
            deg=gDeg; progInput=gCalc.progInput; progBase=gCalc.progBase;
            convCat=gCalc.convCat; convFrom=gCalc.convFrom;
            convTo=gCalc.convTo; convInput=gCalc.convInput;
            convResult=gCalc.convResult; history=gCalc.history;
        }

        // ── mode tabs ─────────────────────────────────────────────────────
        const char* tabNames[]={"SCIENTIFIC","PROGRAMMER","CONVERTER"};
        float tw=1.f/3;
        for(int i=0;i<3;i++){
            bool sel=(int)mode==i;
            drawRect(i*tw,0.f,(i+1)*tw,HDR_H,
                     sel?0.10f:0.06f, sel?0.14f:0.07f, sel?0.22f:0.08f);
            float ts=fminf(0.018f,0.60f*asp*tw/(float)strlen(tabNames[i]));
            float tw2=strlen(tabNames[i])*ts/asp;
            Text::draw(tabNames[i],i*tw+(tw-tw2)*0.5f,(HDR_H-ts)*0.5f,ts,
                       sel?0.55f:0.25f, sel?0.80f:0.35f, sel?1.0f:0.50f);
            if(i<2) drawRect((i+1)*tw-0.002f,0.004f,(i+1)*tw,HDR_H-0.004f,
                              0.12f,0.16f,0.25f);
        }
        // active tab underline
        drawRect((int)mode*tw,HDR_H-0.003f,((int)mode+1)*tw,HDR_H,
                 0.35f,0.60f,1.0f);

        // ── display area ──────────────────────────────────────────────────
        drawRect(0.f,HDR_H,1.f,DISP_Y1,0.05f,0.05f,0.07f);
        drawRect(0.f,DISP_Y1,1.f,DISP_Y1+0.002f,0.15f,0.28f,0.50f);

        if(mode==Mode::SCIENTIFIC){
            // history tape
            float hS=fminf(0.014f,0.45f*asp/30);
            for(int i=0;i<(int)history.size();i++){
                float y=DISP_Y0+0.006f+i*hS;
                if(y+hS>DISP_Y1-0.055f) break;
                float fade=1.f-i*0.18f;
                Text::draw(history[i].c_str(),0.05f,y,hS,
                           0.28f*fade,0.38f*fade,0.55f*fade);
            }
            // expression
            float exS=fminf(0.030f,0.75f*asp/fmaxf(1,(int)expr.size()));
            exS=fminf(exS,0.030f);
            float exW=expr.size()*exS/asp;
            float maxExW=0.90f;
            if(exW>maxExW) exS*=maxExW/exW;
            float exY=DISP_Y1-0.055f;
            Text::draw(expr.empty()?"0":expr.c_str(),
                       0.05f,exY,exS,0.75f,0.80f,0.90f);
            // result preview
            if(!result.empty()){
                float rS=fminf(0.026f,0.70f*asp/fmaxf(1,(int)result.size()));
                float rW=result.size()*rS/asp;
                if(rW>maxExW) rS*=maxExW/rW;
                float rW2=result.size()*rS/asp;
                Text::draw(result.c_str(),0.95f-rW2,DISP_Y1-0.028f,rS,
                           result=="ERROR"?1.0f:0.40f,
                           result=="ERROR"?0.30f:0.85f,
                           result=="ERROR"?0.30f:0.50f);
            }
            // DEG/RAD indicator
            float ds=fminf(0.014f,0.50f*asp/3);
            Text::draw(deg?"DEG":"RAD",0.86f,DISP_Y0+0.008f,ds,
                       0.35f,0.65f,0.35f);
        } else if(mode==Mode::PROGRAMMER){
            // show all bases
            long long val=progInput.empty()?0LL:parseBase(progInput);
            // if mid-expression with operator, just show input
            bool hasOp=progInput.find(' ')!=std::string::npos;
            const char* baseNames[]={"DEC","HEX","OCT","BIN"};
            Base bases[]={Base::DEC,Base::HEX,Base::OCT,Base::BIN};
            float bY=DISP_Y0+0.012f;
            float bRowH=(DISP_H-0.020f)/4;
            for(int i=0;i<4;i++){
                bool active=bases[i]==progBase;
                float nS=fminf(0.016f,0.45f*asp/3);
                Text::draw(baseNames[i],0.05f,bY+i*bRowH,nS,
                           active?0.55f:0.25f,active?0.85f:0.35f,active?1.0f:0.50f);
                std::string shown=hasOp?progInput:toBase(val,bases[i]);
                float vS=fminf(0.018f,0.65f*asp/fmaxf(1,(int)shown.size()));
                float vW=shown.size()*vS/asp;
                float maxVW=0.70f;
                if(vW>maxVW) vS*=maxVW/vW;
                float vW2=shown.size()*vS/asp;
                Text::draw(shown.c_str(),0.95f-vW2,bY+i*bRowH+(nS-vS)*0.5f,vS,
                           active?0.90f:0.45f,active?0.95f:0.50f,active?1.0f:0.55f);
            }
        } else {
            // converter display
            const UnitCat& cat=UNIT_CATS[convCat];
            float cS=fminf(0.022f,0.65f*asp/12);
            Text::draw(cat.name,0.05f,DISP_Y0+0.010f,cS,0.45f,0.65f,1.0f);

            float fromS=fminf(0.018f,0.55f*asp/8);
            const char* fromName=cat.units[convFrom].name;
            const char* toName  =cat.units[convTo].name;

            // from
            Text::draw("FROM",0.05f,DISP_Y0+0.010f+cS+0.008f,fromS*0.75f,0.30f,0.45f,0.65f);
            std::string fromDisp=convInput.empty()?"0":convInput;
            fromDisp+=" "+std::string(fromName);
            float fdS=fminf(0.028f,0.65f*asp/fmaxf(1,(int)fromDisp.size()));
            float fdW=fromDisp.size()*fdS/asp;
            float maxFW=0.85f;
            if(fdW>maxFW) fdS*=maxFW/fdW;
            Text::draw(fromDisp.c_str(),0.05f,DISP_Y0+0.010f+cS+0.008f+fromS*0.75f+0.004f,
                       fdS,0.80f,0.88f,1.0f);

            // to
            float toY=DISP_Y0+0.010f+cS+0.008f+fromS*0.75f+0.004f+fdS+0.010f;
            Text::draw("TO",0.05f,toY,fromS*0.75f,0.30f,0.55f,0.35f);
            std::string toDisp=(convResult.empty()?"0":convResult)+" "+std::string(toName);
            float tdS=fminf(0.028f,0.65f*asp/fmaxf(1,(int)toDisp.size()));
            float tdW=toDisp.size()*tdS/asp;
            if(tdW>maxFW) tdS*=maxFW/tdW;
            Text::draw(toDisp.c_str(),0.05f,toY+fromS*0.75f+0.004f,
                       tdS,0.55f,1.0f,0.55f);
        }

        // ── button grid ───────────────────────────────────────────────────
        if(mode==Mode::SCIENTIFIC){
            const Btn* rows[SCI_ROWS];
            for(int i=0;i<SCI_ROWS;i++) rows[i]=SCI_BTNS[i];
            drawGrid(asp,rows,SCI_COLS,SCI_ROWS,GRID_Y0,GRID_Y1,
                     deg?"DEG":"RAD");
        } else if(mode==Mode::PROGRAMMER){
            const Btn* rows[PROG_ROWS];
            for(int i=0;i<PROG_ROWS;i++) rows[i]=PROG_BTNS[i];
            const char* baseActive=
                progBase==Base::HEX?"HEX":
                progBase==Base::DEC?"DEC":
                progBase==Base::OCT?"OCT":"BIN";
            drawGrid(asp,rows,PROG_COLS,PROG_ROWS,GRID_Y0,GRID_Y1,baseActive);
        } else {
            const Btn* rows[CONV_ROWS];
            for(int i=0;i<CONV_ROWS;i++) rows[i]=CONV_BTNS[i];
            drawGrid(asp,rows,CONV_COLS,CONV_ROWS,GRID_Y0,GRID_Y1);
        }

        drawRect(0.f,0.996f,1.f,1.f,0.25f,0.50f,1.0f,0.7f);
    });

    s.run();
    return 0;
}
