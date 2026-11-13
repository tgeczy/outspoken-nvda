/* The SAPI engine, held to what SAPI asks of it, through the engine itself.
 *
 * TGSpeechBox's SAPI engine was found answering some of SAPI's requests
 * wrongly, and then this one and Panthera's, which share a design: a write
 * Windows 7 takes only in part, rate and volume markup, pitch markup after
 * the first fragment, <spell>, <silence>, a skip request, and where
 * bookmarks go.  These are those questions, asked of this engine with a
 * fake application on the other side, and none of them depends on how a
 * voice sounds -- each compares the engine with itself.
 *
 * It needs extracted engine data and the native host built beside it, and
 * skips cleanly without them, so the gate is real on a machine with voices
 * and quiet on a build box.
 *
 *   speak_test.exe <data root> <voice id>...
 */
#include "outspoken_sapi.cpp"
#include <cstdio>
#include <algorithm>
#include <initializer_list>

static int g_fail;
static void check(bool ok, const char *what) {
    if(!ok){ g_fail++; printf("  FAIL  %s\n", what); }
    else     printf("  ok    %s\n", what);
}

/* This process's settings only: the files and the registry both pointed at
 * scratch places, so the run neither reads nor changes the listener's own. */
class TestSettings {
    HKEY user, machine; std::wstring path, files;
public:
    bool ready;
    TestSettings():user(0),machine(0),ready(false){
        wchar_t suffix[80];
        swprintf_s(suffix,L"Software\\OutspokenTests\\Speak-%lu",GetCurrentProcessId());
        path=suffix;
        wchar_t temp[MAX_PATH];
        if(GetTempPathW(MAX_PATH,temp)){
            swprintf_s(suffix,L"outspoken-speak-test\\settings-%lu",GetCurrentProcessId());
            files=std::wstring(temp)+suffix;
            CreateDirectoryW((std::wstring(temp)+L"outspoken-speak-test").c_str(),0);
            CreateDirectoryW(files.c_str(),0);
            SetEnvironmentVariableW(L"OUTSPOKEN_SAPI_SETTINGS_USER",(files+L"\\user.toml").c_str());
            SetEnvironmentVariableW(L"OUTSPOKEN_SAPI_SETTINGS_MACHINE",(files+L"\\machine.toml").c_str());
        }
        if(RegCreateKeyExW(HKEY_CURRENT_USER,(path+L"\\User").c_str(),0,0,0,KEY_ALL_ACCESS,0,&user,0)||
           RegCreateKeyExW(HKEY_CURRENT_USER,(path+L"\\Machine").c_str(),0,0,0,KEY_ALL_ACCESS,0,&machine,0))return;
        if(RegOverridePredefKey(HKEY_CURRENT_USER,user))return;
        if(RegOverridePredefKey(HKEY_LOCAL_MACHINE,machine)){RegOverridePredefKey(HKEY_CURRENT_USER,0);return;}
        ready=true;
    }
    ~TestSettings(){
        if(ready){RegOverridePredefKey(HKEY_CURRENT_USER,0);RegOverridePredefKey(HKEY_LOCAL_MACHINE,0);}
        if(user)RegCloseKey(user);
        if(machine)RegCloseKey(machine);
        RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());
        if(!files.empty())RemoveDirectoryW(files.c_str());
    }
};

