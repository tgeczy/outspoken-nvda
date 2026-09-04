/* outSPOKEN voices as a SAPI 5 engine, 32- and 64-bit.
 *
 * A fork of panthera-speech's panthera_sapi.cpp, which is where the JAWS
 * lessons live: only Speak/SpellOut/Pronounce fragments are assembled --
 * JAWS sends each word as its own fragment with a bookmark between every
 * pair, and a bookmark's text is its name -- and a space is restored at
 * the seam when neither side brought one.
 *
 * The synthesis path is deliberately not a port.  This DLL launches the
 * embeddable Python installed beside it, running sapi/osp_serve.py, which
 * serves the SAME driver modules the NVDA add-on runs -- NRL rules,
 * MacinTalk 2 command building, number reading, the 8-to-16 widening --
 * so the SAPI voice is byte-identical to the NVDA voice by construction,
 * and tests/test_sapi_serve.py asserts exactly that.  There is no text
 * processing here at all: the driver on the other side of the pipe owns
 * every decision about how speech sounds.
 *
 * The host stays resident: 22 ms warm to first PCM against 131 cold,
 * measured.  An abort kills it -- instant cancel -- and the next Speak
 * pays the cold start once.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <olectl.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>

static HMODULE g_module;
static long g_objects;
static const CLSID CLSID_Outspoken = {0xa1f4055c,0xb6c2,0x4c27,{0xab,0x6a,0xaf,0x54,0xc4,0x09,0xa3,0x09}};
static const unsigned REQ_MAGIC = 0x4F535034, RSP_MAGIC = 0x4F535052, CANCEL_MAGIC = 0x4F535043; /* OSP4 / OSPR / OSPC */
/* SPDFID_WaveFormatEx, by value: the ONE format GUID SAPI recognises as
 * "the WAVEFORMATEX that follows describes the audio".  A fresh GUID here
 * registers fine and then speaks silence -- SAPI cannot negotiate a format
 * it has never heard of.  Found by Tomi's ear inside ten minutes of the
 * first build reaching a real SAPI client. */
static const GUID OutspokenWaveFormatEx = {0xc31adbae,0x527f,0x4ff5,{0xa2,0x30,0xf6,0x2b,0xb6,0x1f,0xf7,0x0c}};
/* The engines' native rate: every Macintosh sound buffer this project has
 * ever measured is 22254 Hz, and resampling it would be somebody else's
 * opinion about a 1984 voice. */
static const DWORD NATIVE_RATE = 22254;

/* The black box -- **off unless somebody asks for it.**
 *
 * The live-client clip survived three fixes and only this file killed it,
 * which earned it a place here.  It did not earn the place it first took,
 * which was on, always, for everyone: a line per utterance is a line per
 * keystroke, appended forever to a file in %TEMP% that never rotates, and
 * the line carried the first forty characters of the text.  For a screen
 * reader that is a running transcript of somebody's mail, their messages
 * and their bank, in a folder anything running as them can read.
 *
 * `Diagnostics` in HKCU, beside the DataPath the settings program already
 * keeps there.  0, the default, writes nothing and creates no file.  1
 * writes the measurements, which are what actually convicted.  2 adds a
 * slice of the text, for the rare report that is about particular words --
 * two deliberate steps to reach the thing with words in it.  Capped either
 * way, because a diagnostic nobody turns off is a disk that fills. */
static const DWORD LOG_CAP = 4u * 1024u * 1024u;

static int diagLevel() {
    HKEY k; DWORD v = 0, n = sizeof v, t;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\outSPOKEN SAPI", 0,
                       KEY_READ, &k)) {
        if (RegQueryValueExW(k, L"Diagnostics", 0, &t, (BYTE*)&v, &n)
            || t != REG_DWORD) v = 0;
        RegCloseKey(k);
    }
    return (int)v;
}
/* And clear up after 1.1.0, which wrote without asking.
 *
 * Everyone who installed it has a log in %TEMP% still growing a line per
 * utterance, with forty characters of each one in it, and turning the tap
 * off does not empty the bucket.  Once per process, with diagnostics off,
 * our own files go -- only the names this engine writes, only in the temp
 * folder, and only when nothing is meant to be being collected. */
