#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aigiskr::core {

std::wstring GetModuleDirectory(void* hmod = nullptr);
std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view s);
std::string SanitizeCommandLine(const wchar_t* cmdline);
bool ComputeFileSha256Hex(const std::wstring& path, std::string& hexOut, uint64_t& sizeOut);

class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logPath, const std::wstring& capturePath);
    void Shutdown();

    void Logf(const char* fmt, ...);
    void LogLine(std::string_view line);
    void WriteCaptureTsv(std::string_view tsvLine);

    uint64_t UptimeMs() const;

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};

}  // namespace aigiskr::core