class FakeToken : public ISpObjectToken {
    std::wstring root, voice;
public:
    FakeToken(const std::wstring &r,const std::wstring &v):root(r),voice(v){}
    STDMETHODIMP QueryInterface(REFIID,void**p){*p=this;return S_OK;}
    STDMETHODIMP_(ULONG) AddRef(){return 2;}
    STDMETHODIMP_(ULONG) Release(){return 1;}
    STDMETHODIMP GetStringValue(LPCWSTR n,LPWSTR *v){
        const std::wstring *s=!n?0:!wcscmp(n,L"DataPath")?&root:!wcscmp(n,L"VoiceId")?&voice:0;
        if(!s)return E_INVALIDARG;
        *v=(LPWSTR)CoTaskMemAlloc((s->size()+1)*sizeof(wchar_t));
        if(!*v)return E_OUTOFMEMORY;
        wcscpy_s(*v,s->size()+1,s->c_str()); return S_OK;
    }
    STDMETHODIMP SetData(LPCWSTR,ULONG,const BYTE*){return E_NOTIMPL;} STDMETHODIMP GetData(LPCWSTR,ULONG*,BYTE*){return E_NOTIMPL;}
    STDMETHODIMP SetStringValue(LPCWSTR,LPCWSTR){return E_NOTIMPL;} STDMETHODIMP SetDWORD(LPCWSTR,DWORD){return E_NOTIMPL;}
    STDMETHODIMP GetDWORD(LPCWSTR,DWORD*){return E_NOTIMPL;} STDMETHODIMP OpenKey(LPCWSTR,ISpDataKey**){return E_NOTIMPL;}
    STDMETHODIMP CreateKey(LPCWSTR,ISpDataKey**){return E_NOTIMPL;} STDMETHODIMP DeleteKey(LPCWSTR){return E_NOTIMPL;}
    STDMETHODIMP DeleteValue(LPCWSTR){return E_NOTIMPL;} STDMETHODIMP EnumKeys(ULONG,LPWSTR*){return E_NOTIMPL;}
    STDMETHODIMP EnumValues(ULONG,LPWSTR*){return E_NOTIMPL;} STDMETHODIMP SetId(LPCWSTR,LPCWSTR,BOOL){return E_NOTIMPL;}
    STDMETHODIMP GetId(LPWSTR*){return E_NOTIMPL;} STDMETHODIMP GetCategory(ISpObjectTokenCategory**){return E_NOTIMPL;}
    STDMETHODIMP CreateInstance(IUnknown*,DWORD,REFIID,void**){return E_NOTIMPL;}
    STDMETHODIMP GetStorageFileName(REFCLSID,LPCWSTR,LPCWSTR,ULONG,LPWSTR*){return E_NOTIMPL;}
    STDMETHODIMP RemoveStorageFileName(REFCLSID,LPCWSTR,BOOL){return E_NOTIMPL;}
    STDMETHODIMP Remove(const CLSID*){return E_NOTIMPL;}
    STDMETHODIMP IsUISupported(LPCWSTR,void*,ULONG,IUnknown*,BOOL*){return E_NOTIMPL;}
    STDMETHODIMP DisplayUI(HWND,LPCWSTR,LPCWSTR,void*,ULONG,IUnknown*){return E_NOTIMPL;}
    STDMETHODIMP MatchesAttributes(LPCWSTR,BOOL*){return E_NOTIMPL;}
};

/* The application: it can take part of a Write or none, change its volume
 * part way, ask for a skip, abort, and it notes when each bookmark came. */
class FakeSite : public ISpTTSEngineSite {
public:
    std::vector<BYTE> audio;
    struct Event { std::wstring name; ULONGLONG offset; size_t written; };
    std::vector<Event> events;
    long rate=0; USHORT volume=100;
    ULONG maxTake=0; int refuseEvery=0, writes=0;
    size_t abortAfter=0;
    size_t skipAfter=0, skipAt=0; long skipAsk=0, skipped=-999;
    bool skipAsked=false, skipInfoFails=false;
    STDMETHODIMP QueryInterface(REFIID,void**p){*p=this;return S_OK;}
    STDMETHODIMP_(ULONG) AddRef(){return 2;}
    STDMETHODIMP_(ULONG) Release(){return 1;}
    STDMETHODIMP AddEvents(const SPEVENT *e,ULONG n){
        for(ULONG i=0;i<n;i++)
            events.push_back({e[i].elParamType==SPET_LPARAM_IS_STRING&&e[i].lParam?
                              (const wchar_t*)e[i].lParam:L"",e[i].ullAudioStreamOffset,audio.size()});
        return S_OK;
    }
    STDMETHODIMP GetEventInterest(ULONGLONG *i){*i=SPEI_TTS_BOOKMARK;return S_OK;}
    STDMETHODIMP_(DWORD) GetActions(){
        if(abortAfter&&audio.size()>=abortAfter)return SPVES_ABORT;
        return (skipAfter&&!skipAsked&&audio.size()>=skipAfter)?SPVES_SKIP:0;
    }
    STDMETHODIMP Write(const void *p,ULONG n,ULONG *w){
        writes++;
        if(refuseEvery&&writes%refuseEvery==0){if(w)*w=0;return S_OK;}
        ULONG take=maxTake&&n>maxTake?maxTake:n;
        audio.insert(audio.end(),(const BYTE*)p,(const BYTE*)p+take);
        if(w)*w=take; return S_OK;
    }
    STDMETHODIMP GetRate(long *r){*r=rate;return S_OK;}
    STDMETHODIMP GetVolume(USHORT *v){*v=volume;return S_OK;}
    STDMETHODIMP GetSkipInfo(SPVSKIPTYPE *t,long *n){
        if(skipInfoFails)return E_NOTIMPL;
        *t=SPVST_SENTENCE;*n=skipAsk;return S_OK;
    }
    STDMETHODIMP CompleteSkip(long n){skipAsked=true;skipped=n;skipAt=audio.size();return S_OK;}
};