static void sweep_logs() {
    static LONG done;
    if (InterlockedExchange(&done, 1) || diagLevel()) return;
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 48) return;
    const wchar_t *globs[] = {L"\\outspoken_sapi.log",
                              L"\\outspoken_sapi_serve-*.log"};
    for (int g = 0; g < 2; g++) {
        wchar_t pat[MAX_PATH]; lstrcpyW(pat, dir); lstrcatW(pat, globs[g]);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t victim[MAX_PATH];
            lstrcpyW(victim, dir); lstrcatW(victim, L"\\");
            lstrcatW(victim, fd.cFileName);
            DeleteFileW(victim);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void logline(const wchar_t *fmt, ...) {
    if (!diagLevel()) return;
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", path, MAX_PATH);
    if (!n || n >= MAX_PATH - 24) return;
    lstrcatW(path, L"\\outspoken_sapi.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0, OPEN_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    {   /* Start over rather than grow without end. */
        LARGE_INTEGER sz;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > (LONGLONG)LOG_CAP) {
            CloseHandle(f);
            f = CreateFileW(path, GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            CREATE_ALWAYS, 0, 0);
            if (f == INVALID_HANDLE_VALUE) return;
        }
    }
    wchar_t line[512];
    va_list ap; va_start(ap, fmt);
    int len = _vsnwprintf_s(line, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (len < 0) len = 511;
    char out[1100]; int m = WideCharToMultiByte(CP_UTF8, 0, line, len, out, 1060, 0, 0);
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32];
    int sn = sprintf_s(stamp, 32, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    DWORD w;
    WriteFile(f, stamp, sn, &w, 0);
    WriteFile(f, out, m, &w, 0);
    WriteFile(f, "\r\n", 2, &w, 0);
    CloseHandle(f);
}

static bool exact(HANDLE h, void *p, DWORD n, bool write) {
    BYTE *b=(BYTE*)p; DWORD done=0, x;
    while(done<n) {
        BOOL ok=write?WriteFile(h,b+done,n-done,&x,0):ReadFile(h,b+done,n-done,&x,0);
        if(!ok || !x) return false; done+=x;
    }
    return true;
}
static std::wstring module_dir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_module,p,MAX_PATH);
    wchar_t *s=wcsrchr(p,L'\\'); if(s)*s=0;
    /* The DLL lives in x86\ or x64\; the serve script and Python live one
     * level up, shared by both bitnesses. */
    std::wstring d=p; size_t slash=d.rfind(L'\\');
    if(slash!=std::wstring::npos){
        std::wstring leaf=d.substr(slash+1);
        if(leaf==L"x86"||leaf==L"x64") d.resize(slash);
    }
    return d;
}
static std::string utf8(const std::wstring &s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),0,0,0,0);
    std::string r(n,0); if(n) WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),&r[0],n,0,0); return r;
}
static std::wstring token_string(ISpObjectToken *t, const wchar_t *name) {
    wchar_t *v=0; std::wstring r;
    if(t && SUCCEEDED(t->GetStringValue(name,&v)) && v) { r=v; CoTaskMemFree(v); }
    return r;
}

/* The resident host: one serve process per SAPI process, guarded.  The
 * protocol is stateless per request, so sharing it between voices is free
 * -- the voice id travels in every request. */
static CRITICAL_SECTION g_hostLock;
static bool g_lockReady;
static HANDLE g_proc, g_in, g_out;
static unsigned g_seq;

static void host_drop() {
    if(g_proc){TerminateProcess(g_proc,0);CloseHandle(g_proc);g_proc=0;}
    if(g_in){CloseHandle(g_in);g_in=0;}
    if(g_out){CloseHandle(g_out);g_out=0;}
}
static bool host_alive() {
    if(!g_proc)return false;
    DWORD code=0;
    if(!GetExitCodeProcess(g_proc,&code)||code!=STILL_ACTIVE){host_drop();return false;}
    return true;
}

