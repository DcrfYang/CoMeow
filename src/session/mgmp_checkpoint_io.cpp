#include "mgmp_checkpoint_io.h"
#include <windows.h>
#include <bcrypt.h>
#include "mgmp_proto.h"
#include <cstring>
#include <string>
#include <utility>
#pragma comment(lib, "bcrypt.lib")

namespace mgmp { namespace checkpoint_io {
bool read(const std::wstring& path, Bytes& out, uint32_t limit) {
    out.clear();
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (h==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n{}; DWORD got=0;
    bool ok=GetFileSizeEx(h,&n) && n.QuadPart>0 && n.QuadPart<=limit;
    if(ok) { out.resize((size_t)n.QuadPart); ok=ReadFile(h,out.data(),(DWORD)out.size(),&got,nullptr) && got==out.size(); }
    CloseHandle(h); if(!ok) out.clear(); return ok;
}
uint64_t nonce() {
    uint64_t v=0;
    if(BCryptGenRandom(nullptr,(PUCHAR)&v,sizeof(v),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return 0;
    return v;
}
bool atomic_write(const std::wstring& path, const Bytes& bytes) {
    if(bytes.empty()) return false;
    auto tmp=path+L".tmp-"+std::to_wstring(GetCurrentProcessId());
    HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD n=0;
    bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&n,nullptr) && n==bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    // Verify disk contents before the one atomic rename makes them visible.
    Bytes check; ok=ok && read(tmp,check,(uint32_t)bytes.size()) && check==bytes;
    if(ok) ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) DeleteFileW(tmp.c_str()); return ok;
}
// The only identity folder (a decimal number) under `root`, or 0 when there are none or several.
static uint64_t only_identity_dir(const wchar_t* root) {
    if(!root || !root[0]) return 0;
    std::wstring pat=std::wstring(root)+L"\\*";
    WIN32_FIND_DATAW fd{}; HANDLE h=FindFirstFileW(pat.c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE) return 0;
    uint64_t found=0; unsigned n=0;
    do {
        if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0]==L'.') continue;
        bool digits=fd.cFileName[0]!=0;
        for(const wchar_t* c=fd.cFileName;*c;++c) if(*c<L'0'||*c>L'9') digits=false;
        if(!digits) continue;
        wchar_t* end=nullptr; const uint64_t v=_wcstoui64(fd.cFileName,&end,10);
        if(!v) continue;
        {   // an identity folder with nothing in it (made by an install that never got as far as a save) is not a candidate
            std::wstring inner=std::wstring(root)+L"\\"+fd.cFileName+L"\\*";
            WIN32_FIND_DATAW f2{}; HANDLE h2=FindFirstFileW(inner.c_str(),&f2);
            bool any=false;
            if(h2!=INVALID_HANDLE_VALUE) { do { if(wcscmp(f2.cFileName,L".")&&wcscmp(f2.cFileName,L"..")) any=true; } while(!any && FindNextFileW(h2,&f2)); FindClose(h2); }
            if(!any) continue;
        }
        found=v; ++n;
    } while(FindNextFileW(h,&fd));
    FindClose(h);
    return n==1 ? found : 0;
}

uint64_t steam_id_from_dir(const wchar_t* dir) {
    if(!dir) return 0;
    uint64_t found=0;
    const wchar_t* p=dir;
    while(*p) {
        while(*p==L'\\'||*p==L'/') ++p;
        const wchar_t* b=p;
        while(*p && *p!=L'\\' && *p!=L'/') ++p;
        const size_t len=(size_t)(p-b);
        if(len!=17 || wcsncmp(b,L"7656119",7)!=0) continue;
        bool digits=true; for(size_t i=0;i<len;++i) if(b[i]<L'0'||b[i]>L'9') digits=false;
        if(!digits) continue;
        found=_wcstoui64(std::wstring(b,len).c_str(),nullptr,10);
    }
    return found;
}

bool identity(uint64_t& id, const wchar_t* adopt_root, bool* adopted) {
    if(adopted) *adopted=false;
    HMODULE module=nullptr; wchar_t file[MAX_PATH]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&identity),&module) || !GetModuleFileNameW(module,file,MAX_PATH)) return false;
    std::wstring path=file; path=path.substr(0,path.find_last_of(L"\\/"))+L"\\mgmp-peer-id.bin";
    Bytes bytes;
    if(read(path,bytes,16)) {
        if(bytes.size()!=16) return false;
        uint64_t hash=0; memcpy(&id,bytes.data(),8); memcpy(&hash,bytes.data()+8,8);
        return id && hash==savefile_hash(bytes.data(),8);
    }
    // Do not replace a corrupt identity: it could orphan existing recovery data.
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) return false;
    id=only_identity_dir(adopt_root);
    if(id) { if(adopted) *adopted=true; }
    else { id=nonce(); if(!id) return false; }
    bytes.resize(16); memcpy(bytes.data(),&id,8);
    uint64_t hash=savefile_hash(bytes.data(),8); memcpy(bytes.data()+8,&hash,8);
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD n=0; bool ok=WriteFile(h,bytes.data(),16,&n,nullptr) && n==16 && FlushFileBuffers(h);
    CloseHandle(h); return ok;
}

