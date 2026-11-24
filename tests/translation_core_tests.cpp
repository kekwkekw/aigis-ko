#include "core/auto_translate.hpp"
#include "core/config.hpp"
#include "core/coverage.hpp"
#include "core/hdtrans_adapter.hpp"
#include "core/hdtrans_host_client.hpp"
#include "core/knowledge_layer.hpp"
#include "core/logger.hpp"
#include "core/mini_json.hpp"
#include "core/pending_text_refresh.hpp"
#include "core/pending_text_sequence.hpp"
#include "core/mem_safe.hpp"
#include "core/tag_parser.hpp"
#include "core/template_matcher.hpp"
#include "core/translation_core.hpp"
#include "diagnostics/healthcheck.hpp"
#include "hooks/live_text_refresh.hpp"
#include "hdtrans_host/host_protocol.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using aigiskr::core::ClassifyTag;
using aigiskr::core::ComputeShapeKey;
using aigiskr::core::CoverageBucket;
using aigiskr::core::CoverageTracker;
using aigiskr::core::ExtractProtectedTags;
using aigiskr::core::IgnoredSubbucket;
using aigiskr::core::MatchTier;
using aigiskr::core::RedactSensitiveTokens;
using aigiskr::core::ShouldIgnoreForCoverage;
using aigiskr::core::TagKind;
using aigiskr::core::TranslateOutcome;
using aigiskr::core::TranslationCore;
using aigiskr::core::UnmatchedSubbucket;
using aigiskr::core::ValidateTagPreservation;
using aigiskr::diagnostics::BuildSafetyState;
using aigiskr::diagnostics::EvaluateBuildSafety;
using aigiskr::diagnostics::EvaluateTextureHealth;
using aigiskr::diagnostics::HealthcheckReport;
using aigiskr::diagnostics::SignatureCheckRecord;
using aigiskr::diagnostics::TextureProfileState;
using aigiskr::diagnostics::WriteHealthcheckJson;

namespace {

int g_failures = 0;

void Check(bool cond, const char* name) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

}  // namespace

