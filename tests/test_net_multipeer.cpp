// Four real processes, Winsock loopback; no game, loader or save files.
#include "mgmp_net.h"
#include "mgmp_log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
using namespace mgmp;
namespace mgmp { void log_line(const char*,const char*,...){} }
int run(bool host,unsigned port){
    if(!(host?net_host((uint16_t)port):net_join("127.0.0.1",(uint16_t)port)))return 2;
    PROCESS_INFORMATION children[3]{};
    if(host){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
        for(auto& child:children){wchar_t cmd[33000];swprintf_s(cmd,L"\"%ls\" --client %u",exe,port);
            STARTUPINFOW si{};si.cb=sizeof(si);
            if(!CreateProcessW(exe,cmd,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&child))return 3;
            CloseHandle(child.hThread);
        }
    }
    bool sent=false,acked=false;unsigned seen=0,done=0;int result=4;
    ULONGLONG deadline=GetTickCount64()+15000;
    while(GetTickCount64()<deadline){
        if(!sent&&net_peer_count()==4){
            uint8_t body[3]={net_self(),42,99};CatDataMsg c{};c.id=0x70000001ull+((uint64_t)net_self()<<24);
            c.data=body;c.size=3;c.hash=0x1234;sent=net_send_catdata(c);
        }
        NetMsg m{};
        while(net_poll(m)){
            if(m.type==MSG_CATDATA){
                if(m.from>=4||m.from==net_self()||m.catdata.id!=0x70000001ull+((uint64_t)m.from<<24)||
                   m.catdata.size!=3||m.catdata.data[0]!=m.from||m.catdata.data[1]!=42||m.catdata.data[2]!=99){
                    net_msg_release(m);net_shutdown();return 5;
                }
                seen|=1u<<m.from;
            }
            if(m.type==MSG_HASH)done|=1u<<m.from;
            net_msg_release(m);
        }
        if(sent && (seen|(1u<<net_self()))==15){
            if(!host){HashMsg h{};acked=net_send_hash(h);if(acked){result=0;Sleep(300);break;}}   // let the frame leave before the socket is closed: without it the host sometimes counted 10 or 12 of 14 acknowledgements (a test race, not a network loss)
            else if((done&14)==14){result=0;break;}
        }
        Sleep(1);
    }
    net_shutdown();
    if(host)for(auto& child:children){
        if(WaitForSingleObject(child.hProcess,20000)!=WAIT_OBJECT_0)result=6;
        DWORD code=1;GetExitCodeProcess(child.hProcess,&code);if(code)result=7;
        CloseHandle(child.hProcess);
    }
    if(host)printf("four-process CATDATA relay: %s (seen=%u, acknowledged=%u)\n",result?"FAILED":"PASSED",seen,done);
    return result;
}
int main(int argc,char** argv){return run(argc==1,argc==1?41000+GetCurrentProcessId()%20000:(unsigned)atoi(argv[2]));}
