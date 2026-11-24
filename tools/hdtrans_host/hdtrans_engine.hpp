// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace aigiskr::hdtrans_host {

class HdTransEngine final {
public:
    HdTransEngine() = default;
    ~HdTransEngine();

    HdTransEngine(const HdTransEngine&) = delete;
    HdTransEngine& operator=(const HdTransEngine&) = delete;

    bool Initialize(const std::wstring& rootPath, std::string* detailOut = nullptr);
    bool TranslateUtf8(std::string_view inputUtf8,
                       std::string& outTranslatedUtf8,
                       std::string* errorOut = nullptr);
    void Shutdown();

    bool IsInitialized() const noexcept { return initialized_; }
    const std::string& EngineVersion() const noexcept { return engineVersion_; }
    const std::string& DictsVersion() const noexcept { return dictsVersion_; }
    const std::wstring& ResolvedRoot() const noexcept { return resolvedRoot_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool initialized_ = false;
    std::string engineVersion_;
    std::string dictsVersion_;
    std::wstring resolvedRoot_;
};

std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

}  // namespace aigiskr::hdtrans_host
