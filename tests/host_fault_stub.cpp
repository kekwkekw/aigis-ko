#include "hdtrans_host/host_protocol.hpp"
#include <cstdlib>
#include <string>
using namespace aigiskr::hdtrans_host;
int main() {
    const char* env = std::getenv("ASTRA_CASE");
    const std::string mode = env ? env : "normal";
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        RequestHeader h; std::string body;
        if (ReadRequestFrame(in,h,body) != PipeIoStatus::Ok) return 0;
        if (h.opcode == 1) {
            if (mode == "hello_timeout") Sleep(INFINITE);
            WriteResponseFrame(out,Status::Ok,h.requestId,"host_version=stub\nengine_version=stub\n");
        } else if (h.opcode == 2) {
            if (mode == "timeout") Sleep(INFINITE);
            if (mode == "crash") return 73;
            if (mode == "truncated") { const uint8_t bad[4]={0x31,0x54,0x44,0x48}; WriteExactBytes(out,bad,4); return 0; }
            WriteResponseFrame(out,Status::Ok,h.requestId+(mode == "wrong_id" ? 1u : 0u),mode == "invalid_utf8" ? std::string("\xff\xfe",2) : std::string("정상 번역"));
            if (mode == "shutdown_hang") Sleep(INFINITE);
        } else if (h.opcode == 3) {
            if (mode == "shutdown_hang") Sleep(INFINITE);
            WriteResponseFrame(out,Status::Ok,h.requestId,"bye"); return 0;
        }
    }
}