// Use SQLite's backup API, not a raw copy of a possibly WAL-backed live DB.
// winsqlite3 is loaded from System32 only; no game SQLite pointers cross DLLs.
struct Sql {
    HMODULE module=nullptr;
    int (*open)(const char*,void**,int,const char*)=nullptr;
    int (*close)(void*)=nullptr;
    int (*exec)(void*,const char*,int(*)(void*,int,char**,char**),void*,char**)=nullptr;
    void* (*init)(void*,const char*,void*,const char*)=nullptr;
    int (*step)(void*,int)=nullptr;
    int (*finish)(void*)=nullptr;
    bool load() {
        if(module) return open && close && exec && init && step && finish;
        module=LoadLibraryExW(L"winsqlite3.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) return false;
#define GET(field, name) field=reinterpret_cast<decltype(field)>(GetProcAddress(module,name))
        GET(open,"sqlite3_open_v2"); GET(close,"sqlite3_close"); GET(exec,"sqlite3_exec");
        GET(init,"sqlite3_backup_init"); GET(step,"sqlite3_backup_step"); GET(finish,"sqlite3_backup_finish");
#undef GET
        return open && close && exec && init && step && finish;
    }
} sql;
static std::string utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(n<1) return {}; std::string out(n,0);
    WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,&out[0],n,nullptr,nullptr); return out;
}
static int check_row(void* result,int n,char** vals,char**) {
    *(bool*)result=n==1 && vals[0] && strcmp(vals[0],"ok")==0; return 0;
}
static bool healthy(void* db) {
    bool ok=false; return sql.exec(db,"PRAGMA quick_check",check_row,&ok,nullptr)==0 && ok;
}
bool snapshot(const std::wstring& source, Bytes& out) {
    out.clear(); if(!sql.load()) return false;
    auto tmp=source+L".mgmp-snapshot-"+std::to_wstring(GetCurrentProcessId());
    DeleteFileW(tmp.c_str());
    void *src=nullptr,*dst=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&src,1,nullptr)==0 &&
            sql.open(utf8(tmp).c_str(),&dst,6,nullptr)==0;
    if(ok) {
        void* backup=sql.init(dst,"main",src,"main"); ok=backup!=nullptr;
        if(backup) { int step=sql.step(backup,-1); int end=sql.finish(backup); ok=step==101 && end==0; }
    }
    if(ok) ok=healthy(dst);
    if(dst) sql.close(dst); if(src) sql.close(src);
    if(ok) ok=read(tmp,out,kMaxSaveBytes);
    DeleteFileW(tmp.c_str()); return ok;
}
static int run_row(void* result,int n,char** vals,char**) {
    if(n==1 && vals[0]) *(bool*)result=strcmp(vals[0],"0")!=0; return 0;
}
bool in_run(const std::wstring& source,bool& running) {
    running=false; if(!sql.load()) return false;
    void* db=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&db,1,nullptr)==0;
    if(ok) ok=healthy(db) && sql.exec(db,"SELECT data FROM properties WHERE key='on_adventure'",run_row,&running,nullptr)==0;
    if(db) sql.close(db); return ok;
}
// "Is this save's adventure already ON THE MAP?" The durable signal is
// files.trollengine_state, the adventure's event engine: its header is
// u32 3, u32 0, u32 0, u32 <count>, and the count only starts accumulating
// once the party has entered the map. Measured 2026-09-28 on real saves: both
// 09:50 chapter-page saves (party chosen, chapter NOT clicked) count 0, while
// every on-map save counts nonzero (after-battle 1, mid-run 4, on-map fixture
// 7). The blob crosses the exec callback as hex() text -- ASCII only, so no
// NUL truncation and no sqlite version sensitivity (instr() on blobs predates
// 3.15 and silently casts to TEXT, which is a false match on every row).
static const char kTrollHex[]="SELECT hex(data) FROM files WHERE key='trollengine_state'";
static int map_row(void* result,int n,char** vals,char**) {
    // hex bytes 12..15 are the count (hex chars 24..31). Zero -> departure
    // preparation, not on the map. A NULL cell, a short blob or a missing row
    // leaves the caller's conservative default.
    if(n==1 && vals[0] && strlen(vals[0])>=32) {
        const char* count=vals[0]+24;
        *(bool*)result=memcmp(count,"00000000",8)!=0;
    }
    return 0;
}
// THE EVENT COUNT ALONE IS NOT ENOUGH (2026-10-01). A save written after the party entered the map but
// before its first node resolved counts 0 too -- measured on the host's steamcampaign02.sav, which then
// passed as "preparation", was offered the fresh start from the warehouse, and loaded straight onto the
// map. The property `adventure_started` is the game's own word for it: across every real save and backup
// on this machine (2026-06..2026-10) it is 0 in every departure-preparation save (on_adventure 1, count 0)
// and 1 in every save on the map, including the four with a count of 0. Either signal is enough.
static int started_row(void* result,int n,char** vals,char**) {
    if(n==1 && vals[0] && strcmp(vals[0],"0")!=0) *(bool*)result=true;
    return 0;
}
bool on_map(const std::wstring& source,bool& started) {
    started=true; if(!sql.load()) return false;
    void* db=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&db,1,nullptr)==0;
    if(ok) ok=healthy(db) && sql.exec(db,kTrollHex,map_row,&started,nullptr)==0;
    if(ok && !started) {
        bool departed=false;
        ok=sql.exec(db,"SELECT data FROM properties WHERE key='adventure_started'",started_row,&departed,nullptr)==0;
        if(ok && departed) started=true;
    }
    if(db) sql.close(db); return ok;
}
// --- fresh-session chapter_map row sync --------------------------------------
// The row travels as hex() text through the exec callback (ASCII only: no NUL
// truncation, no version-sensitive blob handling) and is written back as an
// x'...' blob literal, so the whole sync needs no sqlite entry point beyond
// open/exec/close that the recovery gate does not already use.
struct FileRowOut { int have; Bytes* out; };
static void hex_to_bytes(const char* hex,uint32_t n,Bytes& out) {
    auto nib=[](char c)->int { return c<='9'?c-'0':((c|32)-'a'+10); };
    out.clear(); out.reserve(n/2);
    for(uint32_t i=0;i+1<n;i+=2) out.push_back((uint8_t)((nib(hex[i])<<4)|nib(hex[i+1])));
}
static int file_row(void* result,int n,char** vals,char**) {
    auto* r=(FileRowOut*)result;
    if(n==1 && vals[0]) { r->have=1; hex_to_bytes(vals[0],(uint32_t)strlen(vals[0]),*r->out); }
    return 0;
}
static int count_row(void* result,int n,char** vals,char**) {
    if(n==1 && vals[0]) *(int*)result=atoi(vals[0]); return 0;
}
static bool files_table_exists(void* db,int& exists) {
    exists=0;
    return sql.exec(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name='files'",count_row,&exists,nullptr)==0;
}
bool read_file_row(const std::wstring& source,const char* key,int& have,Bytes& out) {
    have=0; out.clear(); if(!sql.load()) return false;
    void* db=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&db,1,nullptr)==0;
    if(ok) ok=healthy(db);
    int exists=0;
    if(ok) ok=files_table_exists(db,exists);
    if(ok && exists) {
        char query[96]; _snprintf_s(query,_TRUNCATE,"SELECT hex(data) FROM files WHERE key='%s'",key);
        FileRowOut row{0,&out};
        ok=sql.exec(db,query,file_row,&row,nullptr)==0;
        if(ok) have=row.have;
    }
    if(db) sql.close(db);
    return ok;   // a missing table/row is NOT an error: it is a clean save
}
bool write_file_row(const std::wstring& target,const char* key,const Bytes& in) {
    if(in.empty() || in.size()>kMaxSaveBytes || !sql.load()) return false;
    static const char kHexDigits[]="0123456789ABCDEF";
    std::string hex; hex.reserve(in.size()*2);
    for(uint8_t b: in) { hex.push_back(kHexDigits[b>>4]); hex.push_back(kHexDigits[b&15]); }
    std::string q="INSERT OR REPLACE INTO files(key,data) VALUES('";
    q+=key; q+="',x'"; q+=hex; q+="')";
    void* db=nullptr;
    bool ok=sql.open(utf8(target).c_str(),&db,2|4,nullptr)==0;
    if(ok) ok=sql.exec(db,q.c_str(),nullptr,nullptr,nullptr)==0;
    if(db) sql.close(db);
    return ok;
}
static int prop_row(void* result,int n,char** vals,char**) {
    auto* r=static_cast<std::pair<bool*,std::string*>*>(result);
    if(n>=1 && vals[0]) { *r->first=true; *r->second=vals[0]; }
    return 0;
}
static int all_prop_row(void* result,int n,char** vals,char**) {
    auto* v=static_cast<std::vector<std::pair<std::string,std::string>>*>(result);
    if(n>=2 && vals[0]) v->emplace_back(vals[0], vals[1]?vals[1]:"");
    return 0;
}
bool read_all_properties(const std::wstring& source,std::vector<std::pair<std::string,std::string>>& out) {
    out.clear(); if(!sql.load()) return false;
    void* db=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&db,1,nullptr)==0;
    if(ok) ok=sql.exec(db,"SELECT key, data FROM properties",all_prop_row,&out,nullptr)==0;
    if(db) sql.close(db);
    return ok;
}
bool read_property(const std::wstring& source,const char* key,bool& have,std::string& out) {
    have=false; out.clear(); if(!key || !sql.load()) return false;
    for(const char* p=key;*p;++p) if(*p=='\'') return false;   // keys are ours; refuse rather than escape
    void* db=nullptr;
    bool ok=sql.open(utf8(source).c_str(),&db,1,nullptr)==0;
    if(ok) {
        std::string q=std::string("SELECT data FROM properties WHERE key='")+key+"'";
        std::pair<bool*,std::string*> r{&have,&out};
        ok=sql.exec(db,q.c_str(),prop_row,&r,nullptr)==0;
    }
    if(db) sql.close(db); return ok;
}
bool restore(const std::wstring& target,const Bytes& bytes) {
    // Called ONLY before ContinueSlot. Never replace an open game database.
    for(auto suffix:{L"-wal",L"-shm",L"-journal"})
        if(GetFileAttributesW((target+suffix).c_str())!=INVALID_FILE_ATTRIBUTES) return false;
    auto staged=target+L".mgmp-restore";
    if(!atomic_write(staged,bytes)) return false;
    bool running=false;
    if(!in_run(staged,running) || !running) { DeleteFileW(staged.c_str()); return false; }
    // Sidecar absence is insufficient: a second game can keep a rollback-mode
    // DB open without a journal. Require exclusive access before replacement.
    HANDLE lease=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(lease==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n{}; DWORD got=0; Bytes before;
    bool ok=GetFileSizeEx(lease,&n) && n.QuadPart>0 && n.QuadPart<=kMaxSaveBytes;
    if(ok) {
        before.resize((size_t)n.QuadPart);
        ok=ReadFile(lease,before.data(),(DWORD)before.size(),&got,nullptr) && got==before.size();
    }
    if(ok) ok=atomic_write(target+L".before-mgmp-recovery",before);
    CloseHandle(lease); // Windows replacement itself needs to open the destination.
    if(ok) ok=MoveFileExW(staged.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    return ok;
}
} }