/* A fragment list as SAPI hands it over once it has parsed the markup.
 * Fragments are separated by "|"; "#name" is a bookmark; a fragment may
 * open with {r=N} (rate), {p=N} (pitch), {v=N} (volume), {s} (spell) or
 * {sil=N} (a silence of N ms, with no text). */
struct Script {
    std::vector<SPVTEXTFRAG> frags; std::vector<std::wstring> text;
    explicit Script(const std::wstring &s) {
        size_t at=0;
        for(;;){
            size_t bar=s.find(L'|',at);
            text.push_back(s.substr(at,bar==std::wstring::npos?std::wstring::npos:bar-at));
            if(bar==std::wstring::npos)break;
            at=bar+1;
        }
        frags.assign(text.size(),SPVTEXTFRAG());
        for(size_t i=0;i<text.size();i++){
            SPVTEXTFRAG &f=frags[i]; memset(&f,0,sizeof f);
            f.State.eAction=SPVA_Speak; f.State.Volume=100;
            std::wstring &t=text[i];
            while(!t.empty()&&t[0]==L'{'){
                size_t close=t.find(L'}');
                std::wstring m=t.substr(1,close-1); t.erase(0,close+1);
                if(m==L"s")f.State.eAction=SPVA_SpellOut;
                else if(!m.compare(0,4,L"sil=")){f.State.eAction=SPVA_Silence;f.State.SilenceMSecs=_wtol(m.c_str()+4);}
                else if(!m.compare(0,2,L"r="))f.State.RateAdj=_wtol(m.c_str()+2);
                else if(!m.compare(0,2,L"p="))f.State.PitchAdj.MiddleAdj=_wtol(m.c_str()+2);
                else if(!m.compare(0,2,L"v="))f.State.Volume=(USHORT)_wtol(m.c_str()+2);
            }
            if(!t.empty()&&t[0]==L'#'){f.State.eAction=SPVA_Bookmark;t.erase(0,1);}
            f.pTextStart=t.empty()?0:t.c_str(); f.ulTextLen=(ULONG)t.size();
            if(i+1<text.size())f.pNext=&frags[i+1];
        }
    }
};
/* Every utterance measured here is the second time its text is spoken.
 * MacinTalk 2's Ben says a text differently the first time a host meets it
 * -- the unchanged engine too: "The quick brown fox..." is 98326 bytes the
 * first time and 97870 every time after, whatever came between -- so two
 * renders are only comparable once both have been met. */
