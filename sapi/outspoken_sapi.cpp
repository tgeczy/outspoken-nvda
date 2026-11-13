/* outSPOKEN voices as a SAPI 5 engine, 32- and 64-bit.
 *
 * A fork of panthera-speech's panthera_sapi.cpp, which is where the JAWS
 * lessons live: only Speak/SpellOut/Pronounce fragments are assembled --
 * JAWS sends each word as its own fragment with a bookmark between every
 * pair, and a bookmark's text is its name -- and a space is restored at
 * the seam when neither side brought one.
 *
 * The synthesis path is deliberately not a port.  This DLL launches the
 * native host installed beside it, `osp_host.exe --serve`, which is the
 * SAME engine code the NVDA add-on loads as a DLL -- NRL rules, the
 * Component Manager glue, number reading, the 8-to-16 widening, all of it
 * in src/ -- so the SAPI voice is byte-identical to the NVDA voice by
 * construction, and tests/test_sapi_serve.py asserts exactly that.  Until
 * 2.0 the child was an embeddable Python running sapi/osp_serve.py over
 * the driver modules; that script stays as the protocol's specification
 * and tests/test_serve_hosts.py holds the two hosts to each other.  There
 * is no text processing here: the host on the other side of the pipe owns
 * every decision about how speech sounds.  What is here is SAPI's side of
 * the bargain -- markup, bookmarks, spelling, skip, the application's
 * volume, a Write that takes part of a block -- which speak_test.cpp holds
 * it to.
 *
 * The host stays resident: 22 ms warm to first PCM against 131 cold,
 * measured.  An abort is a seq-tagged cancel and a drain, the host kept
 * warm, so the next Speak is warm too.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <olectl.h>
#include "settings.h"
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

/* Since 2.0 every setting comes through settings.cpp: this user's
 * settings.toml, the machine's, then HKCU and HKLM as before, one typed
 * value at a time -- Panthera's model, so the sign-in screen speaks with
 * the settings its owner saved. */