/* The serve side sends one engine buffer per chunk, and no Macintosh sound
 * buffer these engines fill comes anywhere near ten seconds of audio.  A
 * count above this is not a large chunk, it is a desynced pipe being read
 * as one -- and before this check, `audio.resize(frames*2)` on such a
 * count threw bad_alloc straight through the COM boundary into the client
 * application, which is how Panthera's sibling crashed a game.  The serve
 * script now keeps stray prints off the stream entirely; this is armor for
 * whatever corrupts it anyway, because a *small* misread is arithmetic
 * this side cannot detect at all. */
static const unsigned MAX_CHUNK_FRAMES = NATIVE_RATE * 10u;

static DWORD read_timeout_ms() {
    HKEY k; DWORD v = 30000, n = sizeof v, t;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\outSPOKEN SAPI", 0,
                       KEY_READ, &k)) {
        DWORD got = 0; n = sizeof got;
        if (!RegQueryValueExW(k, L"ReadTimeoutMs", 0, &t, (BYTE*)&got, &n)
            && t == REG_DWORD) v = got;
        RegCloseKey(k);
    }
    return v < 1000 ? 1000 : v;
}

/* Read exactly `n` response bytes without ever trusting the host to answer.
 *
 * The old reads blocked in ReadFile with no way out: a serve process that
 * wedged mid-render -- alive, silent -- held the client's speech thread
 * forever, lock in hand, and the session's SAPI speech died with it.  The
 * wait now watches the abort flag, the host's death, and a no-progress
 * deadline (ReadTimeoutMs, default thirty seconds).
 *
 * **The abort is only honoured before the first byte of an item.**  This
 * engine's cancel is graceful -- the host stays warm and the response is
 * drained to its terminator -- so returning mid-item would leave the pipe
 * misaligned inside the very stream the drain exists to preserve.  Started
 * items finish or fail; the deadline holds either way. */
enum ReadWait { RW_OK, RW_FAIL, RW_ABORT };
static ReadWait exact_wait(HANDLE h, void *p, DWORD n, ISpTTSEngineSite *site) {
    BYTE *b=(BYTE*)p; DWORD done=0;
    const DWORD budget=read_timeout_ms();
    DWORD idle=GetTickCount();
    while(done<n){
        DWORD avail=0;
        if(!PeekNamedPipe(h,0,0,0,&avail,0))return RW_FAIL;
        if(avail){
            DWORD want=n-done; if(want>avail)want=avail;
            DWORD got=0;
            if(!ReadFile(h,b+done,want,&got,0)||!got)return RW_FAIL;
            done+=got; idle=GetTickCount();
            continue;
        }
        if(!done&&site&&(site->GetActions()&SPVES_ABORT))return RW_ABORT;
        if(!g_proc||WaitForSingleObject(g_proc,0)==WAIT_OBJECT_0)return RW_FAIL;
        if(GetTickCount()-idle>=budget)return RW_FAIL;
        Sleep(3);
    }
    return RW_OK;
}

/* Scoped, so an exception unwinding out of the speak path releases the lock
 * instead of abandoning it -- an abandoned critical section turns the next
 * utterance into a deadlock, heard as speech dying for good. */