static HRESULT say(Engine *e, const std::wstring &script, FakeSite &site) {
    FakeSite before; before.rate=site.rate; before.volume=site.volume;
    Script p(script);
    e->Speak(0,GUID_NULL,0,p.frags.data(),&before);
    Script s(script);
    return e->Speak(0,GUID_NULL,0,s.frags.data(),&site);
}
static bool has_silence(const std::vector<BYTE> &pcm, size_t ms) {
    size_t need=ms*NATIVE_RATE/1000, run=0;
    const short *s=(const short*)pcm.data();
    for(size_t i=0;i<pcm.size()/2;i++){ run=s[i]?0:run+1; if(run>=need)return true; }
    return false;
}
static bool names_in_order(const FakeSite &s, std::initializer_list<const wchar_t*> want) {
    if(s.events.size()!=want.size())return false;
    size_t i=0;
    for(auto w:want)if(s.events[i++].name!=w)return false;
    return true;
}

static const size_t PAD = 6676;                /* the 150 ms tail, in bytes */

static void voice_checks(Engine *e) {
    const std::wstring LINE=L"The quick brown fox jumps over the lazy dog.";
    FakeSite base; say(e,LINE,base);
    const std::vector<BYTE> &first=base.audio;
    check(first.size()>PAD,"a plain utterance speaks");
    FakeSite again; say(e,LINE,again);
    check(again.audio==first,"and speaks the same the second time");

    /* Windows 7's SAPI takes nothing while its buffer is full, and any
     * application may take part of a block. */
    {
        FakeSite s; s.maxTake=1000; s.refuseEvery=3; say(e,LINE,s);
        check(s.audio==first,"writes taken in part, or refused, lose no audio");
    }
    /* The application's volume, which SAPI leaves to the engine. */
    {
        FakeSite s; s.volume=25; say(e,LINE,s);
        bool ok=s.audio.size()==first.size();
        for(size_t i=0;ok&&i<first.size()/2;i++)
            ok=((const short*)s.audio.data())[i]==((const short*)first.data())[i]/4;
        check(ok,"the application's volume scales every sample");
    }
    /* Markup combines with the application's own settings. */
    {
        FakeSite s; say(e,L"{v=50}"+LINE,s);
        bool ok=s.audio.size()==first.size();
        for(size_t i=0;ok&&i<first.size()/2;i++)
            ok=((const short*)s.audio.data())[i]==((const short*)first.data())[i]/2;
        check(ok,"volume markup scales the speech");
        FakeSite a; a.volume=50; say(e,L"{v=80}"+LINE,a);
        FakeSite b; b.volume=40; say(e,LINE,b);
        check(a.audio==b.audio,"volume markup is a share of the application's volume");
        FakeSite c; c.rate=5; say(e,L"{r=2}"+LINE,c);
        FakeSite d; d.rate=7; say(e,LINE,d);
        check(!c.audio.empty()&&c.audio==d.audio,"rate markup adds to the application's rate");
        FakeSite c2; c2.rate=5; say(e,L"{r=-2}"+LINE,c2);
        FakeSite d2; d2.rate=3; say(e,LINE,d2);
        check(c2.audio==d2.audio,"and slows it too");
        FakeSite f; f.rate=8; say(e,L"{r=8}"+LINE,f);
        FakeSite g; g.rate=10; say(e,LINE,g);
        check(f.audio==g.audio,"inside SAPI's -10..+10");
        FakeSite flat; say(e,L"{p=0}The quick brown fox|{p=0}jumps over the lazy dog.",flat);
        check(flat.audio==first,"fragments in one state are one utterance");
        FakeSite rise; say(e,L"{p=0}The quick brown fox|{p=5}jumps over the lazy dog.",rise);
        check(!rise.audio.empty()&&rise.audio!=flat.audio,"pitch markup reaches a later fragment");
    }
    /* Bookmarks.  Between the words of one utterance the place is an
     * estimate -- the engines speak an utterance whole -- so what is asked
     * of those is order and sense; at the edges it is exact. */
    {
        FakeSite s; say(e,L"The|#1|quick|#2|brown|#3|fox|#4|jumps over the lazy dog.",s);
        bool order=names_in_order(s,{L"1",L"2",L"3",L"4"});
        check(order,"a bookmark between every word: all of them, in order");
        bool rising=order, inside=order, aligned=true;
        for(size_t i=0;order&&i<s.events.size();i++){
            if(i&&s.events[i].offset<=s.events[i-1].offset)rising=false;
            if(!s.events[i].offset||s.events[i].offset>=s.audio.size()-PAD)inside=false;
            if(s.events[i].offset%2)aligned=false;
        }
        check(rising,"each later than the one before");
        check(inside,"each inside the speech");
        check(aligned,"on a sample boundary");
        FakeSite h; say(e,L"#head|"+LINE+L"|#tail",h);
        check(names_in_order(h,{L"head",L"tail"})&&h.events[0].offset==0&&h.events[0].written==0&&
              h.events[1].offset==h.audio.size()-PAD,
              "a mark before the words is at the start, one after them at their end");
        FakeSite m; say(e,L"#1|One.|#2|{p=4}Two.|#3",m);
        bool exact=names_in_order(m,{L"1",L"2",L"3"})&&m.events[1].written==m.events[1].offset&&
                   m.events[1].offset>0&&m.events[1].offset<m.audio.size()-PAD;
        check(exact,"a mark where the state changes is exact, and before the audio after it");
    }
    /* A text that makes no sound still answers its marks. */
    {
        const wchar_t *quiet[]={L"\x2014",L"...",L" ",L"\x00a0",L"\x200b",L"-----",L"."};
        int answered=0;
        for(auto q:quiet){
            FakeSite s; say(e,std::wstring(L"#7|")+q+L"|#8",s);
            if(names_in_order(s,{L"7",L"8"}))answered++;
        }
        check(answered==7,"marks around a text that makes no sound are answered");
        FakeSite m; say(e,L"#1|one|#2|\x2014|#3|two|#4",m);
        check(names_in_order(m,{L"1",L"2",L"3",L"4"}),"and a silent piece between words keeps its marks in order");
    }
    /* <spell> reads the characters, as NVDA's character mode does. */
    {
        FakeSite w; say(e,L"hello",w);
        FakeSite s; say(e,L"{s}hello",s);
        check(!s.audio.empty()&&s.audio!=w.audio,"spelled text is not read as the word");
        check(s.audio.size()>w.audio.size()*13/10,"it takes as long as letters do");
        FakeSite n; say(e,L"42",n);
        FakeSite d; say(e,L"{s}42",d);
        check(!d.audio.empty()&&d.audio!=n.audio,"spelled digits are digits, not a number");
        FakeSite k; say(e,L"#a|{s}ab|#b",k);
        check(names_in_order(k,{L"a",L"b"}),"marks around spelled text are answered");
    }
    /* <silence msec="500"/> is half a second of silence, between the two
     * halves of the text it divides. */
    {
        FakeSite h; say(e,L"Hello.",h);
        FakeSite w; say(e,L"World.",w);
        FakeSite b; say(e,L"Hello.|{sil=500}|World.",b);
        check(has_silence(b.audio,450),"a silence tag is silence");
        long extra=(long)b.audio.size()-(long)(h.audio.size()-PAD+w.audio.size());
        long want=(long)(NATIVE_RATE/2*2);
        check(extra>=want-200&&extra<=want+200,"of the length it asked for, between the words");
    }
    /* A skip request moves by sentences, and speech goes on from there.
     * Three sentences of clearly different lengths: what follows a skip has
     * to be, sample for sample, the text from the sentence it landed on. */
    {
        const std::wstring S1=L"One.", S2=L"This second sentence is a good deal longer than the first one.",
                           S3=L"And a third, of middling length.";
        const std::wstring ALL=S1+L" "+S2+L" "+S3, FROM2=S2+L" "+S3;
        FakeSite all; say(e,ALL,all);
        FakeSite from2; say(e,FROM2,from2);
        FakeSite from3; say(e,S3,from3);
        FakeSite one; say(e,S1,one);
        auto lands=[&](const FakeSite &s,const FakeSite &rest){
            bool ok=s.audio.size()==s.skipAt+rest.audio.size()&&
                    std::equal(rest.audio.begin(),rest.audio.end(),s.audio.begin()+s.skipAt)&&
                    std::equal(s.audio.begin(),s.audio.begin()+s.skipAt,all.audio.begin());
            if(!ok)printf("  --    skipped=%ld at %u: %u byte(s) after, %u expected\n",s.skipped,
                          (unsigned)s.skipAt,(unsigned)(s.audio.size()-s.skipAt),(unsigned)rest.audio.size());
            return ok;
        };
        /* A second past where the first sentence alone ends is well inside
         * the second, whatever pause the engine puts between them. */
        size_t inFirst=2048, inSecond=one.audio.size()-PAD+NATIVE_RATE*2;
        FakeSite a; a.skipAfter=inFirst; a.skipAsk=1; say(e,ALL,a);
        check(a.skipped==1&&lands(a,from2),"+1 in the first sentence goes on with the second");
        FakeSite b; b.skipAfter=inFirst; b.skipAsk=2; say(e,ALL,b);
        check(b.skipped==2&&lands(b,from3),"+2 goes on with the third");
        FakeSite c; c.skipAfter=inSecond; c.skipAsk=0; say(e,ALL,c);
        check(c.skipped==0&&lands(c,from2),"0 goes back to the start of the sentence");
        FakeSite d; d.skipAfter=inSecond; d.skipAsk=-1; say(e,ALL,d);
        check(d.skipped==-1&&lands(d,all),"-1 goes back to the one before");
        FakeSite f; f.skipAfter=inFirst; f.skipAsk=5; say(e,ALL,f);
        check(f.skipped==2&&f.audio.size()<=f.skipAt+PAD,
              "more than the text has: what could be skipped, and the call ends");
        FakeSite g; g.skipAfter=inFirst; g.skipInfoFails=true; say(e,ALL,g);
        check(g.skipped==0&&g.audio.size()<=g.skipAt+PAD,"an application that cannot say how far is told none");
        FakeSite m; m.skipAfter=inFirst; m.skipAsk=2;
        say(e,L"#a|"+S1+L" |#b|"+S2+L" |#c|"+S3,m);
        check(names_in_order(m,{L"a",L"c"}),"marks skipped over are not reported, the one before the landing is");
        FakeSite after; say(e,LINE,after);
        check(after.audio==first,"and the next call is unaffected");
    }
    /* No mark after an abort, and the engine speaks on unharmed. */
    {
        FakeSite s; s.abortAfter=4096;
        say(e,L"#1|This is a deliberately long paragraph, the kind a listener "
              L"interrupts long before it has finished saying itself.|#2",s);
        bool late=false;
        for(auto &ev:s.events)if(ev.name==L"2")late=true;
        check(!late,"an interrupted utterance reports no mark after the interruption");
        FakeSite after; say(e,LINE,after);
        check(after.audio==first,"and the next one is unharmed");
    }
}