static int diagLevel() {
    return (int)outspoken_sapi::setting_dword(L"Diagnostics", 0);
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
    /* The DLL lives in x86\ or x64\; the host program lives one level up,
     * shared by both bitnesses. */
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
/* The command line the resident host was started with.  The settings it
 * carries -- inflection, how numbers are read -- are read fresh per Speak,
 * and a host started under other values is replaced rather than kept:
 * Panthera's rule, that a settings change must respawn the host and never
 * be quietly ignored by one that read its arguments at startup. */
static std::wstring g_hostCmd;

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
    DWORD v = outspoken_sapi::setting_dword(L"ReadTimeoutMs", 30000);
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
static bool host_ensure(const std::wstring &dataRoot, DWORD inflection, const std::wstring &numbers) {
    sweep_logs();
    std::wstring base=module_dir();
    /* The 64-bit host where the installer put one, else the 32-bit one:
     * a 32-bit Windows gets only the latter, and either serves both DLL
     * bitnesses -- the child is its own process. */
    std::wstring host=base+L"\\osp_host.exe";
    if(GetFileAttributesW(host.c_str())==INVALID_FILE_ATTRIBUTES)
        host=base+L"\\osp_host_x86.exe";
    wchar_t infl[16]; swprintf_s(infl,L"%u",(unsigned)inflection);
    std::wstring cmd=L"\""+host+L"\" --serve \""+dataRoot+L"\" --inflection "+infl+L" --numbers "+numbers;
    if(host_alive()&&cmd==g_hostCmd)return true;
    host_drop();
    /* A megabyte of buffer each way against the four-kilobyte default: a
     * request larger than the buffer would block the writer until the host
     * read it, and the response side never has to stall the serve over a
     * chunk the client has not collected yet. */
    SECURITY_ATTRIBUTES sa={sizeof(sa),0,TRUE}; HANDLE inR,inW,outR,outW;
    if(!CreatePipe(&inR,&inW,&sa,1<<20)||!CreatePipe(&outR,&outW,&sa,1<<20))return false;
    SetHandleInformation(inW,HANDLE_FLAG_INHERIT,0);SetHandleInformation(outR,HANDLE_FLAG_INHERIT,0);
    /* The serve process never gets a pipe for its stderr.  A resident child
     * that writes diagnostics into a pipe nobody drains stops dead when the
     * buffer fills, and that would present as speech ending for good; NUL
     * discards and cannot block.  With diagnostics on it goes to a file
     * instead, which is where the host's own complaints are worth having. */
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
    g_proc=pi.hProcess;g_in=inW;g_out=outR;g_hostCmd=cmd;
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
    /* **A fragment list is regrouped into runs, one serve request each.**
     *
     * SAPI hands the engine its text as fragments, each with its own state:
     * the rate, pitch and volume the application's markup asked for, and
     * whether it is to be spelled.  A run is the longest stretch of
     * fragments that agree on all four, so plain text -- JAWS's word after
     * word included -- is still one run, one request and one utterance.  A
     * change of state is where the application asked for a change, and that
     * is where the NVDA driver flushes too (outspoken.py, on its pitch,
     * volume and rate commands): one engine utterance per setting, which is
     * the only way the serve can give one.
     *
     * A spelled run is spoken a character at a time, each a request of its
     * own, exactly as the driver speaks NVDA's character mode: the 1984
     * rules name a lone letter, and the Speech Manager engines do so
     * themselves.
     *
     * Bookmarks keep their place.  A mark with nothing said before it in its
     * run goes out before the run's audio, one with nothing after it, after,
     * and one between a spelled run's characters, between them -- all exact.
     * A mark between words of one utterance is an estimate, by its share of
     * the text: the engines speak an utterance whole and cannot say where in
     * it they are. */
    struct Mark { std::wstring name; size_t pos; };
    struct Run {
        long rateAdj, pitchAdj; USHORT volume; bool spell; ULONG silenceMs;
        std::wstring text; std::vector<Mark> marks;
        /* Where its sentences start, the first of them numbered
         * firstSentence in the whole call. */
        std::vector<size_t> starts; size_t firstSentence=0;
        bool same(const SPVSTATE &s,bool sp) const {
            return !silenceMs&&spell==sp&&rateAdj==s.RateAdj&&
                   pitchAdj==s.PitchAdj.MiddleAdj&&volume==s.Volume;
        }
    };
    HRESULT speakInner(const SPVTEXTFRAG *frags,ISpTTSEngineSite *site){
        if(!token||!site)return E_UNEXPECTED;
        /* Bookmarks are the pacing contract, not decoration: NVDA's SAPI
         * driver interleaves <Bookmark Mark="N"/> with the text and waits
         * for the TTS_BOOKMARK events to advance -- an engine that stays
         * silent about them is an engine whose indexes never arrive, and
         * NVDA's scheduler eventually purges what it thinks is a stuck
         * utterance.  That purge was heard as the end of speech clipping,
         * and the cold restart after it as arrowing lag. */
        std::vector<Run> runs; std::vector<Mark> pending;
        int cur=-1;
        auto open_run=[&](const SPVSTATE &s,bool sp,ULONG silence){
            Run r; r.rateAdj=s.RateAdj; r.pitchAdj=s.PitchAdj.MiddleAdj;
            r.volume=s.Volume; r.spell=sp; r.silenceMs=silence;
            r.marks.swap(pending);           /* marks waiting for a start */
            for(auto &m:r.marks)m.pos=0;
            runs.push_back(r); cur=(int)runs.size()-1;
        };
        for(auto f=frags;f;f=f->pNext){
            const SPVSTATE &st=f->State;
            if(st.eAction==SPVA_Bookmark){
                if(f->pTextStart&&f->ulTextLen){
                    Mark m; m.name.assign(f->pTextStart,f->ulTextLen);
                    if(cur<0){m.pos=0;pending.push_back(m);}
                    else{
                        Run &r=runs[cur];
                        /* After a silence, a mark is after it. */
                        m.pos=r.silenceMs?(size_t)-1:r.text.size();
                        r.marks.push_back(m);
                    }
                }
                continue;
            }
            if(st.eAction==SPVA_Silence){
                /* <silence msec="..."/>: the pause itself, in the stream. */
                open_run(st,false,st.SilenceMSecs?st.SilenceMSecs:1);
                continue;
            }
            switch(st.eAction){
            case SPVA_Speak: case SPVA_SpellOut: case SPVA_Pronounce: break;
            default: continue;
            }
            if(!f->pTextStart||!f->ulTextLen)continue;
            bool spell=st.eAction==SPVA_SpellOut;
            if(cur<0||!runs[cur].same(st,spell))open_run(st,spell,0);
            Run &r=runs[cur];
            if(!r.text.empty()&&!iswspace(r.text.back())&&!iswspace(f->pTextStart[0]))
                r.text.push_back(L' ');
            r.text.append(f->pTextStart,f->ulTextLen);
        }
        if(runs.empty()&&pending.empty())return S_OK;
        std::wstring root=token_string(token,L"DataPath"), voice=token_string(token,L"VoiceId");
        if(root.empty()){
            wchar_t buf[MAX_PATH]; DWORD n=GetEnvironmentVariableW(L"APPDATA",buf,MAX_PATH);
            if(n&&n<MAX_PATH)root=std::wstring(buf)+L"\\nvda";
        }
        long hostRate=0; site->GetRate(&hostRate);
        /* The two engine settings the NVDA driver has and SAPI's own
         * request cannot carry, read fresh so a change in the settings
         * program reaches the next thing spoken.  The number style is
         * matched against its vocabulary here, before it reaches a
         * command line: the machine file is one any account can write. */
        DWORD infl=outspoken_sapi::setting_dword(L"Inflection",50); if(infl>100)infl=100;
        std::wstring numbers=outspoken_sapi::setting_string(L"NumberStyle",L"words");
        if(numbers!=L"digits")numbers=L"words";
        /* A sentence starts with each run, and after a full stop, question
         * mark or exclamation mark followed by a space: what a skip request
         * counts in, and nothing about any one language. */
        size_t sentences=0;
        for(auto &r:runs){
            if(r.silenceMs)continue;
            for(auto &m:r.marks)if(m.pos>r.text.size())m.pos=r.text.size();
            r.firstSentence=sentences++;
            r.starts.assign(1,0);
            if(r.spell)continue;
            for(size_t i=0;i+1<r.text.size();i++){
                wchar_t c=r.text[i];
                if((c==L'.'||c==L'?'||c==L'!')&&iswspace(r.text[i+1])){
                    size_t j=i+1;
                    while(j<r.text.size()&&iswspace(r.text[j]))j++;
                    if(j<r.text.size()){r.starts.push_back(j);sentences++;}
                    i=j-1;
                }
            }
        }
        auto blank=[](const std::wstring &s,size_t from,size_t to){
            for(size_t i=from;i<to&&i<s.size();i++)if(!iswspace(s[i]))return false;
            return true;
        };

        CsLock lock(&g_hostLock);
        bool ok=true, aborted=false, skipping=false; int status=0;
        unsigned long long total=0;
        auto fire=[&](const Mark &m,unsigned long long at){
            SPEVENT ev;memset(&ev,0,sizeof ev);
            ev.eEventId=SPEI_TTS_BOOKMARK;
            ev.elParamType=SPET_LPARAM_IS_STRING;
            ev.ullAudioStreamOffset=at;
            ev.wParam=(WPARAM)_wtol(m.name.c_str());
            ev.lParam=(LPARAM)m.name.c_str();
            site->AddEvents(&ev,1);
        };
        /* Everything the application takes, it takes in full.  Write may
         * accept part of a block, or nothing at all while the audio device's
         * buffer is full -- Windows 7's SAPI does exactly that -- and what it
         * did not take is offered again after a short wait, the abort flag
         * checked between tries.  Before, the rest of the block was simply
         * dropped: on Windows 7 that was audio missing from the middle of
         * speech.  A skip request stops the writing too, where it may. */
        auto write_out=[&](const BYTE *p,ULONG n,bool mayskip)->bool{
            while(n){
                DWORD act=site->GetActions();
                if(act&SPVES_ABORT){aborted=true;return true;}
                if(mayskip&&(act&SPVES_SKIP)){skipping=true;return true;}
                ULONG took=0;
                if(FAILED(site->Write(p,n,&took)))return false;
                if(took>n)took=n;
                if(!took){Sleep(5);continue;}
                p+=took;n-=took;total+=took;
            }
            return true;
        };
        /* One utterance: ask, and stream what comes back.  On an abort the
         * seq-tagged cancel goes out and the response is drained to its
         * terminator, the host kept warm.  On a skip the rest is read unheard
         * instead, which is what says how long the whole would have been --
         * the engines cannot say where in an utterance they are, so the
         * share heard is the share of the text spoken. */
        auto say_one=[&](const std::wstring &text,long sapiRate,long pa,USHORT share,
                         unsigned long long &heard,unsigned long long &whole)->bool{
            heard=whole=0;
            /* The driver's own 0-100 scales, linearly: SAPI zero is the middle
             * of the NVDA slider, and the ten-step ends are its ends. */
            int rate=(int)((sapiRate+10)*5);
            int pitch=(int)(50+pa*5);
            int volume=100;   /* applied here, as gain: see below */
            std::string v=utf8(voice), u=utf8(text);
            unsigned req=REQ_MAGIC,nv=(unsigned)v.size(),nt=(unsigned)u.size();
            unsigned seq=++g_seq;
            ok=host_ensure(root,infl,numbers)&&exact(g_in,&req,4,true)&&exact(g_in,&seq,4,true)&&exact(g_in,&rate,4,true)&&exact(g_in,&pitch,4,true)&&exact(g_in,&volume,4,true)&&exact(g_in,&nv,4,true)&&exact(g_in,&nt,4,true)&&exact(g_in,(void*)v.data(),nv,true)&&exact(g_in,(void*)u.data(),nt,true);
            unsigned magic=0;status=-1;
            auto cancel=[&](){
                unsigned c=CANCEL_MAGIC;
                if(!exact(g_in,&c,4,true)||!exact(g_in,&seq,4,true)){host_drop();ok=false;}
            };
            /* Response reads wait rather than block -- exact_wait watches
             * the abort flag, the serve's death and a no-progress deadline,
             * so a wedged serve costs one failed utterance, not the
             * session. */
            if(ok){
                ReadWait r=exact_wait(g_out,&magic,4,site);
                if(r==RW_OK)r=exact_wait(g_out,&status,4,site);
                if(r==RW_ABORT){
                    aborted=true; cancel();
                    if(ok){
                        r=exact_wait(g_out,&magic,4,0);
                        if(r==RW_OK)r=exact_wait(g_out,&status,4,0);
                        if(r!=RW_OK||magic!=RSP_MAGIC)ok=false;
                    }
                }else if(r!=RW_OK||magic!=RSP_MAGIC)ok=false;
            }
            bool skip=false;
            std::vector<BYTE> audio;
            while(ok&&status==0){
                unsigned frames=0;
                ReadWait r=exact_wait(g_out,&frames,4,(aborted||skip)?0:site);
                if(r==RW_ABORT){aborted=true;cancel();continue;}   /* drain */
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
                whole+=bytes;
                if(aborted||skip)continue;              /* draining, not speaking */
                DWORD act=site->GetActions();
                if(act&SPVES_ABORT){
                    /* Graceful cancel keeps the host warm: the serve side
                     * runs the driver's own instant cancel when the OSPC
                     * frame lands, so the terminator follows fast and the
                     * next utterance costs 21 ms, not a cold start.  The
                     * first build killed the process here, and every
                     * arrow after a cancel paid 158 ms plus engine
                     * warm-up -- the reported arrowing lag.
                     *
                     * The cancel names its target: pipes buffer, and an
                     * untagged cancel arriving after this utterance
                     * finished was cutting the NEXT one -- the black box
                     * showed aborted=0 utterances ending at a fraction of
                     * their audio, which is what the ear heard as tails
                     * clipping on the slow engines. */
                    aborted=true;cancel();continue;
                }
                if(act&SPVES_SKIP){skip=true;continue;}
                /* **SAPI does not apply the application's volume to an
                 * engine's audio** -- this used to say it did.  Measured
                 * through SAPI into memory on the SSI-263 engine, the
                 * sibling built the same way: volume 30 left its samples as
                 * they were, where Microsoft's own voice came out at an
                 * eighth.  So the volume is applied here, per chunk, as
                 * Panthera does: the application's, times the run's own
                 * markup as a share of it, and a change during a paragraph
                 * reaches the next chunk. */
                USHORT sv=100;
                if(FAILED(site->GetVolume(&sv))){ok=false;break;}
                if(sv>100)sv=100;
                unsigned gain=(unsigned)sv*(share>100?100:share);
                if(gain!=10000){
                    short *s=(short*)audio.data();
                    for(unsigned i=0;i<frames;i++)s[i]=(short)((long long)s[i]*gain/10000);
                }
                unsigned long long before=total;
                if(!write_out(audio.data(),bytes,true)){ok=false;break;}
                heard+=total-before;
                if(aborted){cancel();continue;}
                if(skipping){skipping=false;skip=true;continue;}
            }
            return !skip;
        };

        size_t sentence=0;                      /* the one being spoken */
        auto sentence_at=[&](const Run &r,size_t p){
            size_t s=r.firstSentence;
            for(size_t k=1;k<r.starts.size();k++)if(r.starts[k]<=p)s=r.firstSentence+k;
            return s;
        };
        for(auto &m:pending)fire(m,0);          /* marks and nothing else */
        size_t ri=0, from=0;
        bool stopped=false;                     /* a skip that ran out of text */
        while(ri<runs.size()&&ok&&status==0&&!aborted){
            Run &r=runs[ri];
            bool skipped=false;
            long sapiRate=hostRate+r.rateAdj;
            if(sapiRate<-10)sapiRate=-10;if(sapiRate>10)sapiRate=10;
            long pa=r.pitchAdj;
            if(pa<-10)pa=-10;if(pa>10)pa=10;
            if(r.silenceMs){
                for(auto &m:r.marks)if(m.pos!=(size_t)-1)fire(m,total);
                std::vector<BYTE> zero((size_t)r.silenceMs*NATIVE_RATE/1000*2);
                if(!write_out(zero.data(),(ULONG)zero.size(),true))ok=false;
                if(aborted)break;
                if(skipping){skipping=false;skipped=true;}
                else for(auto &m:r.marks)if(m.pos==(size_t)-1)fire(m,total);
            } else if(r.spell){
                sentence=r.firstSentence;
                size_t next=0;                          /* the next mark to send */
                while(next<r.marks.size()&&r.marks[next].pos<from)next++;
                for(size_t i=from;i<r.text.size()&&ok&&status==0&&!aborted&&!skipped;){
                    size_t n=(IS_HIGH_SURROGATE(r.text[i])&&i+1<r.text.size())?2:1;
                    /* a combining mark or variation selector stays with
                     * the character before it */
                    while(i+n<r.text.size()&&
                          ((r.text[i+n]>=0x0300&&r.text[i+n]<=0x036F)||(r.text[i+n]>=0xFE00&&r.text[i+n]<=0xFE0F)))n++;
                    while(next<r.marks.size()&&r.marks[next].pos<=i)fire(r.marks[next++],total);
                    if(!iswspace(r.text[i])){
                        unsigned long long heard,whole;
                        if(!say_one(r.text.substr(i,n),sapiRate,pa,r.volume,heard,whole))skipped=true;
                    }
                    i+=n;
                }
                if(!aborted&&!skipped)while(next<r.marks.size())fire(r.marks[next++],total);
            } else {
                sentence=sentence_at(r,from);
                /* -1: passed over by a skip, 0: before anything said,
                 *  1: between words, 2: after it all */
                std::vector<int> where(r.marks.size());
                for(size_t i=0;i<r.marks.size();i++)
                    where[i]=r.marks[i].pos<from?-1:blank(r.text,from,r.marks[i].pos)?0:
                             blank(r.text,r.marks[i].pos,r.text.size())?2:1;
                for(size_t i=0;i<r.marks.size();i++)if(where[i]==0)fire(r.marks[i],total);
                unsigned long long runStart=total, last=total, heard=0, whole=0;
                std::wstring text=r.text.substr(from);
                if(!blank(text,0,text.size())&&!say_one(text,sapiRate,pa,r.volume,heard,whole)){
                    skipped=true;
                    /* Where it was: the share heard of the share of the text. */
                    size_t span=r.text.size()-from;
                    size_t at=from+(whole?(size_t)((double)heard/(double)whole*(double)span):0);
                    sentence=sentence_at(r,at);
                    /* The marks in what was heard were passed, and are
                     * reported; the rest are skipped over. */
                    for(size_t i=0;i<r.marks.size();i++)
                        if(where[i]==1&&r.marks[i].pos<at){
                            unsigned long long off=runStart+
                                (unsigned long long)((double)(r.marks[i].pos-from)/(double)span*(double)whole)/2*2;
                            if(off>total)off=total;
                            if(off<last)off=last;
                            fire(r.marks[i],off);last=off;
                        }
                }
                if(aborted)break;
                if(!skipped){
                    size_t span=r.text.size()-from;
                    for(size_t i=0;i<r.marks.size();i++)
                        if(where[i]==1){
                            unsigned long long at=runStart+(span?
                                (unsigned long long)((double)(r.marks[i].pos-from)/(double)span*(double)(total-runStart))/2*2:0);
                            if(at<last)at=last;
                            fire(r.marks[i],at);last=at;
                        }
                    for(size_t i=0;i<r.marks.size();i++)if(where[i]==2)fire(r.marks[i],total);
                }
            }
            if(aborted)break;
            if(!skipped){ri++;from=0;continue;}
            /* A skip request (ISpVoice::Skip): how many sentences, from the
             * one being spoken, and in which direction; 0 is the start of
             * this one.  The number that could be skipped is reported, and
             * speech goes on from there if it was the number asked for --
             * or, when the text has fewer sentences than that, ends.  A host
             * that cannot say what it wants is answered with nothing
             * skipped, and the call ends. */
            SPVSKIPTYPE type=SPVST_SENTENCE; long n=0;
            if(FAILED(site->GetSkipInfo(&type,&n))||type!=SPVST_SENTENCE){
                site->CompleteSkip(0);stopped=true;break;
            }
            long target=(long)sentence+n;
            if(target<0||target>=(long)sentences){
                site->CompleteSkip(target<0?-(long)sentence:(long)sentences-1-(long)sentence);
                stopped=true;break;
            }
            site->CompleteSkip(n);
            for(size_t j=0;j<runs.size();j++){
                Run &q=runs[j];
                if(q.silenceMs)continue;
                if((size_t)target>=q.firstSentence&&(size_t)target<q.firstSentence+q.starts.size()){
                    ri=j;from=q.starts[(size_t)target-q.firstSentence];break;
                }
            }
        }
        if(!aborted&&!stopped){
            /* A run that failed still answers the marks after it: an index
             * that never arrives is a stuck utterance to the client. */
            for(size_t j=ri;j<runs.size();j++)
                for(auto &m:runs[j].marks)fire(m,total);
        }
        /* And the tail: the engines end at the last phoneme with zero
         * trailing frames, and SAPI's playback path eats the tail of a
         * stream that ends flush with its data.  150 ms of silence makes
         * what it eats silent.  NVDA's own player drains properly, which is
         * why the add-on never needed this. */
        if(total&&ok&&status==0&&!aborted){
            BYTE pad[6676]={0};                /* 150 ms at 22254 Hz */
            if(!write_out(pad,sizeof pad,false))ok=false;
        }
        /* A desynced pipe is never reused -- and that has to include a
         * drain that failed after an abort: the old guard kept the host
         * when `aborted` was set, so a read error mid-drain left a pipe
         * full of leftovers for the next utterance to misread. */
        if(!ok)host_drop();
        /* The measurements convicted all four bugs; the words never did. */
        size_t chars=0,count=pending.size();
        for(auto &r:runs){chars+=r.text.size();count+=r.marks.size();}
        if(diagLevel()>=2)
            logline(L"speak done: runs=%u chars=%u marks=%u bytes-written=%u ok=%d status=%d aborted=%d text=\"%.40s\"",
                    (unsigned)runs.size(),(unsigned)chars,(unsigned)count,(unsigned)total,
                    ok?1:0,status,aborted?1:0,runs.empty()?L"":runs[0].text.c_str());
        else
            logline(L"speak done: runs=%u chars=%u marks=%u bytes-written=%u ok=%d status=%d aborted=%d",
                    (unsigned)runs.size(),(unsigned)chars,(unsigned)count,(unsigned)total,
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