int main(int argc, char** argv) {
    {
        const aigiskr::hooks::RefreshStreamOrigin origin{0x111,0x222,0x333};
        Check(origin.Preserves(0x555, 0, true) && origin.Preserves(0x111, 0, false) &&
              origin.Preserves(0x222, 0x333, false) && !origin.Preserves(0x222, 0x444, false) &&
              !origin.Preserves(0x555, 0, false),
              "Live refresh uses relocated method origins and measurement state, invalidating independent writes");
        Check(!aigiskr::hooks::RefreshStreamOrigin{}.Preserves(0, 0, false),
              "Unresolved stream origins never authorize a write");
        aigiskr::core::PendingTextRefresh bindings(2);
        aigiskr::core::PendingTextRefresh::Binding first, other, newer;
        bindings.Track(100, "原文", "原文", 7);
        bindings.Track(200, "原文", "原文", 7);
        Check(!bindings.Candidate(100, 7, 0, first) && !bindings.Candidate(100, 8, 1, first),
              "Live refresh waits for completion and stays on the original UI thread");
        Check(!bindings.Track(300, "追加", "追加", 7) && bindings.Size() == 2,
              "Live refresh has bounded pending storage");
        Check(bindings.Candidate(100, 7, 1, first) && bindings.Candidate(200, 7, 1, other) &&
              bindings.Consume(100, first.serial, "原文") && !bindings.Consume(200, other.serial, "新しい文"),
              "Live refresh updates unchanged owner only; same job cannot overwrite changed text");
        Check(bindings.Size() == 0 && !bindings.Candidate(100, 7, 2, first),
              "Live refresh consumes a completion once");
        bindings.Track(100, "原文", "原文", 7);
        bindings.Candidate(100, 7, 2, first);
        bindings.Forget(100); // destruction/clear
        Check(!bindings.Consume(100, first.serial, "原文"), "Live refresh discards destroyed owner");
        bindings.Track(100, "原文", "原文", 7); // same address and source, new lifetime
        Check(!bindings.Consume(100, first.serial, "原文") && bindings.Candidate(100, 7, 2, newer) &&
              bindings.Consume(100, newer.serial, "原文"),
              "Live refresh rejects recycled-address completion without losing the new binding");
        bindings.Track(100, "原文", "原文", 7);
        bindings.Forget(100); // a new non-translatable/empty text write
        Check(!bindings.Candidate(100, 7, 3, first), "Live refresh does not resurrect cleared or replaced text");
        bindings.Track(100, "原文", "原文", 7);
        bindings.Candidate(100, 7, 4, first);
        Check(bindings.Consume(100, first.serial, ""), "Live refresh accepts intentional empty logical text without a per-screen allowlist");
        bindings.Track(100, "案内", "案内", 7);
        bindings.Candidate(100, 7, 4, first);
        Check(bindings.Consume(100, first.serial, ""),
              "Empty-owner single write can be consumed with unchanged lifetime");
        bindings.Track(100, "案内", "案内", 7);
        bindings.Candidate(100, 7, 4, first);
        Check(!bindings.Consume(100, first.serial, "別の文"), "Direct modal refresh rejects changed logical text");
        bindings.Track(100, "案内", "案内", 7);
        bindings.Candidate(100, 7, 4, first);
        bindings.Forget(100);
        bindings.Track(100, "新しい案内", "新しい案内", 7);
        Check(!bindings.Consume(100, first.serial, "") && bindings.Candidate(100, 7, 4, newer) &&
              bindings.Consume(100, newer.serial, ""), "Direct modal refresh rejects stale lifetime even when both owners are empty");
    }
    {
        using Sequence = aigiskr::core::PendingTextSequence;
        Sequence sequence;
        Sequence::Binding candidate, newer;
        std::array<uint32_t, 10> colors{1,2,3,4,5,6,7,8,9,10};
        sequence.Append(100, true, 7, {"原文A", "A", "原文A", true}, colors);
        sequence.Append(100, false, 7, {"原文B", "B", "캐시 번역", false}, {});
        sequence.Append(100, false, 7, {"原文C", "C", "原文C", true}, {});
        Check(!sequence.Candidate(100, 7, 0, candidate) && !sequence.Candidate(100, 8, 1, candidate) &&
              sequence.Candidate(100, 7, 1, candidate), "Story completion stays on its UI thread and waits for a new revision");
        Check(Sequence::Resolve(candidate, [](const std::string& key, std::string& out) {
                  if (key != "A") return false; out = "번역 A"; return true;
              }) && sequence.Commit(100, candidate, true) && candidate.parts.size() == 3 &&
              candidate.parts[0].rendered == "번역 A" && candidate.parts[1].rendered == "캐시 번역" &&
              candidate.parts[2].rendered == "原文C" && candidate.parts[2].pending && candidate.initialColors == colors,
              "Story partial completion preserves order, cached lines, unresolved lines, and initial colors");
        Check(!sequence.Candidate(100, 7, 1, newer) && sequence.Candidate(100, 7, 2, newer) &&
              Sequence::Resolve(newer, [](const std::string& key, std::string& out) {
                  if (key != "C") return false; out = "번역 C"; return true;
              }) && sequence.Commit(100, newer, true) && !sequence.Candidate(100, 7, 3, candidate),
              "Story later completion updates the remaining line then stops polling resolved text");
        sequence.Append(100, false, 7, {"D", "D", "D", true}, {});
        Check(sequence.Candidate(100, 7, 2, candidate) && candidate.parts.size() == 4 &&
              candidate.parts[0].rendered == "번역 A" && candidate.parts[2].rendered == "번역 C",
              "Story append after completion keeps all previously rendered lines");
        sequence.Forget(100); // native clear, destroy, or unrelated write
        Check(!sequence.Commit(100, candidate, true) && !sequence.Candidate(100, 7, 4, newer),
              "Story clear cancels late completions");
        sequence.Append(100, true, 7, {"NEW", "NEW", "NEW", true}, {});
        Check(!sequence.Commit(100, candidate, true) && sequence.Candidate(100, 7, 4, newer) &&
              newer.parts.size() == 1 && newer.parts[0].source == "NEW", "Story recycled address cannot receive the previous turn");
        sequence.Append(100, false, 7, {"TAIL", "TAIL", "TAIL", false}, {});
        Check(!sequence.Commit(100, newer, true), "Story append invalidates a snapshot taken before the append");
        sequence.Candidate(100, 7, 5, newer);
        Check(!sequence.Commit(100, newer, false) && !sequence.Candidate(100, 7, 6, candidate),
              "Story refresh rejects a changed logical owner");
        Sequence bounded(1, 2, 32);
        Check(!bounded.Append(1, false, 7, {"a","a","a",true}, {}) &&
              bounded.Append(1, true, 7, {"a","a","a",true}, {}) &&
              !bounded.Append(2, true, 7, {"a","a","a",true}, {}), "Story requires a complete start and bounds object storage");
        bounded.Append(1, false, 7, {"b","b","b",true}, {});
        Check(!bounded.Append(1, false, 7, {"c","c","c",true}, {}) &&
              !bounded.Append(1, true, 7, {"a","a","a",true}, {}) && !bounded.Candidate(1, 7, 1, candidate),
              "Story line limit disables the whole turn until a real clear");
        bounded.Forget(1);
        bounded.Append(1, true, 7, {"a","a","a",true}, {});
        bounded.Candidate(1, 7, 1, candidate);
        Sequence::Resolve(candidate, [](const std::string&, std::string& out) { out.assign(40, 'x'); return true; });
        Check(!bounded.Commit(1, candidate, true), "Story translated output also obeys the memory bound");
        bounded.Forget(1);
        bounded.Append(1, true, 7, {"a","a","a",true}, {});
        Check(!bounded.Append(1, false, 8, {"b","b","b",true}, {}) && !bounded.Candidate(1, 7, 2, candidate),
              "Story mixed-thread writes disable unsafe replay");
        bounded.Forget(1);
        bounded.Block(1);
        Check(!bounded.Append(1, true, 7, {"a","a","a",true}, {}), "Story unreadable lines cannot leave a replayable partial turn");
    }
    {
        char buffer[4098]{};
        size_t length = 0;
        std::string source(1773, 'x');
        Check(aigiskr::core::SafeCopyCompleteCString(source.c_str(), buffer, sizeof(buffer), &length) &&
              length == source.size() && source == buffer, "Complete source reader preserves text beyond the former 1023-byte prefix");
        source.assign(4096, 'x');
        Check(aigiskr::core::SafeCopyCompleteCString(source.c_str(), buffer, sizeof(buffer), &length) && length == 4096,
              "Complete source reader accepts the configured maximum");
        source.push_back('x');
        Check(!aigiskr::core::SafeCopyCompleteCString(source.c_str(), buffer, sizeof(buffer), &length) && length == 0 && buffer[0] == 0,
              "Complete source reader rejects overlong text without exposing a prefix");
        Check(!aigiskr::core::SafeCopyCompleteCString(nullptr, buffer, sizeof(buffer), &length),
              "Complete source reader safely rejects unreadable text");
    }
    {
        aigiskr::core::PendingTextRefresh singles;
        bool accepted = true;
        for (uintptr_t i=1; i<=5000; ++i) accepted &= singles.Track(i,"原文","原文",7);
        Check(accepted && singles.Size() == 5000, "Production pending bindings have no former 4096-object cutoff");
        aigiskr::core::PendingTextSequence lines;
        for (uintptr_t i=1; i<=80; ++i) {
            accepted &= lines.Append(i,true,7,{std::string(2000,'x'),"key","text",true},{});
            for (int n=0;n<40;++n) accepted &= lines.Append(i,false,7,{std::string(2000,'x'),"key","text",true},{});
        }
        aigiskr::core::PendingTextSequence::Binding b;
        Check(accepted && lines.Size()==80 && lines.Candidate(80,7,1,b) && b.parts.size()==41,
              "Production sequences exceed former 64 objects, 32 parts and 64KiB accounting limits");
        lines.Append(80,false,7,{"","","",false},{});
        Check(lines.Candidate(80,7,1,b) && b.parts.size()==42 && b.parts.back().source.empty(),
              "Story capture preserves a real blank line instead of rejecting the entire turn");
        std::string raw(6000,'x'), copied;
        Check(aigiskr::core::SafeReadCompleteCString(raw.c_str(),copied) && copied==raw,
              "Native source reader accepts a complete source beyond 4096 bytes");
        raw.assign(aigiskr::core::kMaxNativeTextBytes,'x');
        Check(aigiskr::core::SafeReadCompleteCString(raw.c_str(),copied) && copied==raw,
              "Native source reader accepts the exact protocol bound");
        raw.push_back('x');
        Check(!aigiskr::core::SafeReadCompleteCString(raw.c_str(),copied) && copied.empty(),
              "Physical source bound rejects overflow without translating a partial prefix");
        static_assert(aigiskr::core::kMaxNativeTextBytes == aigiskr::hdtrans_host::kMaxPayloadBytes);
    }
    TranslationCore core;

    std::wstring samplePath = L"data\\translations_test\\r0_sample.json";
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        std::string arg(argv[1]);
        samplePath.assign(arg.begin(), arg.end());
    }

    std::string err;
    const bool loaded = core.LoadFromJsonFile(samplePath, &err);
    Check(loaded, "LoadFromJsonFile(r0_sample.json)");
    if (!loaded) {
        std::fprintf(stderr, "  Load error: %s\n", err.c_str());
        return 1;
    }
    Check(core.GetStats().loadedEntries >= 20, "Loaded >= 20 entries from r0_sample.json");

    // 1. Original R0 regression suite
    {
        auto r = core.Translate("ご指示をお願いします。");
        Check(r.outcome == TranslateOutcome::Hit && r.tier == MatchTier::Exact && r.text == "지시를 내려주세요.",
              "Exact single-line hit");
    }
    {
        auto r = core.Translate("達成率:31%");
        Check(r.outcome == TranslateOutcome::Hit && r.text == "달성률: 31%", "Trailing percent literal hit");
    }
    {
        const std::string crlfInput =
            "ビギナー任務1：\r\n戦術指南クエスト「応用演習：強敵との戦い方」クリア";
        const std::string expectedCrlf =
            "초보자 임무 1:\r\n전술 지침 퀘스트 「응용 연습: 강적과의 싸움」 클리어";
        auto r = core.Translate(crlfInput);
        Check(r.outcome == TranslateOutcome::Hit && r.text == expectedCrlf, "CRLF normalized lookup and preservation");
    }
    {
        auto r = core.Translate("%c[ff0000]攻撃力%c[ffffff]が<TIME>の間[ATK]上昇");
        Check(r.outcome == TranslateOutcome::Hit &&
                  r.text == "%c[ff0000]공격력%c[ffffff]이 <TIME> 동안 [ATK] 상승",
              "Valid color + angle + bracket tags preserved");
    }
    {
        auto r = core.Translate("データダウンロード中...( %d/%d )\n");
        Check(r.outcome == TranslateOutcome::Hit &&
                  r.text == "데이터 다운로드 중...( %d/%d )\n",
              "Printf specifiers preserved");
    }
    {
        const std::string brokenSrc = "壊れたタグテスト<TIME>";
        auto r = core.Translate(brokenSrc);
        Check(r.outcome == TranslateOutcome::TagMismatchFallback && r.text == brokenSrc,
              "Broken tag falls back to source");
    }
    {
        auto r = core.Translate("未登録のテキスト123");
        Check(r.outcome == TranslateOutcome::Miss && r.text.empty(), "Unknown text passthrough");
    }
    {
        auto r = core.Translate("");
        Check(r.outcome == TranslateOutcome::Miss && r.text.empty(), "Empty string edge case");
    }
    {
        TranslationCore inlineCore;
        std::string inlineErr;
        const bool ok = inlineCore.LoadFromJsonBytes(
            R"({"_meta":{"v":1},"entries":{"\u3053\u3093\u306b\u3061\u306f":"안녕하세요","tag\u0020%s":"태그 %s"}})",
            &inlineErr);
        auto r1 = inlineCore.Translate("こんにちは");
        auto r2 = inlineCore.Translate("tag %s");
        Check(ok && r1.outcome == TranslateOutcome::Hit && r1.text == "안녕하세요", "JSON unicode escape decoding");
        Check(r2.outcome == TranslateOutcome::Hit && r2.text == "태그 %s", "Printf token re-check on inline JSON");

        TranslationCore badCore;
        std::string badErr;
        Check(!badCore.LoadFromJsonBytes(R"({"_meta":{}})", &badErr), "Reject JSON without exact/entries object");
    }
    {
        const auto stats = core.GetStats();
        Check(stats.hits == 5 && stats.tagFallbacks == 1 && stats.misses == 1, "R0 stats counters accurate");
    }

    // 2. Tag classification & edge-case safety
    {
        Check(ClassifyTag("%%") == TagKind::Literal, "ClassifyTag %% -> Literal");
        Check(ClassifyTag("%c[ff0000]") == TagKind::Style, "ClassifyTag %c[ff0000] -> Style");
        Check(ClassifyTag("<TIME>") == TagKind::EngineControl, "ClassifyTag <TIME> -> EngineControl");
        Check(ClassifyTag("[ATK]") == TagKind::Parameter, "ClassifyTag [ATK] -> Parameter");
        Check(ClassifyTag("%.1f") == TagKind::Parameter, "ClassifyTag %.1f -> Parameter");

        // Literal % followed by Japanese/Korean must NEVER be confused with printf format spec
        const auto pctTags = ExtractProtectedTags("攻撃力が50%上昇");
        Check(pctTags.empty(), "50% followed by kanji produces zero false printf tags");

        // Case-sensitivity & color tampering detection
        Check(!ValidateTagPreservation("%c[ff0000]A", "%c[FF0000]A").valid, "Color hex case change rejected");
        Check(!ValidateTagPreservation("<TIME>秒", "<time>초").valid, "Engine tag case change rejected");
        Check(!ValidateTagPreservation("<TIME><TIME>", "<TIME>").valid, "Duplicate tag count mismatch rejected");
        Check(ValidateTagPreservation("A[X]%d", "%d[X]A").valid, "Reordered multiset equality accepted");
    }

    // 3. Context + Parameterized Template + Bounded Regex hierarchy
    {
        TranslationCore r1;
        std::string r1Err;
        const bool okDb = r1.LoadFromJsonBytes(R"JSON({
            "exact": {
                "癒し手アリサ": "치유사 아리사",
                "ロックする": "잠금 설정",
                "通常": "일반"
            },
            "exact_context": {
                "NameText.RealName": {
                    "癒し手アリサ": "아리사"
                }
            }
        })JSON", &r1Err);
        Check(okDb, "Load R1 exact + exact_context JSON");

        const bool okTpl = r1.LoadTemplatesFromJsonBytes(R"JSON({
            "templates": [
                {"id": "tpl_ato", "source_pattern": "(あと{int:days})", "target_template": "(남음{days})"},
                {"id": "tpl_dl", "source_pattern": "DL({int:a}/{int:b})", "target_template": "다운로드({a}/{b})"}
            ],
            "regex_rules": [
                {
                    "id": "rx_unlock",
                    "anchor_contains": "交換所にて入手",
                    "pattern": "^(.+?)を交換所にて入手$",
                    "replacement": "\\1을 교환소에서 획득"
                }
            ]
        })JSON", &r1Err);
        Check(okTpl, "Load templates + whitelisted regex JSON");

        auto dflt = r1.Translate("癒し手アリサ");
        auto ctxHit = r1.Translate("癒し手アリサ", "NameText.RealName");
        auto ctxMissFallback = r1.Translate("ロックする", "NameText.RealName");
        Check(dflt.outcome == TranslateOutcome::Hit && dflt.tier == MatchTier::Exact && dflt.text == "치유사 아리사",
              "Default exact hit without context");
        Check(ctxHit.outcome == TranslateOutcome::Hit && ctxHit.tier == MatchTier::ExactContext && ctxHit.text == "아리사",
              "Exact+Context overrides default when context provided");
        Check(ctxMissFallback.outcome == TranslateOutcome::Hit && ctxMissFallback.tier == MatchTier::Exact && ctxMissFallback.text == "잠금 설정",
              "Context miss falls back cleanly to global Exact");

        auto tplHit1 = r1.Translate("(あと45)");
        auto tplHit2 = r1.Translate("DL(12/100)");
        auto tplNonInt = r1.Translate("(あとabc)");
        Check(tplHit1.outcome == TranslateOutcome::Hit && tplHit1.tier == MatchTier::Template && tplHit1.text == "(남음45)",
              "Template single integer capture");
        Check(tplHit2.outcome == TranslateOutcome::Hit && tplHit2.tier == MatchTier::Template && tplHit2.text == "다운로드(12/100)",
              "Template dual integer capture");
        Check(tplNonInt.outcome == TranslateOutcome::Miss, "Template {int} rejects non-digit text");

        auto rxHit = r1.Translate("蒼穹の翼を交換所にて入手");
        Check(rxHit.outcome == TranslateOutcome::Hit && rxHit.tier == MatchTier::WhitelistedRegex &&
                  rxHit.text == "蒼穹の翼을 교환소에서 획득",
              "Anchored whitelisted regex capture replacement");

        // Reject broken template rule if tag count mismatches (synthetic check)
        TranslationCore badTplCore;
        badTplCore.LoadFromJsonBytes(R"({"exact":{}})");
        badTplCore.LoadTemplatesFromJsonBytes(R"({
            "templates": [{"id": "bad", "source_pattern": "X {int:n} %d", "target_template": "Y {n}"}]
        })");
        auto tplTagRej = badTplCore.Translate("X 5 %d");
        Check(tplTagRej.outcome == TranslateOutcome::TagMismatchFallback && tplTagRej.text == "X 5 %d",
              "Template dropping %d rejected via tag safety");

        // Whitespace-edge preserving fallback (exact priority preserved)
        TranslationCore wsCore;
        wsCore.LoadFromJsonBytes(R"({
            "exact": {
                "コスト": "코스트",
                "Я 戊稜": "팀 편성",
                "明示改行\n": "명시개행유지"
            }
        })");
        wsCore.LoadTemplatesFromJsonBytes(R"({
            "templates": [{"id": "t_rem", "source_pattern": "終了まで{int:h}時間", "target_template": "종료까지 {h}시간"}]
        })");
        Check(wsCore.Translate("コスト\n").text == "코스트\n", "Trailing LF preserved on fallback");
        Check(wsCore.Translate("コスト\r\n").text == "코스트\r\n", "Trailing CRLF preserved on fallback");
        Check(wsCore.Translate("\nコスト\n").text == "\n코스트\n", "Leading and trailing LF preserved");
        Check(wsCore.Translate("Я 戊稜      ").text == "팀 편성", "Trailing ASCII space padding stripped");
        Check(wsCore.Translate("明示改行\n").text == "명시개행유지", "Exact key with newline beats fallback");
        Check(wsCore.Translate("終了まで5時間\n").text == "종료까지 5시간\n", "Template match inside newline fallback");
        Check(wsCore.Translate("   \n  ").outcome == TranslateOutcome::Miss, "Whitespace-only string misses cleanly");
    }

    // 4. Coverage classification, redaction & disk flush
    {
        Check(ShouldIgnoreForCoverage(""), "Empty ignored");
        Check(ShouldIgnoreForCoverage("123/456"), "Digits/slash ignored");
        Check(ShouldIgnoreForCoverage("Lv.99 HP+100%"), "ASCII stat label ignored");
        Check(ShouldIgnoreForCoverage("지시를 내려주세요."), "Pure Korean string ignored (no Japanese)");
        Check(!ShouldIgnoreForCoverage("攻撃力"), "Kanji NOT ignored");
        Check(!ShouldIgnoreForCoverage("あと5日"), "Mixed Japanese NOT ignored");
        Check(!ShouldIgnoreForCoverage("アイテム"), "Katakana NOT ignored");

        const std::string red = RedactSensitiveTokens("url?token=SECRET12345&id=0123456789abcdef012345");
        Check(red == "url?token=<REDACTED>&id=<HEX>", "Sensitive token + hex redaction");
        Check(ComputeShapeKey("あと5720日で終了") == "あと{n}日で終了", "Shape key numeric grouping");

        CoverageTracker cov;
        const std::string testCovDir = "build32\\test_coverage_out";
        std::error_code ec;
        std::filesystem::remove_all(testCovDir, ec);
        cov.Configure(true, testCovDir);
        cov.SetCurrentScreen("Home");
        cov.Record("戻る", CoverageBucket::Exact, "", 0x1234);
        cov.Record("癒し手アリサ", CoverageBucket::Context, "NameText.RealName", 0x1234);
        cov.Record("(あと2)", CoverageBucket::Template, "tpl_ato_paren", 0x2000);
        cov.Record("未登録文字列99", CoverageBucket::Unmatched, "", 0x3000);
        cov.Record("12345", CoverageBucket::Ignored, "", 0x4000);
        cov.Record("壊れた<TIME>", CoverageBucket::TagRejected, "tag_multiset_mismatch", 0x5000);

        const auto sum = cov.SnapshotSummary();
        Check(sum.translatedCalls() == 3 && sum.translatableCalls() == 5 && sum.ignoredCalls == 1 && sum.tagRejectedCalls == 1,
              "CoverageTracker summary bucket arithmetic accurate");

        std::string jPath, tPath;
        const bool flushed = cov.FlushToDisk(&jPath, &tPath);
        Check(flushed && std::filesystem::exists(jPath) && std::filesystem::exists(tPath),
              "CoverageTracker FlushToDisk writes session JSON & unmatched TSV");
    }

    // 5. Real compiled R1 DB bulk load, sample check, and throughput benchmark
    {
        const std::wstring r1Path = L"data\\runtime\\translations_r1.json";
        const std::wstring tplPath = L"data\\runtime\\templates_r1.json";
        if (std::filesystem::exists(r1Path) && std::filesystem::exists(tplPath)) {
            TranslationCore prodCore;
            std::string dbErr;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok1 = prodCore.LoadFromJsonFile(r1Path, &dbErr);
            const bool ok2 = prodCore.LoadTemplatesFromJsonFile(tplPath, &dbErr);
            const auto tLoad = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            const auto st = prodCore.GetStats();
            std::printf("[BENCH] Loaded translations_r1.json (%zu exact, %zu ctx, %zu tpl, %zu rx) in %lld ms\n",
                        st.loadedEntries, st.loadedContextEntries, st.loadedTemplates, st.loadedRegexRules, tLoad);
            Check(ok1 && ok2 && st.loadedEntries >= 7500, "Compiled translations_r1.json loaded cleanly");

            Check(prodCore.Translate("亡国の王子").text == "망국의 왕자", "R1 ambiguity winner: 亡国の王子 -> 망국의 왕자");
            Check(prodCore.Translate("ロックする").text == "잠금 설정", "R1 UI revision winner: ロックする -> 잠금 설정");
            Check(prodCore.Translate("なし").text == "없음", "R1 reject 나시: なし -> 없음");
            Check(prodCore.Translate("(あと2)").text == "(남음2)", "R1 template live form (あと2) -> (남음2)");
            Check(prodCore.Translate("第4兵舎").text == "제4병사", "R1 template 第4兵舎 -> 제4병사");
            Check(prodCore.Translate("橡2糺筏").text == "제2병사", "R1 carrier template 橡2糺筏 -> 제2병사");
            Check(prodCore.Translate("★4 渥慇 漿宍 糠鮐禾").text == "★4 이하 유닛 보관소", "R1 carrier template ★4 unit storage");
            Check(prodCore.Translate("終了まで12時間").text == "종료까지 12시간", "R1 template 終了まで12時間");
            Check(prodCore.Translate("データ1で受取る").text == "데이터 1로 받기", "R1 template データ1で受取る");
            Check(prodCore.Translate("ランク： 999").text == "랭크: 999", "R1 template ランク： 999");
            Check(prodCore.Translate("Я 戊稜").text == "팀 편성", "R1 carrier alias Я 戊稜 -> 팀 편성");
            Check(prodCore.Translate("コスト\n").text == "코스트\n", "R1 whitespace edge コスト\\n -> 코스트\\n");

            const auto b0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10000; ++i) {
                prodCore.Translate("亡国の王子");
                prodCore.Translate("(あと42)");
                prodCore.Translate("未登録のベンチ文字列");
            }
            const auto bMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - b0).count();
            std::printf("[BENCH] 30,000 mixed lookups completed in %lld ms\n", bMs);
            Check(bMs < 1000, "30k mixed lookups under 1000 ms");
        }
    }

    // 6. R2 coverage taxonomy subbuckets, placeholders & server/core separation
    {
        CoverageTracker r2Cov;
        const std::string outDir = "build32\\test_coverage_r2";
        std::error_code ec;
        std::filesystem::remove_all(outDir, ec);
        r2Cov.Configure(true, outDir);
        Check(r2Cov.LoadClassificationRulesFromJsonFile(L"data\\curated\\internal_placeholders.json"),
              "R2 internal_placeholders.json loaded");
        Check(r2Cov.PlaceholderRuleCount() >= 16, "R2 placeholder rule count >= 16");

        Check(r2Cov.IsIgnoredText("あいうえおかきくけこ"), "R2 curated placeholder ignored");
        Check(r2Cov.ClassifyIgnored("あいうえおかきくけこ") == IgnoredSubbucket::InternalPlaceholder,
              "R2 curated placeholder subbucket");
        aigiskr::core::AutoTranslateConfig productionCfg;
        Check(aigiskr::core::EvaluateAutoTranslateCandidate("あいうえおかきくけこ",0,"UI",productionCfg,&r2Cov).accepted,
              "Coverage placeholder classification no longer suppresses production translation");
        Check(!r2Cov.IsIgnoredText("表示"), "R2 visible UI text not ignored");
        Check(r2Cov.ClassifyIgnored("12/34") == IgnoredSubbucket::Numeric, "R2 numeric subbucket");
        Check(r2Cov.ClassifyIgnored("Lv.99 MAX") == IgnoredSubbucket::AsciiCounter, "R2 ASCII counter subbucket");
        Check(r2Cov.ClassifyIgnored("지시를 내려주세요.") == IgnoredSubbucket::AlreadyKorean,
              "R2 already-Korean subbucket");

        Check(r2Cov.ClassifyUnmatched(0x283b73) == UnmatchedSubbucket::ServerDynamic,
              "R2 server notice RVA 0x283b73 -> ServerDynamic");
        Check(r2Cov.ClassifyUnmatched(0x63d61d) == UnmatchedSubbucket::AssetText,
              "R2 loading lore RVA 0x63d61d -> AssetText");
        Check(r2Cov.ClassifyUnmatched(0x57d40b) == UnmatchedSubbucket::StaticVisible,
              "R2 visible UI RVA -> StaticVisible");

        r2Cov.SetCurrentScreen("Notice");
        r2Cov.Record("未登録サーバー通知", CoverageBucket::Unmatched, "", 0x283b73);
        r2Cov.Record("13周年前夜祭\nキャンペーンのお知らせ", CoverageBucket::Exact, "", 0x283b73);
        r2Cov.SetCurrentScreen("Battle");
        r2Cov.Record("表示", CoverageBucket::Exact, "", 0x57d40b);
        r2Cov.Record("未翻訳ボタン", CoverageBucket::Unmatched, "", 0x57d40b);
        r2Cov.Record("未翻訳ローディング説明", CoverageBucket::Unmatched, "", 0x63d61d);
        r2Cov.Record("あいうえおかきくけこ", CoverageBucket::Ignored, "", 0x74dfb);

        const auto sum = r2Cov.SnapshotSummary();
        Check(sum.noticeTranslatedCalls == 1 && sum.unmatchedServerDynamicCalls == 1 &&
                  sum.noticeTranslatableCalls() == 2,
              "R2 server notice numerator/denominator separated cleanly");
        Check(sum.coreStaticTranslatedCalls() == 1 && sum.unmatchedStaticVisibleCalls == 1 &&
                  sum.unmatchedAssetTextCalls == 1 && sum.coreStaticTranslatableCalls() == 3 &&
                  sum.coreStaticUiOnlyTranslatableCalls() == 2,
              "R2 CoreStatic & UI-only denominators separated cleanly");
        Check(sum.ignoredInternalPlaceholderCalls == 1, "R2 IgnoredInternalPlaceholder counter incremented");
    }

    // 7. R2 corpus, notice overlay merge, context disambiguation & R2 regex rules
    {
        const std::wstring r2Path = L"data\\runtime\\translations_r2.json";
        const std::wstring r2TplPath = L"data\\runtime\\templates_r2.json";
        const std::wstring noticePath = L"data\\notices\\runtime_delta\\notices_r2.json";
        if (std::filesystem::exists(r2Path) && std::filesystem::exists(r2TplPath) &&
            std::filesystem::exists(noticePath)) {
            TranslationCore r2Core;
            std::string err;
            const bool okMain = r2Core.LoadFromJsonFile(r2Path, &err);
            const bool okTpl = r2Core.LoadTemplatesFromJsonFile(r2TplPath, &err);
            const bool okNotice = r2Core.LoadOverlayFromJsonFile(noticePath, &err);
            const auto st = r2Core.GetStats();
            Check(okMain && okTpl && okNotice, "R2 main + templates + notices overlay loaded cleanly");
            Check(st.loadedEntries >= 8200 && st.loadedTemplates >= 29 && st.loadedRegexRules >= 3,
                  "R2 DB & rule counts match expected baseline");

            const auto uiHit = r2Core.Translate("表示");
            Check(uiHit.outcome == TranslateOutcome::Hit && uiHit.text == "표시", "R2 static UI exact hit: 表示");

            const auto starHit = r2Core.Translate("このミッションで\nはじめての★３つ獲得！\n『神聖結晶』を獲得");
            Check(starHit.outcome == TranslateOutcome::Hit &&
                      starHit.text == "이 미션에서\n처음으로 ★3 획득!\n『신성결정』을 획득",
                  "R2 first-time 3-star popup exact hit");

            const auto nExact = r2Core.Translate("SPブラックとレジェンド召喚\nチケットについて");
            Check(nExact.outcome == TranslateOutcome::Hit &&
                      nExact.text == "SP 블랙 및 레전드 소환\n티켓에 대하여",
                  "R2 notice overlay exact hit");

            const auto nTpl = r2Core.Translate("14周年前夜祭\nキャンペーンのお知らせ");
            Check(nTpl.outcome == TranslateOutcome::Hit && nTpl.tier == MatchTier::Template &&
                      nTpl.text == "14주년 전야제\n캠페인 안내",
                  "R2 notice overlay template hit");

            const auto ctxReal = r2Core.Translate("万刃を担う者アルヴァ", "NameText.RealName");
            Check(ctxReal.outcome == TranslateOutcome::Hit && ctxReal.tier == MatchTier::ExactContext &&
                      ctxReal.text == "만인을 짊어진 자 알바",
                  "R2 context disambiguation NameText.RealName");

            const auto ctxStatus = r2Core.Translate("出撃コスト", "UnitStatus");
            Check(ctxStatus.outcome == TranslateOutcome::Hit && ctxStatus.tier == MatchTier::ExactContext &&
                      ctxStatus.text == "출격코스트",
                  "R2 context disambiguation UnitStatus");

            const auto rxExchange = r2Core.Translate("蒼穹の翼を\n交換所にて入手すると解放");
            Check(rxExchange.outcome == TranslateOutcome::Hit && rxExchange.tier == MatchTier::WhitelistedRegex &&
                      rxExchange.text == "蒼穹の翼을\n교환소에서 획득 시 해금",
                  "R2 regex rule rx_exchange_unlock_title");

            const auto rxRank = r2Core.Translate("プレイヤーランクが\n54になると解放");
            Check(rxRank.outcome == TranslateOutcome::Hit && rxRank.tier == MatchTier::WhitelistedRegex &&
                      rxRank.text == "플레이어 랭크\n54 달성 시 해금",
                  "R2 regex rule rx_rank_unlock_title");

            const auto rxGrand = r2Core.Translate("大総力戦ミッション\n「神獣フンババ降臨　極級」を\nクリアで解放");
            Check(rxGrand.outcome == TranslateOutcome::Hit && rxGrand.tier == MatchTier::WhitelistedRegex &&
                      rxGrand.text == "대총력전 미션\n「神獣フンババ降臨　極級」\n클리어 시 해금",
                  "R2 regex rule rx_grand_subjugation_unlock");

            // R3 preflight runtime-JP blockers (TeamCopy + Wangto/Delegate sortie)
            const auto copySrc = r2Core.Translate("%c[f2b719]コピー元");
            Check(copySrc.outcome == TranslateOutcome::Hit && copySrc.text == "%c[f2b719]복사 원본",
                  "R3 TeamCopy source badge translation");

            const auto copyConfirm = r2Core.Translate("『Я1       』\nをコピー元に設定しますか？");
            Check(copyConfirm.outcome == TranslateOutcome::Hit && copyConfirm.tier == MatchTier::Template &&
                      copyConfirm.text == "『Я1       』을(를)\n복사 원본으로 설정하시겠습니까?",
                  "R3 TeamCopy confirm modal template translation");

            const auto copyDone = r2Core.Translate("『팀A』\nをコピー元に設定しました。");
            Check(copyDone.outcome == TranslateOutcome::Hit && copyDone.tier == MatchTier::Template &&
                      copyDone.text == "『팀A』을(를)\n복사 원본으로 설정했습니다.",
                  "R3 TeamCopy confirmed modal template translation");

            const auto copySame = r2Core.Translate("コピー元と同じチームに対して\n上書きすることはできません。");
            Check(copySame.outcome == TranslateOutcome::Hit &&
                      copySame.text == "복사 원본과 같은 팀에는\n덮어쓸 수 없습니다.",
                  "R3 TeamCopy self-overwrite warning translation");

            const auto starGot = r2Core.Translate("★３報酬：神聖結晶（入手済）");
            Check(starGot.outcome == TranslateOutcome::Hit &&
                      starGot.text == "★3 보상: 신성결정(획득 완료)",
                  "R3 Wangto 3-star acquired reward label translation");

            const auto wangtoDesc = r2Core.Translate("美しき田畑に囲まれた池に棲む\n醜き泥の魔物とは！");
            Check(wangtoDesc.outcome == TranslateOutcome::Hit &&
                      wangtoDesc.text == "아름다운 전답에 둘러싸인 연못에 사는\n추악한 진흙 마물이라니!",
                  "R3 Wangto Ch1 description translation");

            const auto delPrompt = r2Core.Translate("委任出撃を行う回数を選択してください");
            Check(delPrompt.outcome == TranslateOutcome::Hit &&
                      delPrompt.text == "위임 출격을 진행할 횟수를 선택해 주세요",
                  "R3 Delegate sortie count prompt translation");

            const auto del1 = r2Core.Translate("1回");
            const auto del2 = r2Core.Translate("2回");
            const auto del3 = r2Core.Translate("3回");
            Check(del1.outcome == TranslateOutcome::Hit && del1.text == "1회" &&
                      del2.outcome == TranslateOutcome::Hit && del2.text == "2회" &&
                      del3.outcome == TranslateOutcome::Hit && del3.text == "3회",
                  "R3 Delegate sortie 1/2/3 count labels");
        }
    }

    // 8. Dual Healthcheck (Executable Safety + R2 Texture Profile)
    {
        const std::string hcDir = "build32\\test_healthcheck";
        std::error_code ec;
        std::filesystem::remove_all(hcDir, ec);
        std::filesystem::create_directories(hcDir, ec);

        HealthcheckReport rep;
        rep.fileSize = aigiskr::diagnostics::kKnownExeSize;
        rep.sha256 = aigiskr::diagnostics::kKnownExeSha256;
        rep.signatures.push_back(SignatureCheckRecord{"ALTextDraw_slot28", true, 1, 1, 0x6e3b30, true, "OK"});
        Check(EvaluateBuildSafety(rep, true, true) == BuildSafetyState::Known,
              "Dual Healthcheck Axis 1 KNOWN on matching SHA256 & signatures");

        Check(EvaluateTextureHealth(rep, hcDir + "\\missing.json", "", false) ==
                  TextureProfileState::ManifestMissing,
              "Dual Healthcheck Axis 2 MANIFEST_MISSING when manifest absent");

        const std::string manifestPath = hcDir + "\\texture_manifest.json";
        {
            // Exercise health-state transitions independently of the number of
            // translated textures in the current release manifest.
            std::ofstream fixture(manifestPath, std::ios::binary | std::ios::trunc);
            fixture << R"({"schema_version":2,"roles":[{"role":"menu_home","status":"CONFIRMED","all_hashes":["0000000000000001","0000000000000002"]},{"role":"status_window","status":"CONFIRMED","all_hashes":["0000000000000002"]}]})";
        }
        {
            Check(EvaluateTextureHealth(rep, manifestPath, hcDir + "\\no_status.json", false) ==
                      TextureProfileState::AwaitingTextures &&
                      rep.texture.manifestRolesTotal == 2 && rep.texture.manifestConfirmedRoles == 2 &&
                      rep.texture.manifestUniqueHashes == 2,
                  "Dual Healthcheck Axis 2 AWAITING_TEXTURES before runtime status");

            auto writeJson = [](const std::string& p, const std::string& body) {
                std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
                ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            };

            const std::string stVanilla = hcDir + "\\tt_vanilla.json";
            writeJson(stVanilla,
                      R"({"manifest_loaded":true,"profile":"VANILLA_KNOWN","injected_files_count":12,"injected_active_count":7,"failed_injections_count":0,"skipped_uploads":10,"matched_vanilla_roles":["menu_home"],"matched_legacy_roles":[],"matched_shared_roles":["status_window"],"failed_roles":[],"unknown_watched_hashes":[]})");
            Check(EvaluateTextureHealth(rep, manifestPath, stVanilla, false) ==
                          TextureProfileState::VanillaKnown &&
                      rep.texture.injectedActiveCount == 7 && rep.texture.matchedVanillaRoles.size() == 1,
                  "Dual Healthcheck Axis 2 VANILLA_KNOWN");

            const std::string stLegacy = hcDir + "\\tt_legacy.json";
            writeJson(stLegacy,
                      R"({"manifest_loaded":true,"profile":"LEGACY_KNOWN","injected_files_count":12,"injected_active_count":5,"failed_injections_count":0,"skipped_uploads":8,"matched_vanilla_roles":[],"matched_legacy_roles":["menu_home"],"matched_shared_roles":[],"failed_roles":[],"unknown_watched_hashes":[]})");
            Check(EvaluateTextureHealth(rep, manifestPath, stLegacy, false) ==
                      TextureProfileState::LegacyKnown,
                  "Dual Healthcheck Axis 2 LEGACY_KNOWN");

            const std::string stMixed = hcDir + "\\tt_mixed.json";
            writeJson(stMixed,
                      R"({"manifest_loaded":true,"profile":"MIXED_OR_PARTIAL_UNKNOWN","injected_files_count":12,"injected_active_count":2,"failed_injections_count":1,"skipped_uploads":4,"matched_vanilla_roles":[],"matched_legacy_roles":[],"matched_shared_roles":[],"failed_roles":["team_menu"],"unknown_watched_hashes":["DEADBEEF@2048x104"]})");
            Check(EvaluateTextureHealth(rep, manifestPath, stMixed, false) ==
                      TextureProfileState::PartialUnknown,
                  "Dual Healthcheck Axis 2 MIXED_OR_PARTIAL_UNKNOWN when failed_roles non-empty");

            const std::string outHc = hcDir + "\\healthcheck_test.json";
            Check(WriteHealthcheckJson(rep, outHc) && std::filesystem::exists(outHc),
                  "WriteHealthcheckJson persists combined Dual Healthcheck JSON");
        }
    }

    // 9. R2.1 Release Configuration Hygiene (EnableTextHooks, LogTextEvents, EnableCoverage defaults & legacy fallback)
    {
        const auto cfgDir = std::filesystem::absolute("build32\\test_config");
        std::error_code ec;
        std::filesystem::remove_all(cfgDir, ec);
        std::filesystem::create_directories(cfgDir, ec);
        const std::wstring gameDirW = cfgDir.wstring() + L"\\";

        auto writeIni = [&](const std::string& name, const std::string& body) -> std::wstring {
            const auto p = cfgDir / name;
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            return p.wstring();
        };

        const auto pDefault = writeIni("release_omitted.ini",
                                       "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\n");
        const auto cDefault = aigiskr::core::LoadProbeConfig(pDefault, gameDirW);
        Check(cDefault.enableHooks && cDefault.enableTextHooks && !cDefault.logTextEvents &&
                  !cDefault.logALTextProp && !cDefault.logALFontW3 && !cDefault.enableCoverage,
              "R2.1 omitted keys default to EnableTextHooks=1, LogTextEvents=0, EnableCoverage=0");

        const auto pExplicit = writeIni("release_explicit.ini",
                                        "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\nEnableTextHooks=1\r\nLogTextEvents=0\r\nLogALTextDraw=1\r\n");
        const auto cExplicit = aigiskr::core::LoadProbeConfig(pExplicit, gameDirW);
        Check(cExplicit.enableTextHooks && !cExplicit.logTextEvents && !cExplicit.enableCoverage,
              "R2.1 explicit EnableTextHooks=1 + LogTextEvents=0 overrides legacy LogALTextDraw");

        const auto pHooksOff = writeIni("hooks_off.ini",
                                        "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\nEnableTextHooks=0\r\nLogTextEvents=0\r\n");
        const auto cHooksOff = aigiskr::core::LoadProbeConfig(pHooksOff, gameDirW);
        Check(!cHooksOff.enableTextHooks && !cHooksOff.logTextEvents,
              "R2.1 EnableTextHooks=0 disarms text hooks while keeping process up");

        const auto pLegacyOff = writeIni("legacy_off.ini",
                                         "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\nLogALTextDraw=0\r\n");
        const auto cLegacyOff = aigiskr::core::LoadProbeConfig(pLegacyOff, gameDirW);
        Check(!cLegacyOff.enableTextHooks && !cLegacyOff.logTextEvents,
              "Legacy LogALTextDraw=0 fallback disarms text hooks");

        const auto pLegacyOn = writeIni("legacy_on.ini",
                                        "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\nLogALTextDraw=1\r\n[Coverage]\r\nEnableCoverage=1\r\n");
        const auto cLegacyOn = aigiskr::core::LoadProbeConfig(pLegacyOn, gameDirW);
        Check(cLegacyOn.enableTextHooks && cLegacyOn.logTextEvents && cLegacyOn.enableCoverage,
              "Legacy LogALTextDraw=1 + explicit EnableCoverage=1 remain supported");

        // R3: missing AigisKR.ini -> release-safe defaults + pluginDir-relative AigisKR/data/*
        const auto pluginSubDir = cfgDir / "scripts";
        const auto pluginDataDir = pluginSubDir / "AigisKR" / "data";
        std::filesystem::create_directories(pluginDataDir, ec);
        {
            std::ofstream ofs(pluginDataDir / "translations.json", std::ios::binary | std::ios::trunc);
            ofs << "{}";
        }
        const std::wstring pluginDirW = pluginSubDir.wstring() + L"\\";
        const std::wstring missingIni = pluginDirW + L"AigisKR.ini";
        const auto cLegacyModePath = pluginSubDir / "legacy_mode.ini";
        { std::ofstream ini(cLegacyModePath); ini << "[Probe]\nMode=Phase3\nEnableHooks=1\n[Phase3]\nReplaceAscii=ATK_TEST\n"; }
        const auto cLegacy = aigiskr::core::LoadProbeConfig(cLegacyModePath.wstring(),gameDirW,pluginDirW);
        Check(cLegacy.enableHooks && cLegacy.enableTranslation && cLegacy.enableHangulFont,
              "Legacy PoC mode cannot disable production translation or Hangul font");
        const auto cMissing = aigiskr::core::LoadProbeConfig(missingIni, gameDirW, pluginDirW);
        Check(cMissing.enableHooks &&
                  cMissing.enableTextHooks && cMissing.enableTranslation && cMissing.enableHangulFont &&
                  !cMissing.logTextEvents && !cMissing.enableCoverage && !cMissing.enableVirtualClick &&
                  !cMissing.captureBackbuffer,
              "R3 missing AigisKR.ini applies release-safe defaults");
        Check(cMissing.translationsDbPath == (pluginDataDir / "translations.json").wstring() &&
                  cMissing.templatesDbPath == (pluginDataDir / "templates.json").wstring() &&
                  cMissing.noticeOverlayPath == (pluginDataDir / "notices.json").wstring() &&
                  cMissing.placeholdersPath.empty(),
              "R3 missing AigisKR.ini resolves runtime DBs under scripts/AigisKR/data without dev paths");

        const auto cSynthetic = aigiskr::core::LoadProbeConfig(
            L"C:\\CleanGame\\scripts\\AigisKR.ini",
            L"C:\\CleanGame\\",
            L"C:\\CleanGame\\scripts\\");
        Check(cSynthetic.sampleDictPath.empty() &&
                  cSynthetic.placeholdersPath.empty() &&
                  cSynthetic.translationsDbPath == L"C:\\CleanGame\\scripts\\AigisKR\\data\\translations.json" &&
                  cSynthetic.coverageDir == L"C:\\CleanGame\\scripts\\AigisKR\\coverage" &&
                  cSynthetic.translationsDbPath.find(L"aigiskr-native") == std::wstring::npos &&
                  cSynthetic.coverageDir.find(L"aigiskr-native") == std::wstring::npos &&
                  !cSynthetic.autoTranslate.enabled &&
                  cSynthetic.autoTranslate.hdTransPath.empty() &&
                  cSynthetic.autoTranslate.cacheFile == L"C:\\CleanGame\\scripts\\AigisKR\\cache\\hdtrans_cache.jsonl" &&
                  cSynthetic.autoTranslate.knowledgeDir == L"C:\\CleanGame\\scripts\\AigisKR\\data\\knowledge" &&
                  !cSynthetic.autoTranslate.retranslateStale,
              "R3 runtime config contains zero aigiskr-native/local hdTrans default paths");

        const auto pAutoIni = writeIni(
            "auto_on.ini",
            "[Probe]\r\nMode=Phase4\r\nEnableHooks=1\r\n"
            "[Phase5]\r\nEnableExtendedGlyphFallback=0\r\n"
            "[AutoTranslate]\r\nEnabled=1\r\nProvider=hdTrans\r\nHdTransPath=C:\\MyHdTrans\r\n"
            "KnowledgeDir=AigisKR\\custom_kl\r\nRetranslateStale=1\r\n"
            "CacheEnabled=1\r\nTranslateUI=1\r\nTranslateStory=0\r\nMinLength=2\r\nMaxLength=512\r\nLogAutoTranslate=1\r\n");
        const auto cAuto = aigiskr::core::LoadProbeConfig(pAutoIni, gameDirW, pluginDirW);
        Check(!cAuto.enableExtendedGlyphFallback &&
                  cAuto.autoTranslate.enabled &&
                  cAuto.autoTranslate.provider == "hdTrans" &&
                  cAuto.autoTranslate.hdTransPath == L"C:\\MyHdTrans" &&
                  cAuto.autoTranslate.knowledgeDir == (pluginSubDir / "AigisKR" / "custom_kl").wstring() &&
                  cAuto.autoTranslate.retranslateStale &&
                  cAuto.autoTranslate.cacheEnabled &&
                  cAuto.autoTranslate.translateUI &&
                  !cAuto.autoTranslate.translateStory &&
                  cAuto.autoTranslate.minLength == 2 &&
                  cAuto.autoTranslate.maxLength == 512 &&
                  cAuto.autoTranslate.logAutoTranslate,
              "R3.2 AutoTranslate and Phase5 INI keys parse cleanly");
    }

    // R3.2 Tests: Tag masking/restoration, candidate filter, AutoTranslate queue/precedence/cache resilience
    {
        const std::string tagSrc = "%c[ff0000]攻撃力%c[ffffff]が<TIME>の間[ATK]上昇 100%%";
        const auto masked = aigiskr::core::MaskProtectedTags(tagSrc);
        Check(masked.tokens.size() == 5 &&
                  masked.masked == "__AIGIS_TAG_0000__攻撃力__AIGIS_TAG_0001__が__AIGIS_TAG_0002__の間__AIGIS_TAG_0003__上昇 100__AIGIS_TAG_0004__",
              "R3.2 MaskProtectedTags emits indexed sentinels");

        std::string restored;
        std::string reason;
        const std::string reordered = "__AIGIS_TAG_0002__ 동안 __AIGIS_TAG_0000__공격력__AIGIS_TAG_0001__이 __AIGIS_TAG_0003__ 상승 100__AIGIS_TAG_0004__";
        Check(aigiskr::core::RestoreProtectedTags(reordered, masked.tokens, restored, &reason) &&
                  restored == "<TIME> 동안 %c[ff0000]공격력%c[ffffff]이 [ATK] 상승 100%%" &&
                  aigiskr::core::ValidateTagPreservation(tagSrc, restored).valid,
              "R3.2 RestoreProtectedTags handles reordered sentinels safely");

        Check(!aigiskr::core::RestoreProtectedTags("__AIGIS_TAG_0000__공격력", masked.tokens, restored, &reason),
              "R3.2 RestoreProtectedTags rejects missing sentinels");
        Check(!aigiskr::core::RestoreProtectedTags("__AIGIS_TAG_0000____AIGIS_TAG_0000____AIGIS_TAG_0001____AIGIS_TAG_0002____AIGIS_TAG_0003____AIGIS_TAG_0004__",
                                                  masked.tokens, restored, &reason),
              "R3.2 RestoreProtectedTags rejects duplicate sentinels");
        Check(!aigiskr::core::RestoreProtectedTags("__AIGIS_TAG_0000__test", {}, restored, &reason),
              "R3.2 RestoreProtectedTags rejects hallucinated sentinel for untagged source");
    }

    {
        aigiskr::core::AutoTranslateConfig acfg;
        acfg.enabled = true;
        acfg.minLength = 1;
        std::string longSource;
        for (int i=0;i<2000;++i) longSource += "あ";
        Check(aigiskr::core::EvaluateAutoTranslateCandidate(longSource,0,"UI",acfg).accepted,
              "Production defaults do not impose the old 4096-byte candidate cutoff");
        Check(aigiskr::core::EvaluateAutoTranslateCandidate("기사 攻撃力",0,"UI",acfg).accepted,
              "Mixed Korean and Japanese Kanji text remains translatable");
        acfg.maxLength = 200;
        acfg.translateUI = true;
        acfg.translateStory = false;

        Check(aigiskr::core::EvaluateAutoTranslateCandidate("戦神の加護", 0x1234u, "WarGodBlessing", acfg).accepted,
              "R3.2 Candidate filter accepts Japanese UI string");
        Check(aigiskr::core::EvaluateAutoTranslateCandidate("剣", 0x1234u, "UnitStatus", acfg).accepted,
              "R3.2 Candidate filter accepts single Kanji word when MinLength=1");
        Check(aigiskr::core::EvaluateAutoTranslateCandidate("%c[ff0000]攻撃力%c[ffffff]上昇", 0x1234u, "Skill", acfg).accepted,
              "R3.2 Candidate filter accepts tagged Japanese string");
        Check(!aigiskr::core::EvaluateAutoTranslateCandidate("전신의 가호", 0x1234u, "WarGodBlessing", acfg).accepted,
              "R3.2 Candidate filter rejects already-Korean string");
        Check(!aigiskr::core::EvaluateAutoTranslateCandidate("HP 100/200 +50%", 0x1234u, "UnitStatus", acfg).accepted,
              "R3.2 Candidate filter rejects ASCII/numeric strings");
        Check(!aigiskr::core::EvaluateAutoTranslateCandidate("%c[ff0000]<TIME>[ATK]", 0x1234u, "UnitStatus", acfg).accepted,
              "R3.2 Candidate filter rejects tag-only strings without Japanese glyphs");
        Check(!aigiskr::core::EvaluateAutoTranslateCandidate("https://example.com?token=abc", 0x1234u, "Notice", acfg).accepted,
              "R3.2 Candidate filter rejects URLs/session tokens");
        Check(!aigiskr::core::EvaluateAutoTranslateCandidate("テストセリフ", 0xAD6C0u, "Story", acfg).accepted,
              "R3.2 Candidate filter enforces TranslateStory=0");
    }

    // Fake backend testing precedence, async dedup, tag rejection, persistence & corrupted JSONL recovery.
    {
        struct FakeBackend final : public aigiskr::core::IAutoTranslateBackend {
            std::atomic<int> initCount{0};
            std::atomic<int> transCount{0};
            std::atomic<int> shutCount{0};
            std::thread::id workerTid{};
            std::mutex gateMu;
            std::condition_variable gateCv;
            bool paused = false;

            void SetPaused(bool p) {
                {
                    std::lock_guard<std::mutex> lk(gateMu);
                    paused = p;
                }
                gateCv.notify_all();
            }

            bool ProbeAvailability(const std::wstring& rootPath, std::string* detailOut) const override {
                if (rootPath.find(L"MISSING") != std::wstring::npos) {
                    if (detailOut) *detailOut = "missing fake path";
                    return false;
                }
                return true;
            }
            bool Initialize(const std::wstring&, std::string* detailOut) override {
                std::unique_lock<std::mutex> lk(gateMu);
                gateCv.wait(lk, [this]() { return !paused; });
                workerTid = std::this_thread::get_id();
                ++initCount;
                if (detailOut) *detailOut = "fake-ready";
                return true;
            }
            bool TranslateUtf8(std::string_view inputUtf8,
                               std::string& outTranslatedUtf8,
                               std::string* errorOut) override {
                {
                    std::unique_lock<std::mutex> lk(gateMu);
                    gateCv.wait(lk, [this]() { return !paused; });
                }
                ++transCount;
                if (inputUtf8.find("BADTAG") != std::string_view::npos) {
                    // Simulate backend corrupting/dropping sentinel
                    outTranslatedUtf8 = "깨진 태그";
                    return true;
                }
                if (inputUtf8.find("HALLUCINATE") != std::string_view::npos) {
                    outTranslatedUtf8 = "이상한 <BAD> 결과";
                    return true;
                }
                if (inputUtf8 == "非同期テスト") {
                    outTranslatedUtf8 = "비동기 테스트";
                    return true;
                }
                if (inputUtf8.find("の攻撃力") != std::string_view::npos) {
                    // Replace JP portion while keeping sentinels intact
                    outTranslatedUtf8 = std::string(inputUtf8);
                    const auto p = outTranslatedUtf8.find("の攻撃力");
                    outTranslatedUtf8.replace(p, std::string_view("の攻撃力").size(), "의 공격력");
                    return true;
                }
                if (errorOut) *errorOut = "unhandled";
                return false;
            }
            void Shutdown(bool) override { ++shutCount; }
            bool IsInitialized() const noexcept override { return initCount.load() > 0; }
            std::string EngineVersion() const override { return "FakeEngine-1.0"; }
        };

        std::error_code ec;
        const auto testDir = std::filesystem::temp_directory_path() / "aigiskr_r32_auto_tests";
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);

        const auto seedPath = testDir / "seed.jsonl";
        const auto cachePath = testDir / "cache.jsonl";
        const auto overridePath = testDir / "overrides.json";
        {
            std::ofstream ofs(seedPath, std::ios::binary);
            ofs << "{\"src\":\"シード語\",\"dst\":\"시드 번역\",\"provider\":\"hdTrans\"}\n";
            ofs << "{\"src\":\"ご指示をお願いします。\",\"dst\":\"잘못된 자동번역\",\"provider\":\"hdTrans\"}\n";
            ofs << "{\"src\":\"達成率:31%\",\"dst\":\"자동번역 오염\",\"provider\":\"hdTrans\"}\n";
        }
        {
            std::ofstream ofs(overridePath, std::ios::binary);
            ofs << "{\"exact\":{\"ご指示をお願いします。\":\"수동 최우선 오버라이드\"}}\n";
        }
        {
            std::ofstream ofs(cachePath, std::ios::binary);
            ofs << "{\"src\":\"既存キャッシュ\",\"dst\":\"기존 캐시 번역\"}\n";
            ofs << "{\"src\":\"<BROKEN>タグ\",\"dst\":\"태그 손상\"}\n";  // tag mismatch on load
            ofs << "{\"src\":\"broken_truncated_json";                    // interrupted write: no final newline
        }

        aigiskr::core::AutoTranslateConfig acfg;
        acfg.enabled = true;
        acfg.hdTransPath = L"MOCK_OK";
        acfg.cacheEnabled = true;
        acfg.seedCacheFile = seedPath.wstring();
        acfg.cacheFile = cachePath.wstring();
        acfg.overridesFile = overridePath.wstring();

        aigiskr::core::AutoTranslateManager mgr;
        auto fake = std::make_unique<FakeBackend>();
        FakeBackend* fakePtr = fake.get();
        mgr.SetBackendForTesting(std::move(fake));
        Check(mgr.Initialize(acfg), "R3.2 AutoTranslateManager Initialize with corrupt/seed files");

        const auto stLoad = mgr.GetStats();
        Check(stLoad.malformedCacheLines == 1 && stLoad.rejectedCacheLines == 1 &&
                  stLoad.overridesLoaded == 1 && stLoad.currentCacheEntries == 4,
              "R3.2 AutoTranslateManager tolerates corrupt JSONL lines and skips bad-tag cache rows");

        // Precedence: ManualOverride > Curated > AutoCache
        std::string out;
        Check(mgr.LookupOverride("ご指示をお願いします。", "", out) && out == "수동 최우선 오버라이드",
              "R3.2 ManualOverride beats Curated and AutoCache");
        const auto curHit = core.Translate("達成率:31%");
        Check(curHit.outcome == aigiskr::core::TranslateOutcome::Hit && curHit.text == "달성률: 31%",
              "R3.2 Curated DB hit remains ahead of AutoCache fallback");
        Check(mgr.LookupCacheOrQueue("シード語", 0x100u, "UI", out) == aigiskr::core::AutoLookupOutcome::CacheHit &&
                  out == "시드 번역",
              "R3.2 Seed cache hit serves immediately without queueing");

        // Async queue & deduplication & zero synchronous translation on render thread
        fakePtr->SetPaused(true);
        const int transBeforeQueue = fakePtr->transCount.load();
        Check(mgr.LookupCacheOrQueue("非同期テスト", 0x120u, "WarGodBlessing", out) ==
                      aigiskr::core::AutoLookupOutcome::Queued &&
                  fakePtr->transCount.load() == transBeforeQueue,
              "R3.2 First uncached lookup enqueues async job with zero sync backend calls");
        Check(mgr.LookupCacheOrQueue("非同期テスト", 0x120u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::AlreadyPending,
              "R3.2 Duplicate uncached lookup deduplicates pending key");

        // Tagged async translation + tag-mismatch rejection tests
        Check(mgr.CompletionRevision() == 0 && !mgr.LookupCompleted("非同期テスト", out) &&
              mgr.GetStats().jobsQueued == 1,
              "Live completion lookup is read-only while the backend is blocked");
        Check(mgr.LookupCacheOrQueue("%c[ff0000]味方%c[ffffff]の攻撃力", 0x130u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::Queued,
              "R3.2 Queues tagged string");
        Check(mgr.LookupCacheOrQueue("<TIME>BADTAGあいう", 0x140u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::Queued,
              "R3.2 Queues BADTAG string to verify sentinel rejection");
        Check(mgr.LookupCacheOrQueue("HALLUCINATEテスト", 0x150u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::Queued,
              "R3.2 Queues HALLUCINATE string to verify stray tag rejection");

        fakePtr->SetPaused(false);
        Check(mgr.WaitForIdleForTesting(5000), "R3.2 Background worker drains queued jobs");
        Check(mgr.CompletionRevision() == 2 && mgr.LookupCompleted("非同期テスト", out) && out == "비동기 테스트" &&
              !mgr.LookupCompleted("<TIME>BADTAGあいう", out),
              "Live completion publishes accepted results only, never tag-rejected output");

        Check(mgr.LookupCacheOrQueue("非同期テスト", 0x120u, "WarGodBlessing", out) ==
                      aigiskr::core::AutoLookupOutcome::CacheHit &&
                  out == "비동기 테스트",
              "R3.2 Subsequent lookup returns newly cached async translation");
        Check(mgr.LookupCacheOrQueue("%c[ff0000]味方%c[ffffff]の攻撃力", 0x130u, "WarGodBlessing", out) ==
                      aigiskr::core::AutoLookupOutcome::CacheHit &&
                  out == "%c[ff0000]味方%c[ffffff]의 공격력",
              "R3.2 Tagged async result restores protected tags accurately");
        Check(mgr.LookupCacheOrQueue("<TIME>BADTAGあいう", 0x140u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::PreviouslyFailed,
              "R3.2 Sentinel-corrupt output is rejected, uncached, and marked failed");
        Check(mgr.LookupCacheOrQueue("HALLUCINATEテスト", 0x150u, "WarGodBlessing", out) ==
                  aigiskr::core::AutoLookupOutcome::PreviouslyFailed,
              "R3.2 Hallucinated tag output is rejected and uncached");

        const auto stDone = mgr.GetStats();
        Check(stDone.jobsCompleted == 2 && stDone.tagRejected == 2 &&
                  stDone.jobsDeduplicated == 1 && stDone.renderThreadBackendCalls == 0,
              "R3.2 AutoTranslate stats record exact completed/rejected/dedup counts");

        mgr.Shutdown(true);

        // Restart persistence check: fresh AutoTranslateManager with MISSING backend still serves written cache!
        aigiskr::core::AutoTranslateConfig restartCfg = acfg;
        restartCfg.hdTransPath = L"C:\\MISSING\\HDTRANS";
        aigiskr::core::AutoTranslateManager mgrRestart;
        Check(mgrRestart.Initialize(restartCfg),
              "R3.2 Restart with missing HdTransPath initializes gracefully without crash");
        Check(!mgrRestart.GetStats().backendAvailable &&
                  mgrRestart.LookupCacheOrQueue("非同期テスト", 0x120u, "WarGodBlessing", out) ==
                      aigiskr::core::AutoLookupOutcome::CacheHit &&
                  out == "비동기 테스트",
              "R3.2 Restart reloads persisted JSONL cache even when hdTrans runtime is missing");
        Check(mgrRestart.LookupCacheOrQueue("未翻訳の新しい文字列", 0x160u, "UI", out) ==
                  aigiskr::core::AutoLookupOutcome::BackendUnavailable,
              "R3.2 Uncached string falls back cleanly when hdTrans runtime is unavailable");
        mgrRestart.Shutdown(true);

        // A directory at the cache filename forces append failure without ACL changes.
        const auto blockedCache = testDir / "cache_is_a_directory";
        std::filesystem::create_directories(blockedCache, ec);
        auto writeFailureCfg = acfg;
        writeFailureCfg.cacheFile = blockedCache.wstring();
        aigiskr::core::AutoTranslateManager writeFailureMgr;
        writeFailureMgr.SetBackendForTesting(std::make_unique<FakeBackend>());
        Check(writeFailureMgr.Initialize(writeFailureCfg), "Astra Cache write failure initializes safely");
        writeFailureMgr.LookupCacheOrQueue("非同期テスト", 0x120u, "UI", out);
        Check(writeFailureMgr.WaitForIdleForTesting(5000) &&
                  writeFailureMgr.LookupCacheOrQueue("非同期テスト", 0x120u, "UI", out) ==
                      aigiskr::core::AutoLookupOutcome::CacheHit && out == "비동기 테스트",
              "Astra Cache append failure preserves successful in-memory translation");
        writeFailureMgr.Shutdown(true);

        // Optional local D:\hdTrans real J2KEngine.dll integration smoke (only when present on test host)
        if (GetFileAttributesW(L"D:\\hdTrans\\J2KEngine.dll") != INVALID_FILE_ATTRIBUTES) {
            aigiskr::core::AutoTranslateConfig liveCfg;
            liveCfg.enabled = true;
            liveCfg.hdTransPath = L"D:\\hdTrans";
            liveCfg.cacheEnabled = true;
            liveCfg.cacheFile = (testDir / "live_j2k_cache.jsonl").wstring();

            aigiskr::core::AutoTranslateManager liveMgr;
            liveMgr.SetBackendForTesting(std::make_unique<aigiskr::core::HdTransJ2KAdapter>());
            Check(liveMgr.Initialize(liveCfg), "R3.2 Real J2KEngine.dll AutoTranslateManager Initialize");
            Check(liveMgr.LookupCacheOrQueue("%c[ff0000]攻撃力%c[ffffff]が10秒間上昇", 0x200u, "WarGodBlessing", out) ==
                      aigiskr::core::AutoLookupOutcome::Queued,
                  "R3.2 Real J2KEngine.dll enqueues tagged Japanese text");
            Check(liveMgr.LookupCacheOrQueue("ソルジャーズブレイブ", 0x201u, "UnitList", out) ==
                      aigiskr::core::AutoLookupOutcome::Queued,
                  "R3.2 Real J2KEngine.dll enqueues Katakana ability name");
            Check(liveMgr.WaitForIdleForTesting(10000), "R3.2 Real J2KEngine.dll completes queued batch");
            const bool hit1 = liveMgr.LookupCacheOrQueue("%c[ff0000]攻撃力%c[ffffff]が10秒間上昇", 0x200u, "WarGodBlessing", out) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit;
            const auto stLive = liveMgr.GetStats();
            Check(hit1 && stLive.jobsCompleted == 2 && stLive.tagRejected == 0 && stLive.backendInitialized,
                  "R3.2 Real J2KEngine.dll translates cleanly with tag preservation");
            liveMgr.Shutdown(true);
        }

        // R3.3 Tests: KnowledgeLayer v0 (negative guards, sentinels + batchim agreement, Pre/Post rules, cache kl_ver)
        {
            aigiskr::core::KnowledgeLayer klMissing;
            std::string klErr;
            Check(!klMissing.LoadFromDirectory(L"C:\\NO_SUCH_KL_DIR_XYZ", &klErr) && !klMissing.IsLoaded(),
                  "R3.3 KnowledgeLayer missing directory fails gracefully without crash");

            aigiskr::core::KnowledgeLayer kl;
            const std::filesystem::path repoKlDir =
                std::filesystem::exists("data\\knowledge\\manifest_v0.json")
                    ? std::filesystem::path("data\\knowledge")
                    : std::filesystem::path("..\\..\\data\\knowledge");
            Check(kl.LoadFromDirectory(repoKlDir.wstring(), &klErr) && kl.IsLoaded(),
                  "R3.3 KnowledgeLayer loads data/knowledge manifest and rules");
            const auto kst = kl.GetStats();
            Check(kst.version == "aigis-v0" && kst.exactOverrides >= 30 &&
                      kst.properNouns >= 50 && kst.preRules >= 20 && kst.postRules >= 30,
                  "R3.3 KnowledgeLayer exposes expected version and rule counts");

            std::string exactOut;
            Check(kl.LookupExactOverride("ソルジャーズブレイブ", exactOut) && exactOut == "솔저즈 브레이브" &&
                      kl.LookupExactOverride("太上老君", exactOut) && exactOut == "태상노군",
                  "R3.3 KnowledgeLayer exact overrides translate WarGod and opaque deity names");

            // Negative guards: マス in 女神アダマス, ちび in ちびっこ, 引き付け in verb 引き付ける, アンナ in アリアンナ, 殿 in 神殿/ドロテア殿
            const auto guardAdamas = kl.PreProcess("女神アダマスは勝利した");
            Check(guardAdamas.properNounsMasked == 0 && guardAdamas.text.find("AKN") == std::string::npos,
                  "R3.3 Guard: 女神アダマス does not false-trigger grid マス rule");

            const auto guardChibiVerb = kl.PreProcess("ちびっこ扱いしないでください");
            Check(guardChibiVerb.properNounsMasked == 0 && guardChibiVerb.text.find("AKN") == std::string::npos,
                  "R3.3 Guard: ちびっこ does not false-trigger ちび character prefix");

            const auto guardAggroVerb = kl.PreProcess("敵の遠距離攻撃を引き付ける");
            Check(guardAggroVerb.properNounsMasked == 0 && guardAggroVerb.text.find("AKN") == std::string::npos,
                  "R3.3 Guard: verbal 引き付ける does not false-trigger nominal 引き付け");

            const auto guardAggroMasu = kl.PreProcess("遠距離攻撃を自身に引き付けます。");
            Check(guardAggroMasu.properNounsMasked == 0 && guardAggroMasu.text.find("AKN") == std::string::npos,
                  "R3.3 Guard: verbal 引き付けます does not false-trigger nominal 引き付け");

            const auto guardAnna = kl.PreProcess("アリアンナとドロテア殿は神殿へ");
            Check(guardAnna.properNounsMasked == 0 && guardAnna.text.find("AKN") == std::string::npos,
                  "R3.3 Guard: アリアンナ / ドロテア殿 / 神殿 do not false-trigger アンナ / NPC殿");

            // Notice angle bracket shield + restore roundtrip
            const auto preNotice = kl.PreProcess("＜超猛毒：HPに継続ダメージ＞");
            Check(preNotice.text == "《超猛毒：HPに継続ダメージ》",
                  "R3.3 PreRule shields fullwidth ＜...＞ into 《...》");
            const auto postNotice = kl.PostProcess("《초맹독：HP에 계속 데미지》", preNotice);
            Check(postNotice.text == "＜초맹독: HP에 계속 데미지＞",
                  "R3.3 PostRule restores 《...》 back to ＜...＞ cleanly");

            // Proper noun sentinel + Korean batchim particle agreement (vowel 라+이->가, consonant ㄹ+가->이, ㄹ+으로->로, ㄴ+로->으로)
            {
                const auto preEftora = kl.PreProcess("突然エフトラがふたり");
                Check(preEftora.sentinels.size() == 1 && preEftora.text.find("AKN0AKN") != std::string::npos,
                      "R3.3 Sentinel masks エフトラ cleanly");
                const std::string simJ2k = "갑자기 " + aigiskr::core::WideToUtf8(preEftora.sentinels[0].sentinelWide) + "이 둘";
                const auto postEftora = kl.PostProcess(simJ2k, preEftora);
                Check(postEftora.text == "갑자기 에프트라가 둘",
                      "R3.3 Batchim fixer corrects 에프트라(vowel)+이 -> 에프트라가");
            }
            {
                const auto preEiketsu = kl.PreProcess("英傑が増えた");
                Check(preEiketsu.sentinels.size() == 1, "R3.3 Sentinel masks 英傑");
                const std::string s = aigiskr::core::WideToUtf8(preEiketsu.sentinels[0].sentinelWide);
                Check(kl.PostProcess(s + "가 늘었다", preEiketsu).text == "영걸이 늘었다",
                      "R3.3 Batchim fixer corrects 영걸(consonant)+가 -> 영걸이");
                Check(kl.PostProcess(s + "으로 공격", preEiketsu).text == "영걸로 공격",
                      "R3.3 Batchim fixer obeys ㄹ-batchim exception: 영걸으로 -> 영걸로");
            }
            {
                const auto preChibiTag = aigiskr::core::MaskProtectedTags("ちび聖霊ニナ<NUM3>体");
                const auto preChibi = kl.PreProcess(preChibiTag.masked);
                const std::string simJ2k = aigiskr::core::WideToUtf8(preChibi.sentinels[0].sentinelWide) + "성령 니나__AIGIS_TAG_0000__몸";
                std::string restoredTags;
                std::string rsn;
                Check(aigiskr::core::RestoreProtectedTags(simJ2k, preChibiTag.tokens, restoredTags, &rsn),
                      "R3.3 Protected tag <NUM3> survives alongside AKN sentinel");
                const auto postChibi = kl.PostProcess(restoredTags, preChibi);
                Check(postChibi.text == "치비 성령 니나 <NUM3>체",
                      "R3.3 PostProcess restores ちび spacing, <NUM3> tag space, and 몸->체 counter");
            }
            {
                // Ellipsis/star boundary particles, 이라고->라고, counter 체을->체를, real LF (not literal \\n)
                const auto preEllip = kl.PreProcess("エフトラと……ちぃエフトラと呼んでください");
                Check(preEllip.sentinels.size() == 2, "R3.3 Sentinel masks ちぃエフトラ and エフトラ cleanly");
                if (preEllip.sentinels.size() == 2) {
                    // PN-005 (priority 280) -> AKN0AKN (치이 에프트라), PN-020 (priority 250) -> AKN1AKN (에프트라)
                    const std::string sChii = aigiskr::core::WideToUtf8(preEllip.sentinels[0].sentinelWide);
                    const std::string sEft = aigiskr::core::WideToUtf8(preEllip.sentinels[1].sentinelWide);
                    const auto postEllip = kl.PostProcess(sEft + "과……" + sChii + "이라고 불러 주세요", preEllip);
                    Check(postEllip.text == "에프트라와……치이 에프트라라고 불러 주세요",
                          "R3.3 Batchim fixer handles ellipsis (…) boundary and 이라고->라고");
                }

                const auto preDono = kl.PreProcess("NPC殿 ※殿は★評価対象");
                Check(preDono.sentinels.size() == 2, "R3.3 Sentinel masks NPC殿 and ※殿 when ShiroPro context holds");
                if (preDono.sentinels.size() == 2) {
                    const std::string sd0 = aigiskr::core::WideToUtf8(preDono.sentinels[0].sentinelWide);
                    const std::string sd1 = aigiskr::core::WideToUtf8(preDono.sentinels[1].sentinelWide);
                    const auto postDono = kl.PostProcess(sd0 + " " + sd1 + "은★평가 대상", preDono);
                    Check(postDono.text == "NPC 도노 ※도노는★평가 대상",
                          "R3.3 Batchim fixer handles symbol (★) boundary on ※도노은");
                }

                const auto postCounter = kl.PostProcess("[ITEM_NAME]을 사용\n성령 <NUM3>몸을 구매", {});
                Check(postCounter.text == "[ITEM_NAME]을(를) 사용하여\n성령 <NUM3>체를 구매",
                      "R3.3 PostProcess fixes [ITEM_NAME] newline with actual LF and <NUM3>몸을 -> <NUM3>체를");
            }

            // Cache versioning & RetranslateStale policy test
            const auto klCacheFile = testDir / "kl_versioned_cache.jsonl";
            {
                std::ofstream ofs(klCacheFile, std::ios::binary | std::ios::trunc);
                ofs << "{\"src\":\"旧未バージョンテスト\",\"dst\":\"구버전 무버전\"}\n";
                ofs << "{\"src\":\"古いバージョンテスト\",\"dst\":\"구버전 kl\",\"kl_ver\":\"aigis-v0-old\"}\n";
                ofs << "{\"src\":\"現行バージョンテスト\",\"dst\":\"현행 버전 유지\",\"kl_ver\":\"aigis-v0\"}\n";
            }

            struct KlFakeBackend final : public aigiskr::core::IAutoTranslateBackend {
                bool initialized = false;
                bool ProbeAvailability(const std::wstring&, std::string* d = nullptr) const override {
                    if (d) *d = "ready";
                    return true;
                }
                bool Initialize(const std::wstring&, std::string* e = nullptr) override {
                    initialized = true;
                    if (e) e->clear();
                    return true;
                }
                void Shutdown(bool = true) override { initialized = false; }
                bool IsInitialized() const noexcept override { return initialized; }
                bool TranslateUtf8(std::string_view in, std::string& out, std::string*) override {
                    out = std::string(in) + " -> 번역됨";
                    return true;
                }
                std::string EngineVersion() const override { return "FakeJ2K/1.0"; }
            };

            aigiskr::core::AutoTranslateConfig klCfg;
            klCfg.enabled = true;
            klCfg.cacheEnabled = true;
            klCfg.cacheFile = klCacheFile.wstring();
            klCfg.knowledgeDir = repoKlDir.wstring();
            klCfg.retranslateStale = true;

            aigiskr::core::AutoTranslateManager klMgr;
            klMgr.SetBackendForTesting(std::make_unique<KlFakeBackend>());
            Check(klMgr.Initialize(klCfg), "R3.3 AutoTranslateManager Initialize with RetranslateStale=1");
            const auto stInit = klMgr.GetStats();
            Check(stInit.knowledgeLoaded && stInit.knowledgeVersion == "aigis-v0" &&
                      stInit.staleCacheSkipped == 2 && stInit.currentCacheEntries == 1,
                  "R3.3 RetranslateStale=1 skips legacy & mismatched kl_ver entries while preserving current kl_ver");

            std::string klOut;
            Check(klMgr.LookupCacheOrQueue("現行バージョンテスト", 0x300u, "UI", klOut) ==
                          aigiskr::core::AutoLookupOutcome::CacheHit &&
                      klOut == "현행 버전 유지",
                  "R3.3 Matching kl_ver entry hits immediately");
            Check(klMgr.LookupCacheOrQueue("ソルジャーズブレイブ", 0x301u, "WarGod", klOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                  "R3.3 WarGod exact override queues cleanly on first miss");
            Check(klMgr.LookupCacheOrQueue("旧未バージョンテスト", 0x302u, "UI", klOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                  "R3.3 Skipped stale entry re-queues for fresh translation");
            Check(klMgr.WaitForIdleForTesting(5000), "R3.3 AutoTranslateManager drains KL jobs");
            Check(klMgr.LookupCacheOrQueue("ソルジャーズブレイブ", 0x301u, "WarGod", klOut) ==
                          aigiskr::core::AutoLookupOutcome::CacheHit &&
                      klOut == "솔저즈 브레이브",
                  "R3.3 Worker KL exact override persists to cache");
            const auto stKlDone = klMgr.GetStats();
            Check(stKlDone.klExactHits == 1 && stKlDone.jobsCompleted == 2,
                  "R3.3 KL exact override telemetry records hit accurately");
            klMgr.Shutdown(true);

            // Verify appended cache line carries "kl_ver":"aigis-v0"
            std::ifstream checkCache(klCacheFile, std::ios::binary);
            std::string cacheContent((std::istreambuf_iterator<char>(checkCache)),
                                     std::istreambuf_iterator<char>());
            Check(cacheContent.find("\"kl_ver\":\"aigis-v0\"") != std::string::npos,
                  "R3.3 Persisted runtime JSONL cache embeds kl_ver metadata");
        }

        // 15. R3.5 aigis-v0.1 P0 hotfix rules (PN-072, PN-045, PO-041, EO-039/040)
        const std::filesystem::path repoKlDirCheck =
            std::filesystem::exists("data\\knowledge\\manifest_v0.json")
                ? std::filesystem::path("data\\knowledge")
                : std::filesystem::path("..\\..\\data\\knowledge");
        const std::filesystem::path repoKlV01Dir =
            std::filesystem::exists("data\\knowledge_v0_1\\manifest_v0_1.json")
                ? std::filesystem::path("data\\knowledge_v0_1")
                : std::filesystem::path("..\\..\\data\\knowledge_v0_1");
        if (std::filesystem::exists(repoKlV01Dir / "manifest_v0_1.json", ec)) {
            aigiskr::core::KnowledgeLayer kl01;
            std::string kl01Err;
            Check(kl01.LoadFromDirectory(repoKlV01Dir.wstring(), &kl01Err) &&
                      kl01.Version() == "aigis-v0.1",
                  "R3.5 KnowledgeLayer v0.1 loads manifest_v0_1.json with version aigis-v0.1");

            // Freeze check: data/knowledge still loads immutable aigis-v0
            aigiskr::core::KnowledgeLayer kl0Check;
            Check(kl0Check.LoadFromDirectory(repoKlDirCheck.wstring()) && kl0Check.Version() == "aigis-v0",
                  "R3.5 Frozen data/knowledge remains strictly aigis-v0");

            // PN-072: ちびアクアナイト defers to PostProcess without breaking J2K, ちび聖霊ニナ still works
            {
                const auto preAqua = kl01.PreProcess("ちびアクアナイト");
                Check(preAqua.text == "ちびアクアナイト" && preAqua.deferredRewrites.size() == 1,
                      "R3.5 PN-072 leaves ちびアクアナイト intact for J2K and schedules deferred rewrite");
                const auto postAqua = kl01.PostProcess("꼬맹이 아쿠아 나이트", preAqua);
                Check(postAqua.text == "치비 아쿠아 나이트",
                      "R3.5 PN-072 PostProcess rewrites 꼬맹이 아쿠아 나이트 -> 치비 아쿠아 나이트");

                const auto preStoryChibi = kl01.PreProcess("このチビめ");
                Check(preStoryChibi.deferredRewrites.empty() && preStoryChibi.sentinels.empty(),
                      "R3.5 Story チビ does not trigger PN-072 deferred rewrite");
                Check(kl01.PostProcess("이 꼬맹이 녀석", preStoryChibi).text == "이 꼬맹이 녀석",
                      "R3.5 Story 꼬맹이 without ちび<katakana> remains untouched");

                const auto preNinaTag = aigiskr::core::MaskProtectedTags("ちび聖霊ニナ<NUM3>体");
                const auto preNina = kl01.PreProcess(preNinaTag.masked);
                Check(preNina.sentinels.size() == 1, "R3.5 PN-072 still sentinelizes ちび+kanji");
                std::string restored;
                std::string rsn;
                aigiskr::core::RestoreProtectedTags(
                    aigiskr::core::WideToUtf8(preNina.sentinels[0].sentinelWide) + "성령 니나__AIGIS_TAG_0000__몸",
                    preNinaTag.tokens, restored, &rsn);
                Check(kl01.PostProcess(restored, preNina).text == "치비 성령 니나 <NUM3>체",
                      "R3.5 PN-072 sentinel + kanji path unchanged");
            }

            // PN-045: ワルキューレプロテクト spacing vs ワルキューレ系クラス tight suffix
            {
                const auto preProt = kl01.PreProcess("ワルキューレプロテクト");
                Check(preProt.sentinels.size() == 1 && preProt.text.find("AKN0AKN ") != std::string::npos,
                      "R3.5 PN-045 inserts PreProcess space before following katakana");
                const auto postProt = kl01.PostProcess("AKN0AKN 프로텍트", preProt);
                Check(postProt.text == "발키리 프로텍트",
                      "R3.5 PN-045 restores 발키리 프로텍트 with clean boundary spacing");

                const auto preKei = kl01.PreProcess("ワルキューレ系クラス");
                Check(preKei.sentinels.size() == 2 && preKei.text.find("AKN0AKN系") != std::string::npos,
                      "R3.5 PN-045 keeps ワルキューレ系 unspaced before kanji suffix");
                const auto postKei = kl01.PostProcess("AKN0AKN계 AKN1AKN", preKei);
                Check(postKei.text == "발키리계 클래스",
                      "R3.5 PN-045 preserves 발키리계 클래스 without unwanted space before 계");
            }

            // PO-041A..F: 퇴각 취급 particle agreement + non-particle guard
            {
                Check(kl01.PostProcess("아군 유닛을\n퇴각 취급으로 한다", {}).text ==
                          "아군 유닛을\n퇴각 처리로 한다",
                      "R3.5 PO-041A fixes 퇴각 취급으로 -> 퇴각 처리로");
                Check(kl01.PostProcess("사망 시 퇴각 취급이 되어", {}).text ==
                          "사망 시 퇴각 처리가 되어",
                      "R3.5 PO-041B fixes 퇴각 취급이 -> 퇴각 처리가");
                Check(kl01.PostProcess("퇴각 취급은 없다 / 퇴각 취급을 한다 / 퇴각 취급과 동등", {}).text ==
                          "퇴각 처리는 없다 / 퇴각 처리를 한다 / 퇴각 처리와 동등",
                      "R3.5 PO-041C/D/E fix 은/을/과 batchim agreement");
                Check(kl01.PostProcess("퇴각 취급 이후 행동", {}).text ==
                          "퇴각 처리 이후 행동",
                      "R3.5 PO-041 boundary guard preserves 이후 without mis-rewriting 이");
            }

            // EO-039 / EO-040 WarGod titles
            {
                std::string eoOut;
                Check(kl01.LookupExactOverride("神の加護あれ！", eoOut) && eoOut == "신의 가호가 있기를!",
                      "R3.5 EO-039 WarGod title 神の加護あれ！ present in aigis-v0.1");
                Check(kl01.LookupExactOverride("東のもののふたち", eoOut) && eoOut == "동방의 무사들",
                      "R3.5 EO-040 WarGod title 東のもののふたち present in aigis-v0.1");
            }
        }

        std::filesystem::remove_all(testDir, ec);
    }

    // 16. R3.5 Host Protocol Framing, Pipe Roundtrip, Selection, Crash/Restart & Real Host Tests
    {
        namespace hp = aigiskr::hdtrans_host;

        // Wire framing encode/decode & constants
        Check(hp::kProtocolVersion == 1u && hp::kMaxPayloadBytes == 65536u && hp::kFrameHeaderBytes == 16u,
              "R3.5 Host protocol version=1 and max payload=64KiB constants");
        {
            hp::RequestHeader rh{hp::kProtocolMagic, hp::kProtocolVersion, static_cast<uint16_t>(hp::Opcode::Translate), 42u, 128u};
            uint8_t buf[16]{};
            hp::EncodeRequestHeader(rh, buf);
            const auto dec = hp::DecodeRequestHeader(buf);
            Check(dec.magic == rh.magic && dec.protocolVersion == rh.protocolVersion &&
                      dec.opcode == rh.opcode && dec.requestId == 42u && dec.payloadSize == 128u,
                  "R3.5 RequestHeader little-endian encode/decode roundtrip");

            hp::ResponseHeader rsp{hp::kProtocolMagic, hp::kProtocolVersion, static_cast<uint16_t>(hp::Status::Ok), 42u, 99u};
            hp::EncodeResponseHeader(rsp, buf);
            const auto rdec = hp::DecodeResponseHeader(buf);
            Check(rdec.magic == rsp.magic && rdec.protocolVersion == rsp.protocolVersion &&
                      rdec.status == 0 && rdec.requestId == 42u && rdec.payloadSize == 99u,
                  "R3.5 ResponseHeader little-endian encode/decode roundtrip");
        }

        // Anonymous pipe multiline UTF-8 + malformed + oversized framing checks
        {
            HANDLE rPipe = nullptr, wPipe = nullptr;
            Check(CreatePipe(&rPipe, &wPipe, nullptr, 131072) != FALSE, "R3.5 Create anonymous test pipe");
            const std::string multiUtf8 = "1行目テスト\n2行目テスト<NUM0>\n한국어テスト";
            Check(hp::WriteRequestFrame(wPipe, hp::Opcode::Translate, 77u, multiUtf8),
                  "R3.5 WriteRequestFrame sends multiline UTF-8 payload");
            hp::RequestHeader gotHdr{};
            std::string gotPayload;
            Check(hp::ReadRequestFrame(rPipe, gotHdr, gotPayload) == hp::PipeIoStatus::Ok &&
                      gotHdr.requestId == 77u && gotPayload == multiUtf8,
                  "R3.5 ReadRequestFrame preserves multiline UTF-8 payload accurately");

            // Oversized payload guard (WriteRequestFrame rejects, ReadRequestFrame flags PayloadTooLarge)
            const std::string huge(hp::kMaxPayloadBytes + 1, 'a');
            Check(!hp::WriteRequestFrame(wPipe, hp::Opcode::Translate, 78u, huge),
                  "R3.5 WriteRequestFrame rejects >64KiB payload");

            uint8_t rawOver[16]{};
            hp::EncodeRequestHeader({hp::kProtocolMagic, hp::kProtocolVersion,
                                     static_cast<uint16_t>(hp::Opcode::Translate), 79u,
                                     hp::kMaxPayloadBytes + 1},
                                    rawOver);
            Check(hp::WriteExactBytes(wPipe, rawOver, 16), "R3.5 Write oversized header raw bytes");
            Check(hp::ReadRequestFrame(rPipe, gotHdr, gotPayload) == hp::PipeIoStatus::PayloadTooLarge,
                  "R3.5 ReadRequestFrame rejects >64KiB payload cleanly");

            // Malformed magic guard
            uint8_t badMagic[16]{};
            hp::EncodeRequestHeader({0xDEADBEEFu, 1u, 1u, 80u, 0u}, badMagic);
            hp::WriteExactBytes(wPipe, badMagic, 16);
            Check(hp::ReadRequestFrame(rPipe, gotHdr, gotPayload) == hp::PipeIoStatus::InvalidMagic,
                  "R3.5 ReadRequestFrame rejects malformed magic bytes");

            CloseHandle(wPipe);
            CloseHandle(rPipe);
        }

        std::error_code ec;
        const auto hostTestDir = std::filesystem::temp_directory_path(ec) / "aigiskr_r35_host_tests";
        std::filesystem::remove_all(hostTestDir, ec);
        std::filesystem::create_directories(hostTestDir, ec);

        // Bundled / External / Auto selection & Config INI tests
        {
            const auto fakePlugin = hostTestDir / "game" / "scripts";
            const auto fakeHost = fakePlugin / "AigisKR" / "bin" / "AigisKR.HdTransHost.exe";
            const auto fakeBundled = fakePlugin / "AigisKR" / "hdtrans";
            const auto fakeExt = hostTestDir / "ext_hdtrans";
            const auto fakeInvalidExt = hostTestDir / "missing_ext_hdtrans";

            std::filesystem::create_directories(fakeHost.parent_path(), ec);
            std::filesystem::create_directories(fakeBundled / "hdtrans-data", ec);
            std::filesystem::create_directories(fakeExt / "hdtrans-data", ec);

            { std::ofstream f(fakeHost, std::ios::binary); f << "stub"; }
            { std::ofstream f(fakeBundled / "J2KEngine.dll", std::ios::binary); f << "stub"; }
            { std::ofstream f(fakeBundled / "hdtrans-data" / "dicts.hdsnap", std::ios::binary); f << "stub"; }
            { std::ofstream f(fakeExt / "J2KEngine.dll", std::ios::binary); f << "stub"; }
            { std::ofstream f(fakeExt / "hdtrans-data" / "dicts.hdsnap", std::ios::binary); f << "stub"; }

            aigiskr::core::AutoTranslateConfig ac;
            ac.enabled = true;
            ac.hdTransHostPath = fakeHost.wstring();
            ac.bundledHdTransPath = fakeBundled.wstring();

            ac.hdTransMode = aigiskr::core::HdTransMode::Auto;
            ac.hdTransPath.clear();
            auto sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(sel.available && sel.rootType == "bundled" && sel.warning.empty(),
                  "R3.5 Selection: Auto + empty HdTransPath picks Bundled");

            ac.hdTransPath = fakeExt.wstring();
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(sel.available && sel.rootType == "external" && sel.resolvedEngineRoot == fakeExt.wstring(),
                  "R3.5 Selection: Auto + valid HdTransPath picks External");

            ac.hdTransPath = fakeInvalidExt.wstring();
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(sel.available && sel.rootType == "bundled" && !sel.warning.empty(),
                  "R3.5 Selection: Auto + invalid HdTransPath warns and falls back to Bundled");

            ac.hdTransMode = aigiskr::core::HdTransMode::Bundled;
            ac.hdTransPath = fakeExt.wstring();
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(sel.available && sel.rootType == "bundled",
                  "R3.5 Selection: Bundled ignores valid External HdTransPath");

            std::filesystem::remove(fakeBundled / "J2KEngine.dll", ec);
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(!sel.available && sel.rootType == "none",
                  "R3.5 Selection: Bundled + missing bundle fails open without External fallback");

            { std::ofstream f(fakeBundled / "J2KEngine.dll", std::ios::binary); f << "stub"; }
            ac.hdTransMode = aigiskr::core::HdTransMode::External;
            ac.hdTransPath = fakeInvalidExt.wstring();
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(!sel.available && sel.rootType == "none",
                  "R3.5 Selection: External + invalid External fails open without Bundled fallback");

            ac.hdTransPath = fakeExt.wstring();
            std::filesystem::remove(fakeHost, ec);
            sel = aigiskr::core::ResolveHdTransSelection(ac);
            Check(!sel.available && sel.rootType == "none",
                  "R3.5 Selection: Missing AigisKR.HdTransHost.exe marks host unavailable safely");

            // INI config parse verification
            const auto iniPath = fakePlugin / "AigisKR.ini";
            {
                std::ofstream ini(iniPath, std::ios::binary);
                ini << "[AutoTranslate]\n"
                    << "Enabled=1\n"
                    << "HdTransMode=External\n"
                    << "HdTransPath=" << aigiskr::core::WideToUtf8(fakeExt.wstring()) << "\n"
                    << "HostTimeoutMs=22000\n";
            }
            const auto parsedCfg = aigiskr::core::LoadProbeConfig(
                iniPath.wstring(), (hostTestDir / "game").wstring(), fakePlugin.wstring());
            Check(parsedCfg.autoTranslate.enabled &&
                      parsedCfg.autoTranslate.hdTransMode == aigiskr::core::HdTransMode::External &&
                      parsedCfg.autoTranslate.hostTimeoutMs == 22000u &&
                      parsedCfg.autoTranslate.hdTransHostPath == fakeHost.wstring() &&
                      parsedCfg.autoTranslate.bundledHdTransPath == fakeBundled.wstring(),
                  "R3.5 LoadProbeConfig parses HdTransMode/HostTimeoutMs and plugin-relative defaults");
            const auto unicodeExternal = hostTestDir / L"한글 폴더 (A&B)";
            {
                std::ofstream ini(iniPath, std::ios::binary | std::ios::trunc);
                ini << "[AutoTranslate]\nEnabled=1\nHdTransMode=External\nHdTransPath="
                    << aigiskr::core::WideToUtf8(unicodeExternal.wstring()) << "\n";
            }
            const auto unicodeCfg = aigiskr::core::LoadProbeConfig(
                iniPath.wstring(), (hostTestDir / "game").wstring(), fakePlugin.wstring());
            Check(unicodeCfg.autoTranslate.hdTransPath == unicodeExternal.wstring(),
                  "Astra UTF-8 INI preserves Korean external path with spaces parentheses and ampersand");
        }

        // Recover across repeated disconnects and failed initial handshakes.
        {
            struct RecoveringBackend final : public aigiskr::core::IAutoTranslateBackend {
                std::atomic<int> initCalls{0}, translateCalls{0};
                int initFailures = 0;
                bool alwaysCrash = false, initialized = false;
                aigiskr::core::BackendFailureKind lastFail = aigiskr::core::BackendFailureKind::None;
                bool ProbeAvailability(const std::wstring&, std::string* = nullptr) const override { return true; }
                bool Initialize(const std::wstring&, std::string* d = nullptr) override {
                    initialized = ++initCalls > initFailures;
                    if (d) *d = initialized ? "ok" : "temporary_init_failure";
                    return initialized;
                }
                bool TranslateUtf8(std::string_view in, std::string& out, std::string* err = nullptr) override {
                    const int n = ++translateCalls;
                    if (alwaysCrash || n == 1 || n == 3) {
                        initialized = false;
                        lastFail = n == 3 ? aigiskr::core::BackendFailureKind::Timeout
                                          : aigiskr::core::BackendFailureKind::TransportCrash;
                        if (err) *err = "temporary_transport_failure";
                        return false;
                    }
                    lastFail = aigiskr::core::BackendFailureKind::None;
                    out = std::string(in) + "(OK)";
                    return true;
                }
                void Shutdown(bool = true) override { initialized = false; }
                bool IsInitialized() const noexcept override { return initialized; }
                std::string EngineVersion() const override { return "RecoveringFake/1.0"; }
                aigiskr::core::BackendFailureKind LastFailureKind() const noexcept override { return lastFail; }
                aigiskr::core::HostTelemetrySnapshot GetHostTelemetry() const override {
                    aigiskr::core::HostTelemetrySnapshot s;
                    s.isHostBackend = true;
                    s.hostSpawned = initCalls > 0;
                    s.hostSpawnCount = initCalls.load();
                    return s;
                }
            };
            aigiskr::core::AutoTranslateConfig ccfg;
            ccfg.enabled = true;
            ccfg.cacheEnabled = false;
            auto* fake = new RecoveringBackend();
            aigiskr::core::AutoTranslateManager mgr;
            mgr.SetBackendForTesting(std::unique_ptr<aigiskr::core::IAutoTranslateBackend>(fake));
            Check(mgr.Initialize(ccfg), "Recovery manager initializes lazily");
            std::string out;
            mgr.LookupCacheOrQueue("最初の通信失敗", 0x101, "UI", out);
            mgr.LookupCacheOrQueue("次の翻訳", 0x102, "UI", out);
            Check(mgr.WaitForIdleForTesting(6000), "Two transport failures recover and drain the preserved queue");
            const auto st = mgr.GetStats();
            Check(st.backendAvailable && st.hostRestartCount == 2 && st.hostSpawnCount == 3 &&
                  st.jobsCompleted == 2 && st.jobsFailed == 2 && st.renderThreadBackendCalls == 0,
                  "Second Host failure no longer disables the session; retries remain on the worker");
            Check(mgr.LookupCacheOrQueue("最初の通信失敗", 0x101, "UI", out) == aigiskr::core::AutoLookupOutcome::CacheHit &&
                  mgr.LookupCacheOrQueue("次の翻訳", 0x102, "UI", out) == aigiskr::core::AutoLookupOutcome::CacheHit,
                  "Both failed source and queued neighbor complete without screen reload or requeue");
            mgr.LookupCacheOrQueue("障害後の新しい翻訳", 0x103, "UI", out);
            Check(mgr.WaitForIdleForTesting(2000) && mgr.LookupCompleted("障害後の新しい翻訳", out),
                  "New jobs still translate after multiple Host recoveries");
            mgr.Shutdown(true);

            fake = new RecoveringBackend();
            fake->initFailures = 2;
            fake->translateCalls = 3; // focus this case on initialization recovery
            mgr.SetBackendForTesting(std::unique_ptr<aigiskr::core::IAutoTranslateBackend>(fake));
            mgr.Initialize(ccfg);
            const auto started = std::chrono::steady_clock::now();
            mgr.LookupCacheOrQueue("初期接続失敗から復帰", 0x101, "UI", out);
            Check(mgr.WaitForIdleForTesting(6500) && mgr.LookupCompleted("初期接続失敗から復帰", out) &&
                  mgr.GetStats().hostRestartCount == 2 &&
                  std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2900),
                  "Failed HELLO/init retries preserve pending source and use increasing cooldown");
            mgr.Shutdown(true);

            fake = new RecoveringBackend();
            fake->alwaysCrash = true;
            mgr.SetBackendForTesting(std::unique_ptr<aigiskr::core::IAutoTranslateBackend>(fake));
            mgr.Initialize(ccfg);
            mgr.LookupCacheOrQueue("継続障害の翻訳", 0x101, "UI", out);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!mgr.GetStats().jobsFailed && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            Check(mgr.GetStats().jobsFailed == 1 && fake->initCalls == 1 &&
                  mgr.LookupCacheOrQueue("継続障害の翻訳", 0x101, "UI", out) == aigiskr::core::AutoLookupOutcome::AlreadyPending,
                  "Persistent failure stays deduplicated during cooldown, preventing a spawn storm");
            const auto stopStart = std::chrono::steady_clock::now();
            mgr.Shutdown(true);
            Check(std::chrono::steady_clock::now() - stopStart < std::chrono::milliseconds(500),
                  "Shutdown interrupts recovery cooldown immediately");
        }

        // Real AigisKR.HdTransHost.exe IPC integration & timeout tests when binary + D:\hdTrans exist
        wchar_t selfExe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
        const std::filesystem::path exeDir = std::filesystem::path(selfExe).parent_path();
        std::filesystem::path realHostExe = exeDir / "AigisKR.HdTransHost.exe";
        if (!std::filesystem::is_regular_file(realHostExe, ec)) {
            realHostExe = exeDir.parent_path() / "tools" / "hdtrans_host" / "Release" / "AigisKR.HdTransHost.exe";
        }
        const std::filesystem::path localHdTrans(L"D:\\hdTrans");
        const std::filesystem::path repoKlV01Dir2 =
            std::filesystem::exists("data\\knowledge_v0_1\\manifest_v0_1.json")
                ? std::filesystem::path("data\\knowledge_v0_1")
                : std::filesystem::path("..\\..\\data\\knowledge_v0_1");
        if (std::filesystem::is_regular_file(realHostExe, ec) &&
            aigiskr::core::ProbeHdTransEngineRoot(localHdTrans.wstring())) {
            // Test B: Full end-to-end AutoTranslateManager + real AigisKR.HdTransHost.exe + aigis-v0.1!
            const auto realCache = hostTestDir / "real_host_cache.jsonl";
            aigiskr::core::AutoTranslateConfig rcfg;
            rcfg.enabled = true;
            rcfg.hdTransMode = aigiskr::core::HdTransMode::External;
            rcfg.hdTransPath = localHdTrans.wstring();
            rcfg.hdTransHostPath = realHostExe.wstring();
            rcfg.bundledHdTransPath = (hostTestDir / "unused_bundled").wstring();
            rcfg.cacheEnabled = true;
            rcfg.cacheFile = realCache.wstring();
            rcfg.hostTimeoutMs = 15000u;
            if (std::filesystem::exists(repoKlV01Dir2 / "manifest_v0_1.json", ec)) {
                rcfg.knowledgeDir = repoKlV01Dir2.wstring();
            }

            // B1. Warm cache zero-spawn check before cold jobs
            {
                std::ofstream wofs(realCache, std::ios::binary | std::ios::trunc);
                wofs << "{\"src\":\"攻撃力上昇テスト\",\"dst\":\"공격력 상승 테스트\",\"kl_ver\":\"aigis-v0.1\"}\n";
            }
            {
                aigiskr::core::AutoTranslateManager realMgr;
                Check(realMgr.Initialize(rcfg), "R3.5 Real Host AutoTranslateManager Initialize succeeds");
                std::string warmOut;
                Check(realMgr.LookupCacheOrQueue("攻撃力上昇テスト", 0x200u, "UI", warmOut) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          warmOut == "공격력 상승 테스트",
                      "R3.5 Real Host warm cache hit succeeds");
                const auto stWarm = realMgr.GetStats();
                Check(!stWarm.hostSpawned && stWarm.hostSpawnCount == 0 && stWarm.renderThreadHostIpcCalls == 0,
                      "R3.5 Real Host warm cache maintains host_spawned=0");

                // Now queue cold jobs: P0 hotfixes + multiline + WarGod exact overrides
                std::string dummyOut;
                Check(realMgr.LookupCacheOrQueue("ちびアクアナイト", 0x201u, "Unit", dummyOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                      "R3.5 Real Host queues ちびアクアナイト");
                Check(realMgr.LookupCacheOrQueue("ワルキューレプロテクト", 0x202u, "WarGod", dummyOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                      "R3.5 Real Host queues ワルキューレプロテクト");
                Check(realMgr.LookupCacheOrQueue("死亡時撤退扱いとなり\n再出撃可能", 0x203u, "Unit", dummyOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                      "R3.5 Real Host queues multiline 撤退扱いとなり");
                Check(realMgr.LookupCacheOrQueue("神の加護あれ！", 0x204u, "WarGod", dummyOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                      "R3.5 Real Host queues 神の加護あれ！");
                Check(realMgr.LookupCacheOrQueue("東のもののふたち", 0x205u, "WarGod", dummyOut) ==
                          aigiskr::core::AutoLookupOutcome::Queued,
                      "R3.5 Real Host queues 東のもののふたち");
                Check(realMgr.WaitForIdleForTesting(20000), "R3.5 Real Host drains cold P0 batch");

                const auto stCold = realMgr.GetStats();
                Check(stCold.hostSpawned && stCold.hostSpawnCount == 1 && stCold.hostProtocolVer == 1 &&
                          stCold.hostEngineRootType == "external" &&
                          stCold.hostEngineVer.find("0.9.5") != std::string::npos &&
                          stCold.renderThreadBackendCalls == 0 && stCold.renderThreadHostIpcCalls == 0 &&
                          stCold.ipcCompleted >= 4 && stCold.jobsCompleted == 5 && stCold.jobsFailed == 0,
                      "R3.5 Real Host telemetry confirms lazy spawn, proto=1, hdTrans 0.9.5, 0 render IPC");

                std::string k1, k2, k3, k4, k5;
                Check(realMgr.LookupCacheOrQueue("ちびアクアナイト", 0x201u, "Unit", k1) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          k1 == "치비 아쿠아 나이트",
                      "R3.5 Live Host + KL v0.1: ちびアクアナイト -> 치비 아쿠아 나이트");
                Check(realMgr.LookupCacheOrQueue("ワルキューレプロテクト", 0x202u, "WarGod", k2) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          k2 == "발키리 프로텍트",
                      "R3.5 Live Host + KL v0.1: ワルキューレプロテクト -> 발키리 프로텍트");
                Check(realMgr.LookupCacheOrQueue("死亡時撤退扱いとなり\n再出撃可能", 0x203u, "Unit", k3) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          k3.find("퇴각 처리가") != std::string::npos,
                      "R3.5 Live Host + KL v0.1: 撤退扱い -> 퇴각 처리가 with fixed batchim particle");
                Check(realMgr.LookupCacheOrQueue("神の加護あれ！", 0x204u, "WarGod", k4) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          k4 == "신의 가호가 있기를!",
                      "R3.5 Live Host + KL v0.1: 神の加護あれ！ -> 신의 가호가 있기를!");
                Check(realMgr.LookupCacheOrQueue("東のもののふたち", 0x205u, "WarGod", k5) ==
                              aigiskr::core::AutoLookupOutcome::CacheHit &&
                          k5 == "동방의 무사들",
                      "R3.5 Live Host + KL v0.1: 東のもののふたち -> 동방의 무사들");

                realMgr.Shutdown(true);
                const auto stShut = realMgr.GetStats();
                Check(stShut.hostExitCode == 0, "R3.5 Real Host shuts down cleanly with exit code 0");
            }
        }

        std::filesystem::remove_all(hostTestDir, ec);
    }

    return g_failures == 0 ? 0 : 1;
}