int wmain(int argc, wchar_t **argv) {
    TestSettings settings;
    if(!settings.ready){fprintf(stderr,"cannot isolate test settings\n");return 1;}
    InitializeCriticalSection(&g_hostLock); g_lockReady=true;
    if(argc<3){fprintf(stderr,"usage: speak_test <data root> <voice id>...\n");return 2;}
    std::wstring host=module_dir()+L"\\osp_host.exe";
    if(GetFileAttributesW(host.c_str())==INVALID_FILE_ATTRIBUTES&&
       GetFileAttributesW((module_dir()+L"\\osp_host_x86.exe").c_str())==INVALID_FILE_ATTRIBUTES){
        printf("no native host beside the test; speak checks skipped\n");
        return 0;
    }
    for(int i=2;i<argc;i++){
        FakeToken tok(argv[1],argv[i]);
        Engine *e=new Engine; e->SetObjectToken(&tok);
        FakeSite probe; HRESULT hr=say(e,L"probe",probe);
        if(FAILED(hr)||probe.audio.empty()){
            wprintf(L"%s: not installed here, skipped\n",argv[i]);
            e->Release(); continue;
        }
        wprintf(L"%s:\n",argv[i]);
        voice_checks(e);
        e->Release();
    }
    host_drop();
    printf(g_fail?"\n%d check(s) FAILED\n":"\nall speak checks pass\n",g_fail);
    return g_fail?1:0;
}
