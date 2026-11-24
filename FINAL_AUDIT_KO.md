# AigisKR v0.1.0 최종 배포 검수

2026-11-24 재정리 (원본 기준 후보 rc.11 / 2026-11-20) / 판정: **RELEASE READY WITH DOCUMENTED DEFERRALS**.

## 검수 결과

| 항목 | 결과 |
|---|---|
| 번역 코어·실제 hdTrans 호스트 회귀 | 289 PASS / 0 FAIL |
| 두 번의 독립 패키징 | binary/source ZIP·manifest·checksums 100% 동일 |
| 설치 ZIP·대응 소스 ZIP CRC와 경로 검사 | PASS (`testzip=None`, 경로 위반 0) |
| 설치 파일별 SHA-256 | 133/133 일치 |
| rc.11 실행 구성 요소 보존 | ASI·Host·TT·winmm·hdTrans·번역 DB·83 DDS 등 125개 파일 바이트 동일 |
| 신규 이미지 검수 | 20개 위치 정적 PASS, 18개 개별 표시 확인 |
| 목표 텍스처 로딩 | 15/15 |
| 최근 실기 주입 실패·폰트 오류·관찰 crash/hang | 0 |
| 대응 소스 ZIP만으로 x86 전체 빌드 (`AigisKR.asi`, `AigisKR.HdTransHost.exe`, `translation_core_tests.exe`) | PASS |
| 설치 Host 및 소스로 재빌드한 Host의 실제 번역·정상 종료 | 각각 PASS |
| 공식 hdTrans 번들 provenance | 14개 파일 SHA 일치 |
| 원본 EXE/게임 캐시 보존 | 3932개 해시 동일 |
| 기존 canonical 이미지 보존 | 510개 해시 동일 |
| 사용자 설정·번역 캐시 | 보존 |

정식 설치 ZIP은 rc.11 검증 실행 구성 요소(125개 파일)를 바이트 단위로 그대로 유지하면서 루트 `LICENSE`(`AGPL-3.0-only`), 동기화된 문서(`README_KO.md`, `KNOWN_LIMITATIONS.md`, `RELEASE_NOTES_KO.md`, `THIRD_PARTY_NOTICES.md`, `LICENSES/hdTrans/SOURCE.txt`, BOM 제거된 `LICENSES/MinHook-BSD-2-Clause.txt`), `diagnostics.ps1`(`0.1.0` 표기)을 반영했다. 대응 소스 ZIP과 공개 저장소에는 `AigisKR.asi` 전체 소스(`CMakeLists.txt`, `src/`, `tests/`, `packaging/`, `third_party/safetyhook`, `third_party/Hooking.Patterns`), `tools/hdtrans_host/`, `docs/patches/texture_toolkit_partial_patch_r3_2.patch`, `upstream/hdTrans-0.9.5/`를 함께 포함했다.

## 보류 항목

사용자 결정에 따라 잠금 전장 변형(N06/03_0662)과 별도 홈 배너(B01/03_0487)는 개별 화면 검수를 보류한다. 두 항목의 원고·텍스처 로딩·정적 통합은 확인했다. 이전의 미노출 이벤트·∞ 스킬 중단·차지 상태와 현재 프레임이 없는 03_0756도 기존 보류를 유지한다.

이는 모든 게임 상태의 UI FINAL FREEZE 판정이 아니다. 명시된 지원 범위와 알려진 제한 사항을 가진 정식 배포 판정이다. 이전 후보의 negative missing-replacement 및 cold-cache 즉시 갱신 검증은 해당 구성 요소가 동일하므로 그 증거를 이어받으며, 이번 회차에서 새로 시행했다고 표기하지 않는다.

## 배포 파일

- `AigisKR-0.1.0.zip`: 126,214,929 bytes / SHA-256 `71cc51d2524d81343d09d7668071bd9eedbc8da94a966ad239458837a383e15e`
- `AigisKR-0.1.0-sources.zip`: 30,959,557 bytes / SHA-256 `7d4eee1cd68c5503a24b8dc658abcd18da5c9c48f468195ebfaa872af3f66006`

파일별 명세는 `RELEASE_MANIFEST.json`, 전체 체크섬은 `SHA256SUMS.txt`를 참조한다. 사용자 EXE·게임 캐시·DMM 인증 정보·개인 로그·스크린샷은 정식 ZIP이나 공개 저장소에 포함하지 않는다.
