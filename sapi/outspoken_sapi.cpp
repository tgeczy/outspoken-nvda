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

static HMODULE g_module;
static long g_objects;
static const CLSID CLSID_Outspoken = {0xa1f4055c,0xb6c2,0x4c27,{0xab,0x6a,0xaf,0x54,0xc4,0x09,0xa3,0x09}};
static const unsigned REQ_MAGIC = 0x4F535034, RSP_MAGIC = 0x4F535052; /* OSP4 / OSPR */
static const GUID OutspokenWaveFormatEx = {0x54b2ca89,0x04d6,0x4aa7,{0x8a,0xc5,0x83,0x2c,0xc1,0x24,0x68,0x8c}};
/* The engines' native rate: every Macintosh sound buffer this project has
 * ever measured is 22254 Hz, and resampling it would be somebody else's
 * opinion about a 1984 voice. */
static const DWORD NATIVE_RATE = 22254;

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
static bool host_ensure(const std::wstring &dataRoot) {
    if(host_alive())return true;
    std::wstring base=module_dir();
    std::wstring cmd=L"\""+base+L"\\python\\python.exe\" \""+base+
                     L"\\osp_serve.py\" \""+dataRoot+L"\"";
    SECURITY_ATTRIBUTES sa={sizeof(sa),0,TRUE}; HANDLE inR,inW,outR,outW;
    if(!CreatePipe(&inR,&inW,&sa,0)||!CreatePipe(&outR,&outW,&sa,0))return false;
    SetHandleInformation(inW,HANDLE_FLAG_INHERIT,0);SetHandleInformation(outR,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;si.hStdInput=inR;si.hStdOutput=outW;si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi={}; std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end());mutableCmd.push_back(0);
    BOOL made=CreateProcessW(0,mutableCmd.data(),0,0,TRUE,CREATE_NO_WINDOW,0,base.c_str(),&si,&pi);
    CloseHandle(inR);CloseHandle(outW);
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
        if(!token||!site)return E_UNEXPECTED;
        std::wstring text;
        for(auto f=frags;f;f=f->pNext){
            switch(f->State.eAction){
            case SPVA_Speak: case SPVA_SpellOut: case SPVA_Pronounce: break;
            default: continue;
            }
            if(!f->pTextStart||!f->ulTextLen)continue;
            if(!text.empty()&&!iswspace(text.back())&&!iswspace(f->pTextStart[0]))
                text.push_back(L' ');
            text.append(f->pTextStart,f->ulTextLen);
        }
        if(text.empty())return S_OK;
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
        EnterCriticalSection(&g_hostLock);
        bool ok=host_ensure(root);
        ok=ok&&exact(g_in,&req,4,true)&&exact(g_in,&rate,4,true)&&exact(g_in,&pitch,4,true)&&exact(g_in,&volume,4,true)&&exact(g_in,&nv,4,true)&&exact(g_in,&nt,4,true)&&exact(g_in,(void*)v.data(),nv,true)&&(nt==0||exact(g_in,(void*)u.data(),nt,true));
        unsigned magic=0;int status=-1;bool aborted=false;
        ok=ok&&exact(g_out,&magic,4,false)&&exact(g_out,&status,4,false)&&magic==RSP_MAGIC;
        std::vector<BYTE> audio;
        while(ok&&status==0){
            unsigned frames=0;
            if(!exact(g_out,&frames,4,false)){ok=false;break;}
            if(!frames)break;
            unsigned bytes=frames*2; audio.resize(bytes);
            if(!exact(g_out,audio.data(),bytes,false)){ok=false;break;}
            if(site->GetActions()&SPVES_ABORT){
                /* Instant cancel is worth a cold start: kill the host
                 * mid-utterance rather than draining a render nobody
                 * wants.  131 ms rebuilds it on the next Speak. */
                aborted=true;host_drop();break;
            }
            ULONG wrote=0;if(FAILED(site->Write(audio.data(),bytes,&wrote))){ok=false;break;}
        }
        if(!ok&&!aborted)host_drop();    /* a desynced pipe is never reused */
        LeaveCriticalSection(&g_hostLock);
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