struct CsLock {
    CRITICAL_SECTION *cs;
    CsLock(CRITICAL_SECTION *c):cs(c){EnterCriticalSection(cs);}
    ~CsLock(){LeaveCriticalSection(cs);}
};
static bool host_ensure(const std::wstring &dataRoot) {
    sweep_logs();
    if(host_alive())return true;
    std::wstring base=module_dir();
    std::wstring cmd=L"\""+base+L"\\python\\python.exe\" \""+base+
                     L"\\osp_serve.py\" \""+dataRoot+L"\"";
    /* A megabyte of buffer each way against the four-kilobyte default: a
     * request larger than the buffer would block the writer until the host
     * read it, and the response side never has to stall the serve over a
     * chunk the client has not collected yet. */
    SECURITY_ATTRIBUTES sa={sizeof(sa),0,TRUE}; HANDLE inR,inW,outR,outW;
    if(!CreatePipe(&inR,&inW,&sa,1<<20)||!CreatePipe(&outR,&outW,&sa,1<<20))return false;
    SetHandleInformation(inW,HANDLE_FLAG_INHERIT,0);SetHandleInformation(outR,HANDLE_FLAG_INHERIT,0);
    /* The serve process never gets a pipe for its stderr.  A resident child
     * that writes a traceback into a pipe nobody drains stops dead when the
     * buffer fills, and that would present as speech ending for good; NUL
     * discards and cannot block.  With diagnostics on it goes to a file
     * instead, which is where a Python traceback is worth having. */
    HANDLE errH=INVALID_HANDLE_VALUE;
    {
        SECURITY_ATTRIBUTES esa={sizeof(esa),0,TRUE};
        wchar_t epath[MAX_PATH]; DWORD en=GetEnvironmentVariableW(L"TEMP",epath,MAX_PATH);
        if(diagLevel()&&en&&en<MAX_PATH-40){
            wchar_t leaf[40];
            swprintf_s(leaf,40,L"\\outspoken_sapi_serve-%u.log",(unsigned)GetCurrentProcessId());
            lstrcatW(epath,leaf);
            errH=CreateFileW(epath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&esa,OPEN_ALWAYS,0,0);
        }
        if(errH==INVALID_HANDLE_VALUE)
            errH=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&esa,OPEN_EXISTING,0,0);
    }
    STARTUPINFOW si={sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;si.hStdInput=inR;si.hStdOutput=outW;
    si.hStdError=errH!=INVALID_HANDLE_VALUE?errH:GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi={}; std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end());mutableCmd.push_back(0);
    BOOL made=CreateProcessW(0,mutableCmd.data(),0,0,TRUE,CREATE_NO_WINDOW,0,base.c_str(),&si,&pi);
    CloseHandle(inR);CloseHandle(outW);
    if(errH!=INVALID_HANDLE_VALUE)CloseHandle(errH);
    if(!made){CloseHandle(inW);CloseHandle(outR);return false;}
    CloseHandle(pi.hThread);
    g_proc=pi.hProcess;g_in=inW;g_out=outR;
    return true;
}

