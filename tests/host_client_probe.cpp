#include "core/hdtrans_host_client.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
int wmain(int argc,wchar_t** argv) {
    if(argc<3 || argc>4) return 2;
    const int iterations=argc==4 ? _wtoi(argv[3]) : 1;
    if(iterations<1 || iterations>100) return 2;
    DWORD baselineHandles=0, finalHandles=0;
    for(int i=0;i<iterations;++i) {
    {
    aigiskr::core::HdTransHostClient client;
    client.Configure(argv[1],"bundled",200);
    std::string detail,translated;
    const bool init=client.Initialize(argv[2],&detail);
    std::printf("init=%d detail=%s\n",init,detail.c_str()); std::fflush(stdout);
    if(init) {
        const bool ok=client.TranslateUtf8("日本語の翻訳試験",translated,&detail);
        std::printf("translate=%d bytes=%zu failure=%d\n",ok,translated.size(),int(client.LastFailureKind())); std::fflush(stdout);
    }
    std::puts("shutdown_begin"); std::fflush(stdout);
    client.Shutdown(true);
    std::puts("shutdown_end"); std::fflush(stdout);
    }
    GetProcessHandleCount(GetCurrentProcess(),&finalHandles);
    if(i==0) baselineHandles=finalHandles;
    }
    std::printf("handles_baseline=%lu handles_final=%lu delta=%ld\n",baselineHandles,finalHandles,
                static_cast<long>(finalHandles)-static_cast<long>(baselineHandles));
    return finalHandles>baselineHandles ? 3 : 0;
}
