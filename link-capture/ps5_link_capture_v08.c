/*
 * PS5 Link Capture v0.8 — autonomous LAN web UI + Overdrive capture protocol.
 * Copyright 2026 PS5 Link Capture contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Contains NO Overdrive source here: the ELF is built from its original sources
 * in the fork's CI, then linked as an embedded read-only byte array.
 * The corresponding source of that embedded helper must accompany distribution.
 *
 * Real PS5 test REQUIRED. HOST_SIM supports deterministic mock ELF-loader tests.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern const unsigned char ps5_capture_elf[];
extern const size_t ps5_capture_elf_len;

#define HTTP_PORT 8765
#define ELF_PORT 9021
#define MAX_HTTP 8192
#define MAX_PAGE 512
#define MAX_CANDIDATE 2048
#define CAPTURE_TIMEOUT 180
#define IO_TIMEOUT 200

#ifndef HOST_SIM
int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char *, void *);
static int open_browser(const char *url) { return sceSystemServiceLaunchWebBrowser(url, NULL); }
#else
static int open_browser(const char *url) { fprintf(stderr, "HOST_SIM launch: %s\n", url); return 0; }
#endif

typedef enum { IDLE, CONNECTING, PREPARING, ARMED, OPENING, WAITING, CAPTURED, FAILED } Stage;
static const char *const stage_names[] = {
    "idle", "connecting", "preparing", "armed", "opening", "waiting", "captured", "failed"
};
static struct {
    pthread_mutex_t lock;
    Stage stage;
    unsigned long job_id;
    int launch_pending, launch_rc, active, worker_socket;
    char page[MAX_PAGE];
    char filename[156];
    char candidate[MAX_CANDIDATE];
    char error[240];
} state = { .lock = PTHREAD_MUTEX_INITIALIZER, .stage = IDLE, .launch_rc = -999 };
static volatile sig_atomic_t running = 1;
static int capture_port = ELF_PORT;

static const char page_html[] =
"<!doctype html><html lang='ar' dir='rtl'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>PS5 Link Capture v0.8</title><style>"
"body{font-family:system-ui,sans-serif;max-width:720px;margin:6vh auto;padding:20px;background:#0b142a;color:#f3f6ff}"
"main{background:#1a2947;border-radius:18px;padding:24px}input,button{box-sizing:border-box;width:100%;border-radius:9px;padding:14px;margin:9px 0;font:inherit}"
"input{direction:ltr;text-align:left;border:0}button{background:#5389ff;color:white;border:0;cursor:pointer}button:disabled{opacity:.5}"
".result{direction:ltr;text-align:left;overflow-wrap:anywhere;background:#101e38;padding:12px;border-radius:8px}"
"small{color:#bacce8}#err{color:#ffb5b5}#capture{display:none}a{color:#b7d5ff}</style></head><body><main>"
"<h2>🎮 PS5 Link Capture v0.8</h2><p>أرسل رابط Vikingfile من هاتفك. سيبدأ PS5 الالتقاط أولاً، ثم يفتح المتصفح تلقائياً لحل الكابتشا يدوياً.</p>"
"<form id='frm'><input id='url' value='https://vik1ngfile.site/f/jTxcioO1oV' required aria-label='Vikingfile URL'>"
"<input id='filename' placeholder='اسم الملف الكامل إن كنت تعرفه (اختياري)' maxlength='155' aria-label='Optional exact filename'>"
"<button id='start'>ابدأ التقاط الرابط وفتح المتصفح</button></form>"
"<p>الحالة: <strong id='status'>...</strong></p><p id='err'></p>"
"<div id='capture'><p>الرابط المكتشف <b>(غير متحقق من الملف المطلوب)</b>:</p><p id='result' class='result'></p>"
"<button id='copy' type='button'>نسخ الرابط المرشح</button></div>"
"<small>الموقع لا يحل الكابتشا ولا يبدأ تنزيلًا تلقائيًا. قد يكون الرابط المكتشف لملف آخر، فتحقق من الاسم والحجم والاستجابة قبل تنزيله."
" شغّل ELF Loader على 9021 أولاً، ولا تفتح منافذ الجهاز للإنترنت.</small></main>"
"<script>const $=x=>document.getElementById(x);let last='';"
"$('frm').onsubmit=async e=>{e.preventDefault();$('start').disabled=true;$('err').textContent='';"
"try{let p=JSON.stringify({url:$('url').value,filename:$('filename').value});"
"let r=await fetch('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},body:p});"
"let t=await r.text();if(!r.ok)throw Error(t);}catch(x){$('err').textContent=x.message;}finally{refresh();}};"
"$('copy').onclick=()=>{if(last&&navigator.clipboard)navigator.clipboard.writeText(last);else prompt('انسخ الرابط',last)};"
"async function refresh(){try{let r=await fetch('/api/status',{cache:'no-store'}),s=await r.json();"
"$('status').textContent=s.stage+' (طلب '+s.job+')';$('err').textContent=s.error||'';"
"$('start').disabled=s.active;"
"last=s.candidate||'';$('capture').style.display=last?'block':'none';$('result').textContent=last;"
"}catch(e){$('err').textContent='تعذر الاتصال بالخادم: '+e.message;}}setInterval(refresh,1500);refresh();</script>"
"</body></html>";

static int valid_page(const char *url) {
    const char *p;
    size_t n;
    if (!url || strncmp(url, "https://", 8) || (n = strlen(url)) >= MAX_PAGE) return 0;
    p = url + 8;
    if (!strncmp(p, "vik1ngfile.site", 15) && p[15] == '/') p += 15;
    else if (!strncmp(p, "vikingfile.com", 14) && p[14] == '/') p += 14;
    else return 0;
    if (strncmp(p, "/f/", 3)) return 0;
    p += 3;
    for (int i = 0; i < 10; i++) if (!isalnum((unsigned char)p[i])) return 0;
    return p[10] == '\0' || (p[10] == '/' && p[11] == '\0');
}
static int valid_filename(const char *name) {
    size_t len = strlen(name);
    if (len > 155 || strstr(name,"..")) return 0;
    for (size_t i=0; i<len; i++) if ((unsigned char)name[i]<32 || (unsigned char)name[i]>126 || name[i]=='/' || name[i]=='\\') return 0;
    return 1;
}
static int valid_capture(const char *url) {
    const char *p;
    size_t n;
    if (!url || (n = strlen(url)) >= MAX_CANDIDATE || strncmp(url, "https://", 8)) return 0;
    p = url + 8;
    if (!strncmp(p,"vik1ngfile.site",15) && p[15]=='/') p += 15;
    else if (!strncmp(p,"vikingfile.com",14) && p[14]=='/') p += 14;
    else return 0;
    if (strncmp(p,"/d/",3)) return 0;
    p += 3;
    for (int i=0;i<10;i++) if (!isalnum((unsigned char)p[i])) return 0;
    if (p[10]!='/' || !p[11]) return 0;
    /* Prevent newline, quote or control characters in JSON and UI. No auto-open. */
    for (const unsigned char *s=(const unsigned char *)(p+11);*s;s++)
        if (*s<33 || *s>126 || *s=='"' || *s=='\\' || *s=='<' || *s=='>') return 0;
    return 1;
}
static void set_error(unsigned long id, const char *message) {
    pthread_mutex_lock(&state.lock);
    if (id == state.job_id && state.stage!=FAILED && state.stage!=CAPTURED) {
        state.stage = FAILED;
        snprintf(state.error,sizeof state.error,"%s", message);
        state.launch_pending = 0;
    }
    pthread_mutex_unlock(&state.lock);
}
static void set_stage(unsigned long id, Stage new_stage) {
    pthread_mutex_lock(&state.lock);
    if (id == state.job_id) state.stage = new_stage;
    pthread_mutex_unlock(&state.lock);
}
static int send_all(int fd, const void *bytes, size_t n) {
    const unsigned char *p=bytes;
    while(n) { ssize_t k=send(fd,p,n,0); if(k<=0) return -1; p+=k;n-=(size_t)k; }
    return 0;
}
static int connect_loader(void) {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in dest;
    struct timeval tv={8,0};
    if (fd<0) return -1;
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    memset(&dest,0,sizeof dest);
    dest.sin_family=AF_INET;dest.sin_port=htons((uint16_t)capture_port);
    dest.sin_addr.s_addr=htonl(0x7F000001u);
    if (connect(fd,(struct sockaddr *)&dest,sizeof dest)) { close(fd);return -1; }
    return fd;
}
static void *capture_task(void *arg) {
    unsigned long id=(unsigned long)(uintptr_t)arg;
    char filename[156];
    int fd=-1, armed=0;
    char command[310], line[MAX_CANDIDATE+40];
    size_t used=0;
    struct timespec start;
    pthread_mutex_lock(&state.lock);
    snprintf(filename,sizeof filename,"%s",state.filename);
    pthread_mutex_unlock(&state.lock);
    fd=connect_loader();
    if(fd<0) {set_error(id,"Cannot reach local ELF Loader on 127.0.0.1:9021");goto finish;}
    pthread_mutex_lock(&state.lock);
    if(id==state.job_id)state.worker_socket=fd;
    pthread_mutex_unlock(&state.lock);
    /* The original helper supports BRC1, canonical /d/ prefix and optional exact filename. */
    int n=snprintf(command,sizeof command,"BRC1\nhttps://vikingfile.com/d/\n/%s\n%d\n0\nfresh\n",filename,CAPTURE_TIMEOUT);
    if(n<=0 || (size_t)n>=sizeof command || send_all(fd,ps5_capture_elf,ps5_capture_elf_len)
       || send_all(fd,command,(size_t)n)) {
        set_error(id,"Could not send the capture ELF or BRC1 request");goto done;
    }
    set_stage(id,PREPARING);
    clock_gettime(CLOCK_MONOTONIC,&start);
    while (running) {
        char chunk[4096];
        ssize_t got=recv(fd,chunk,sizeof chunk,0);
        if(got<=0){set_error(id,got==0?"Capture helper disconnected":"Capture helper stopped responding");break;}
        for(ssize_t i=0;i<got;i++) {
            char ch=chunk[i];
            if(ch!='\n') {
                if((unsigned char)ch<32 || ++used>=sizeof line-1) {
                    set_error(id,"Malformed capture helper response");goto done;
                }
                line[used-1]=ch; continue;
            }
            line[used]='\0';used=0;
            if (!strcmp(line,"preparing") || !strcmp(line,"waiting") || !strncmp(line,"known ",6)) continue;
            if (!strncmp(line,"armed ",6)) {
                armed=1;
                pthread_mutex_lock(&state.lock);
                if (id==state.job_id) {state.stage=ARMED;state.launch_pending=1;}
                pthread_mutex_unlock(&state.lock);
            } else if(!strncmp(line,"found ",6)) {
                const char *candidate=line+6;
                if(!armed || !valid_capture(candidate)) {set_error(id,"Invalid or unexpected capture candidate");goto done;}
                pthread_mutex_lock(&state.lock);
                if(id==state.job_id) {
                    snprintf(state.candidate,sizeof state.candidate,"%s",candidate);
                    state.stage=CAPTURED;
                    state.launch_pending=0;
                }
                pthread_mutex_unlock(&state.lock);
                goto done;
            } else if(!strncmp(line,"fail ",5)) {
                set_error(id,line+5);goto done;
            } else {set_error(id,"Unexpected capture protocol message");goto done;}
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(now.tv_sec-start.tv_sec > CAPTURE_TIMEOUT+15) {set_error(id,"Capture deadline exceeded");break;}
    }
done:
    pthread_mutex_lock(&state.lock);
    if(id==state.job_id)state.worker_socket=-1;
    pthread_mutex_unlock(&state.lock);
    if(fd>=0)close(fd);
finish:
    pthread_mutex_lock(&state.lock);
    if(id==state.job_id)state.active=0;
    pthread_mutex_unlock(&state.lock);
    return NULL;
}

/* The browser API is invoked on the main HTTP-server thread, the path already
 * proven to work on the user's PS5, while the worker keeps BRC1 socket alive. */
static void maybe_launch(void) {
    char page[MAX_PAGE];
    unsigned long id;
    int should=0;
    pthread_mutex_lock(&state.lock);
    if(state.launch_pending && state.stage==ARMED) {
        state.launch_pending=0;
        state.stage=OPENING;
        snprintf(page,sizeof page,"%s",state.page);
        id=state.job_id;should=1;
    }
    pthread_mutex_unlock(&state.lock);
    if(!should) return;
    int rc=open_browser(page);
    pthread_mutex_lock(&state.lock);
    if(id==state.job_id) {
        state.launch_rc=rc;
        if(rc==0 && state.stage==OPENING) state.stage=WAITING;
        if(rc!=0 && state.stage==OPENING) {
            state.stage=FAILED;
            snprintf(state.error,sizeof state.error,"PS5 browser launch failed (code %d)",rc);
            if(state.worker_socket>=0)shutdown(state.worker_socket,SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&state.lock);
}

static size_t json_escape(char *to, size_t cap, const char *from) {
    size_t len=0;
    for(const unsigned char *p=(const unsigned char *)from;*p;p++) {
        const char *v=NULL;char tmp[8];
        if(*p=='"') v="\\\"";
        else if(*p=='\\') v="\\\\";
        else if(*p=='\n') v="\\n";
        else if(*p=='\r') v="\\r";
        else if(*p<32){snprintf(tmp,sizeof tmp,"\\u%04x",*p);v=tmp;}
        if(v) {size_t n=strlen(v);if(len+n>=cap)break;memcpy(to+len,v,n);len+=n;}
        else if(len+1<cap)to[len++]=(char)*p;
        else break;
    }
    to[len]='\0';return len;
}
static void answer(int fd,int code,const char *content_type,const char *data) {
    char head[420];
    const char *reason=code==200?"OK":code==202?"Accepted":code==400?"Bad Request":code==403?"Forbidden":code==409?"Conflict":"Not Found";
    int n=snprintf(head,sizeof head,"HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n\r\n",code,reason,content_type,strlen(data));
    if(n>0 && (size_t)n<sizeof head){(void)send_all(fd,head,(size_t)n);(void)send_all(fd,data,strlen(data));}
}
static const char *find_header(char *req,const char *key) {
    char *p=strstr(req,"\r\n"); if(!p)return NULL; p+=2;
    size_t k=strlen(key);
    for (;*p && !(p[0]=='\r'&&p[1]=='\n');) {
        char *end=strstr(p,"\r\n");if(!end)break;
        if((size_t)(end-p)>k && !strncasecmp(p,key,k) && p[k]==':') {
            const char *v=p+k+1;while(*v==' '||*v=='\t')v++;return v;
        }
        p=end+2;
    }
    return NULL;
}
static int get_string(const char *body,const char *key,char *out,size_t cap) {
    /* Deliberately tiny JSON string reader: accepts ASCII and standard escapes.
     * URLs/filenames containing JSON unicode sequences are rejected rather than
     * accidentally interpreted differently on PS5. */
    char pattern[40];
    snprintf(pattern,sizeof pattern,"\"%s\"",key);
    const char *p=strstr(body,pattern);
    if(!p)return -1;
    p+=strlen(pattern);while(*p==' '||*p=='\t')p++;
    if(*p++!=':')return -1;
    while(*p==' '||*p=='\t')p++;
    if(*p++!='"')return -1;
    size_t i=0;
    for(;*p && *p!='"';p++){
        unsigned char ch=(unsigned char)*p;
        if(ch=='\\'){p++;if(!*p)return -1;ch=(unsigned char)*p;
            if(ch=='n')ch='\n';else if(ch=='r')ch='\r';else if(ch=='t')ch='\t';
            else if(ch!='"' && ch!='\\') return -1;
        }
        if(ch<32 || ch>126 || i+1>=cap)return -1;
        out[i++]=(char)ch;
    }
    if(*p!='"')return -1;
    out[i]=0;return 0;
}
static void request(int fd) {
    char input[MAX_HTTP+1],method[9],path[128],status[5300];
    size_t used=0,bodylen,headerlen;
    while(used<MAX_HTTP){
        ssize_t n=recv(fd,input+used,MAX_HTTP-used,0);
        if(n<=0)return;
        used+=(size_t)n;input[used]='\0';
        if(strstr(input,"\r\n\r\n"))break;
    }
    char *body=strstr(input,"\r\n\r\n");
    if(!body || sscanf(input,"%8s %127s",method,path)!=2) {answer(fd,400,"text/plain","Invalid HTTP request");return;}
    body+=4;headerlen=(size_t)(body-input);
    if(!strcmp(method,"GET") && !strcmp(path,"/")){answer(fd,200,"text/html",page_html);return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/api/status")){
        char error[480],candidate[4100];
        pthread_mutex_lock(&state.lock);
        json_escape(error,sizeof error,state.error);
        json_escape(candidate,sizeof candidate,state.candidate);
        snprintf(status,sizeof status,"{\"version\":\"0.8\",\"stage\":\"%s\",\"job\":%lu,\"active\":%s,\"browser_return\":%d,\"candidate\":\"%s\",\"error\":\"%s\"}",
            stage_names[state.stage],state.job_id,state.active?"true":"false",state.launch_rc,candidate,error);
        pthread_mutex_unlock(&state.lock);
        answer(fd,200,"application/json",status);return;
    }
    if(strcmp(method,"POST") || strcmp(path,"/api/start")){answer(fd,404,"text/plain","Not found");return;}
    const char *cl=find_header(input,"Content-Length");
    if(!cl){answer(fd,400,"text/plain","Content-Length required");return;}
    char *end;
    errno=0;unsigned long count=strtoul(cl,&end,10);
    if(errno||end==cl||(*end!='\r'&&*end!=' '&&*end!='\t')||count<2||count>1500){
        answer(fd,400,"text/plain","Invalid request size");return;
    }
    bodylen=(size_t)count;
    while(used-headerlen<bodylen && used<MAX_HTTP){
        ssize_t n=recv(fd,input+used,MAX_HTTP-used,0);if(n<=0)break;
        used+=(size_t)n;
    }
    if(used-headerlen<bodylen){answer(fd,400,"text/plain","Incomplete request");return;}
    body[bodylen]=0;
    /* Read the complete request body before returning HTTP 403. Closing a TCP
     * socket with unread request bytes can emit RST on Linux and cause clients
     * to fail while reading the otherwise-correct 403 response body. */
    const char *origin=find_header(input,"Origin"),*host=find_header(input,"Host");
    if(origin){
        char got[160],allowed[160];
        const char *o=strstr(origin,"\r\n"),*h=host?strstr(host,"\r\n"):NULL;
        if(!o||!h||o-origin>=130||h-host>=115){answer(fd,403,"text/plain","Bad origin");return;}
        snprintf(got,sizeof got,"%.*s",(int)(o-origin),origin);
        snprintf(allowed,sizeof allowed,"http://%.*s",(int)(h-host),host);
        if(strcmp(got,allowed)){answer(fd,403,"text/plain","Cross-site request denied");return;}
    }
    char url[MAX_PAGE],filename[156]="";
    if(get_string(body,"url",url,sizeof url) || !valid_page(url) ||
       (strstr(body,"\"filename\"") && get_string(body,"filename",filename,sizeof filename)) ||
       !valid_filename(filename)){
        answer(fd,400,"text/plain","Invalid Vikingfile /f/ URL or optional filename");return;
    }
    pthread_mutex_lock(&state.lock);
    if(state.active){
        pthread_mutex_unlock(&state.lock);answer(fd,409,"text/plain","Capture already running");return;
    }
    state.job_id++;
    unsigned long id=state.job_id;
    state.stage=CONNECTING;
    state.launch_pending=0;state.launch_rc=-999;state.active=1;state.worker_socket=-1;
    state.candidate[0]=0;
    snprintf(state.page,sizeof state.page,"%s",url);
    snprintf(state.filename,sizeof state.filename,"%s",filename);
    state.error[0]=0;
    pthread_mutex_unlock(&state.lock);
    pthread_t worker;
    if(pthread_create(&worker,NULL,capture_task,(void *)(uintptr_t)id)!=0){
        set_error(id,"Could not start capture worker");
        answer(fd,409,"text/plain","Thread creation failed");return;
    }
    pthread_detach(worker);
    answer(fd,202,"text/plain","Capture started. The PS5 browser will open after ARMED.");
}
static void on_signal(int sig){(void)sig;running=0;}
int main(void) {
    int listener,enable=1;
    struct sockaddr_in addr;
    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);
#ifdef HOST_SIM
    const char *http=getenv("LINK_CAPTURE_TEST_PORT"), *loader=getenv("LINK_CAPTURE_TEST_LOADER_PORT");
    int port=http?atoi(http):HTTP_PORT;
    if(loader)capture_port=atoi(loader);
#else
    int port=HTTP_PORT;
    (void)sceUserServiceInitialize(NULL);
#endif
    listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0)return 1;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&enable,sizeof enable);
    memset(&addr,0,sizeof addr);addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);addr.sin_port=htons((uint16_t)port);
    if(bind(listener,(struct sockaddr *)&addr,sizeof addr)||listen(listener,8)){
        perror("bind/listen");close(listener);return 1;
    }
    fprintf(stderr,"PS5 Link Capture v0.8: HTTP :%d, local ELF loader :%d\n",port,capture_port);
    while(running){
        fd_set fds;struct timeval tv={0,200000};
        FD_ZERO(&fds);FD_SET(listener,&fds);
        int ready=select(listener+1,&fds,NULL,NULL,&tv);
        if(ready>0 && FD_ISSET(listener,&fds)){
            int fd=accept(listener,NULL,NULL);
            if(fd>=0){struct timeval to={5,0};
                setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof to);
                setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&to,sizeof to);
                request(fd);close(fd);
            }
        }
        maybe_launch();
    }
    close(listener);
#ifndef HOST_SIM
    (void)sceUserServiceTerminate();
#endif
    return 0;
}