class Engine : public ISpTTSEngine, public ISpObjectWithToken {
    LONG refs; ISpObjectToken *token;
public:
    Engine():refs(1),token(0){InterlockedIncrement(&g_objects);}
    ~Engine(){if(token)token->Release();InterlockedDecrement(&g_objects);}
    STDMETHODIMP QueryInterface(REFIID i,void **p){
        if(!p)return E_POINTER; *p=0;
        if(i==IID_IUnknown||i==IID_ISpTTSEngine)*p=(ISpTTSEngine*)this;
        else if(i==IID_ISpObjectWithToken)*p=(ISpObjectWithToken*)this;
        else return E_NOINTERFACE; AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef(){return InterlockedIncrement(&refs);}
    STDMETHODIMP_(ULONG) Release(){ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    STDMETHODIMP SetObjectToken(ISpObjectToken *t){if(!t)return E_INVALIDARG;if(token)return E_UNEXPECTED;token=t;t->AddRef();return S_OK;}
    STDMETHODIMP GetObjectToken(ISpObjectToken **t){if(!t)return E_POINTER;*t=token;if(token)token->AddRef();return token?S_OK:S_FALSE;}
    STDMETHODIMP GetOutputFormat(const GUID*,const WAVEFORMATEX*,GUID *id,WAVEFORMATEX **wf){
        if(!id||!wf)return E_POINTER; *id=OutspokenWaveFormatEx;
        WAVEFORMATEX f={WAVE_FORMAT_PCM,1,NATIVE_RATE,NATIVE_RATE*2,2,16,0};
        *wf=(WAVEFORMATEX*)CoTaskMemAlloc(sizeof f);if(!*wf)return E_OUTOFMEMORY;**wf=f;return S_OK;
    }
    STDMETHODIMP Speak(DWORD,REFGUID,const WAVEFORMATEX*,const SPVTEXTFRAG *frags,ISpTTSEngineSite *site){
        /* A COM method must never let an exception out: SAPI has no
         * handler for one and the client application dies of it.  The
         * frame-count clamp below makes the known thrower unreachable,
         * but the guarantee belongs at the boundary, whatever the cause. */
        try {
            return speakInner(frags,site);
        } catch(...) {
            if(g_lockReady){
                CsLock lock(&g_hostLock);
                host_drop();       /* mid-protocol unwind = desynced pipe */
            }
            return E_FAIL;
        }
    }
    HRESULT speakInner(const SPVTEXTFRAG *frags,ISpTTSEngineSite *site){
        if(!token||!site)return E_UNEXPECTED;
        std::wstring text;
        /* Bookmarks are the pacing contract, not decoration: NVDA's SAPI
         * driver interleaves <Bookmark Mark="N"/> with the text and waits
         * for the TTS_BOOKMARK events to advance -- an engine that stays
         * silent about them is an engine whose indexes never arrive, and
         * NVDA's scheduler eventually purges what it thinks is a stuck
         * utterance.  That purge was heard as the end of speech clipping,
         * and the cold restart after it as arrowing lag. */
        struct Mark { std::wstring name; size_t chars; };
        std::vector<Mark> marks;
        for(auto f=frags;f;f=f->pNext){
            if(f->State.eAction==SPVA_Bookmark){
                if(f->pTextStart&&f->ulTextLen){
                    Mark m; m.name.assign(f->pTextStart,f->ulTextLen);
                    m.chars=text.size(); marks.push_back(m);
                }
                continue;
            }
            switch(f->State.eAction){
            case SPVA_Speak: case SPVA_SpellOut: case SPVA_Pronounce: break;
            default: continue;
            }
            if(!f->pTextStart||!f->ulTextLen)continue;
            if(!text.empty()&&!iswspace(text.back())&&!iswspace(f->pTextStart[0]))
                text.push_back(L' ');
            text.append(f->pTextStart,f->ulTextLen);
        }
        if(text.empty()&&marks.empty())return S_OK;
        std::wstring root=token_string(token,L"DataPath"), voice=token_string(token,L"VoiceId");
        if(root.empty()){
            wchar_t buf[MAX_PATH]; DWORD n=GetEnvironmentVariableW(L"APPDATA",buf,MAX_PATH);
            if(n&&n<MAX_PATH)root=std::wstring(buf)+L"\\nvda";
        }
        long sapiRate=0; site->GetRate(&sapiRate);
        if(sapiRate<-10)sapiRate=-10;if(sapiRate>10)sapiRate=10;
        /* The driver's own 0-100 scales, linearly: SAPI zero is the middle
         * of the NVDA slider, and the ten-step ends are its ends. */
        int rate=(int)((sapiRate+10)*5);
        int pitch=50;
        if(frags){
            long pa=frags->State.PitchAdj.MiddleAdj;
            if(pa<-10)pa=-10;if(pa>10)pa=10;
            pitch=(int)(50+pa*5);
        }
        int volume=100;   /* SAPI applies the application's volume itself. */
        std::string v=utf8(voice), u=utf8(text);
        unsigned req=REQ_MAGIC,nv=(unsigned)v.size(),nt=(unsigned)u.size();
        CsLock lock(&g_hostLock);
        bool ok=true;int status=0;bool aborted=false;
        unsigned long long total=0;
        unsigned seq=++g_seq;
        if(!text.empty()){
            ok=host_ensure(root);
            ok=ok&&exact(g_in,&req,4,true)&&exact(g_in,&seq,4,true)&&exact(g_in,&rate,4,true)&&exact(g_in,&pitch,4,true)&&exact(g_in,&volume,4,true)&&exact(g_in,&nv,4,true)&&exact(g_in,&nt,4,true)&&exact(g_in,(void*)v.data(),nv,true)&&exact(g_in,(void*)u.data(),nt,true);
            unsigned magic=0;status=-1;
            /* Response reads wait rather than block -- exact_wait watches
             * the abort flag, the serve's death and a no-progress deadline,
             * so a wedged serve costs one failed utterance, not the
             * session.  An abort while waiting takes the same door as the
             * mid-stream one below: the seq-tagged cancel frame, then a
             * drain to the terminator with the host kept warm. */
            if(ok){
                ReadWait r=exact_wait(g_out,&magic,4,site);
                if(r==RW_OK)r=exact_wait(g_out,&status,4,site);
                if(r==RW_ABORT){
                    aborted=true;
                    unsigned c=CANCEL_MAGIC;
                    if(!exact(g_in,&c,4,true)||!exact(g_in,&seq,4,true)){host_drop();ok=false;}
                    else{
                        r=exact_wait(g_out,&magic,4,0);
                        if(r==RW_OK)r=exact_wait(g_out,&status,4,0);
                        if(r!=RW_OK||magic!=RSP_MAGIC)ok=false;
                    }
                }else if(r!=RW_OK||magic!=RSP_MAGIC)ok=false;
            }
            std::vector<BYTE> audio;
            while(ok&&status==0){
                unsigned frames=0;
                ReadWait r=exact_wait(g_out,&frames,4,aborted?0:site);
                if(r==RW_ABORT){
                    aborted=true;
                    unsigned c=CANCEL_MAGIC;
                    if(!exact(g_in,&c,4,true)||!exact(g_in,&seq,4,true)){host_drop();ok=false;break;}
                    continue;                      /* drain to terminator */
                }
                if(r!=RW_OK){ok=false;break;}
                if(!frames)break;
                if(frames>MAX_CHUNK_FRAMES){
                    /* Not a chunk, a desynced stream read as one; see the
                     * constant.  The pipe is unusable from here. */
                    logline(L"desync: frame count %u refused",frames);
                    ok=false;break;
                }
                unsigned bytes=frames*2; audio.resize(bytes);
                if(exact_wait(g_out,audio.data(),bytes,0)!=RW_OK){ok=false;break;}
                if(!aborted&&(site->GetActions()&SPVES_ABORT)){
                    /* Graceful cancel keeps the host warm: the serve side
                     * runs the driver's own instant cancel when the OSPC
                     * frame lands, so the terminator follows fast and the
                     * next utterance costs 21 ms, not a cold start.  The
                     * first build killed the process here, and every
                     * arrow after a cancel paid 158 ms plus engine
                     * warm-up -- the reported arrowing lag. */
                    aborted=true;
                    /* The cancel names its target: pipes buffer, and an
                     * untagged cancel arriving after this utterance
                     * finished was cutting the NEXT one -- the black box
                     * showed aborted=0 utterances ending at a fraction of
                     * their audio, which is what the ear heard as tails
                     * clipping on the slow engines. */
                    unsigned c=CANCEL_MAGIC;
                    if(!exact(g_in,&c,4,true)||!exact(g_in,&seq,4,true)){host_drop();break;}
                    continue;                      /* drain to terminator */
                }
                if(aborted)continue;               /* draining, not speaking */
                ULONG wrote=0;if(FAILED(site->Write(audio.data(),bytes,&wrote))){ok=false;break;}
                total+=bytes;
            }
        }
        if(ok&&status==0&&!aborted){
            /* The pacing contract: one TTS_BOOKMARK event per bookmark
             * fragment.  The audio streamed as one utterance, so the
             * offsets are proportional estimates by character position --
             * NVDA schedules the index at its own player position when
             * the event arrives, so arrival is what unblocks it and the
             * offset is bookkeeping. */
            for(size_t i=0;i<marks.size();i++){
                SPEVENT ev;memset(&ev,0,sizeof ev);
                ev.eEventId=SPEI_TTS_BOOKMARK;
                ev.elParamType=SPET_LPARAM_IS_STRING;
                ev.ullAudioStreamOffset=text.empty()?0:
                    (unsigned long long)((double)marks[i].chars/(double)text.size()*(double)total);
                ev.wParam=(WPARAM)_wtol(marks[i].name.c_str());
                ev.lParam=(LPARAM)marks[i].name.c_str();
                site->AddEvents(&ev,1);
            }
            /* And the tail: the engines end at the last phoneme with zero
             * trailing frames, and SAPI's playback path eats the tail of
             * a stream that ends flush with its data.  150 ms of silence
             * makes what it eats silent.  NVDA's own player drains
             * properly, which is why the add-on never needed this. */
            if(total){
                BYTE pad[6676]={0};                /* 150 ms at 22254 Hz */
                ULONG wrote=0;site->Write(pad,sizeof pad,&wrote);
            }
        }
        /* A desynced pipe is never reused -- and that has to include a
         * drain that failed after an abort: the old guard kept the host
         * when `aborted` was set, so a read error mid-drain left a pipe
         * full of leftovers for the next utterance to misread. */
        if(!ok)host_drop();
        /* The measurements convicted all four bugs; the words never did. */
        if(diagLevel()>=2)
            logline(L"speak done: chars=%u marks=%u bytes-written=%u ok=%d status=%d aborted=%d text=\"%.40s\"",
                    (unsigned)text.size(),(unsigned)marks.size(),(unsigned)total,
                    ok?1:0,status,aborted?1:0,text.c_str());
        else
            logline(L"speak done: chars=%u marks=%u bytes-written=%u ok=%d status=%d aborted=%d",
                    (unsigned)text.size(),(unsigned)marks.size(),(unsigned)total,
                    ok?1:0,status,aborted?1:0);
        return aborted||(ok&&status==0)?S_OK:E_FAIL;
    }
};
class Factory:public IClassFactory{LONG refs;public:Factory():refs(1){InterlockedIncrement(&g_objects);} ~Factory(){InterlockedDecrement(&g_objects);} STDMETHODIMP QueryInterface(REFIID i,void**p){if(!p)return E_POINTER;*p=0;if(i==IID_IUnknown||i==IID_IClassFactory)*p=this;else return E_NOINTERFACE;AddRef();return S_OK;} STDMETHODIMP_(ULONG)AddRef(){return InterlockedIncrement(&refs);} STDMETHODIMP_(ULONG)Release(){ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;} STDMETHODIMP CreateInstance(IUnknown*o,REFIID i,void**p){if(o)return CLASS_E_NOAGGREGATION;Engine*e=new Engine;HRESULT h=e->QueryInterface(i,p);e->Release();return h;} STDMETHODIMP LockServer(BOOL x){InterlockedExchangeAdd(&g_objects,x?1:-1);return S_OK;}};

STDAPI DllCanUnloadNow(){return g_objects?S_FALSE:S_OK;}
STDAPI DllGetClassObject(REFCLSID c,REFIID i,void **p){if(c!=CLSID_Outspoken)return CLASS_E_CLASSNOTAVAILABLE;Factory*f=new Factory;HRESULT h=f->QueryInterface(i,p);f->Release();return h;}
static HRESULT reg(bool add){
    wchar_t cls[64];StringFromGUID2(CLSID_Outspoken,cls,64);std::wstring key=L"Software\\Classes\\CLSID\\"+std::wstring(cls);
    if(!add){RegDeleteTreeW(HKEY_LOCAL_MACHINE,key.c_str());return S_OK;}
    HKEY h,k; if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,key.c_str(),0,0,0,KEY_WRITE,0,&h,0))return SELFREG_E_CLASS;
    const wchar_t name[]=L"outSPOKEN SAPI speech engine";RegSetValueExW(h,0,0,REG_SZ,(BYTE*)name,sizeof(name));
    wchar_t self[MAX_PATH];GetModuleFileNameW(g_module,self,MAX_PATH);
    std::wstring sub=key+L"\\InprocServer32",path=self;RegCloseKey(h);
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,sub.c_str(),0,0,0,KEY_WRITE,0,&k,0))return SELFREG_E_CLASS;
    RegSetValueExW(k,0,0,REG_SZ,(BYTE*)path.c_str(),(DWORD)((path.size()+1)*2));const wchar_t both[]=L"Both";RegSetValueExW(k,L"ThreadingModel",0,REG_SZ,(BYTE*)both,sizeof(both));RegCloseKey(k);return S_OK;
}
STDAPI DllRegisterServer(){return reg(true);} STDAPI DllUnregisterServer(){return reg(false);}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,LPVOID){
    if(why==DLL_PROCESS_ATTACH){g_module=h;DisableThreadLibraryCalls(h);
        if(!g_lockReady){InitializeCriticalSection(&g_hostLock);g_lockReady=true;}}
    if(why==DLL_PROCESS_DETACH){if(g_proc){TerminateProcess(g_proc,0);CloseHandle(g_proc);}}
    return TRUE;
}
