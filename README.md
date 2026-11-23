# AigisKR v0.1.0

천년전쟁 아이기스 R Windows판의 한국어 패치입니다. 주요 UI 이미지, 수동 검수 텍스트, 한글 폰트와 오프라인 자동번역을 제공합니다. 게임 실행 파일과 `.cache`를 직접 수정하지 않습니다.

[정식 버전 다운로드](https://github.com/kekwkekw/aigiskr/releases/latest) · [변경 내역](RELEASE_NOTES_KO.md) · [알려진 제한 사항](KNOWN_LIMITATIONS.md)

## 지원 환경

- Windows 네이티브 x86 클라이언트 `Aigis_r.exe`. 검수한 게임 버전은 **5.1.0**입니다.
- 순정 게임 리소스를 기준으로 검수했습니다. 이전 유니코드 패치나 다른 모드와 섞인 환경의 완전 호환은 검증하지 않았습니다.
- 번역 엔진 `hdTrans v0.9.5`를 포함합니다. 기본 사용에 별도 번역 프로그램이나 API 키가 필요하지 않습니다.

## 설치

1. 게임을 완전히 종료합니다.
2. [릴리스 페이지](https://github.com/kekwkekw/aigiskr/releases/latest)에서 **`AigisKR-0.1.0.zip`**을 받습니다. `-sources.zip`은 대응 소스이므로 게임 폴더에 설치하지 않습니다.
3. ZIP 내부의 파일·폴더를 **`Aigis_r.exe`가 있는 폴더**에 압축 해제합니다.
4. 평소 사용하는 DMM 공식 실행 경로로 게임을 시작합니다.

다른 모드가 설치한 `winmm.dll`이 있다면 먼저 백업하고 충돌 여부를 확인하세요. 기존 AigisKR의 파일을 업데이트하는 경우에는 새 배포본으로 교체할 수 있습니다.

```text
게임 폴더/
  Aigis_r.exe                  기존 게임 파일
  winmm.dll                    패치 로더
  AigisKR_Diagnostics.cmd      문제 확인용 도구
  scripts/
    AigisKR.asi
    AigisKR.ini
    TextureToolkit-x86.asi
    TextureToolkit.ini
    AigisKR/
      bin/AigisKR.HdTransHost.exe
      hdtrans/
      data/
  TT/
    texture_manifest_r2.json
    inject/*.dds
```

## 기존 버전에서 업데이트

게임을 종료한 뒤 새 ZIP으로 패치 파일을 교체합니다. 직접 수정한 `scripts/AigisKR.ini`와 `scripts/AigisKR/data/overrides.json`은 먼저 백업하고 필요한 설정을 다시 적용하세요.

기존 자동번역 캐시 `scripts/AigisKR/cache/hdtrans_cache.jsonl`은 유지할 수 있습니다. 배포 ZIP은 사용자 번역 캐시, 원본 EXE와 `.cache`를 포함하지 않습니다.

## 번역 표시와 설정

기본 설정은 자동번역 및 번역 완료 후 현재 화면 갱신을 켭니다. 처음 보는 문장은 잠깐 일본어로 표시될 수 있으며, 번역이 끝나면 지원되는 UI·회상 본문·연습 안내가 한국어로 갱신됩니다. 일부 직접 그리기 경로는 다음 텍스트 갱신이나 화면 재진입이 필요할 수 있습니다.

설정은 게임 종료 후 `scripts/AigisKR.ini`에서 변경하고 재시작합니다.

```ini
[AutoTranslate]
Enabled=1
LiveTextRefresh=1
HdTransMode=Auto
HdTransPath=
```

- `Enabled=0`: 자동번역을 끕니다. 수동 검수 번역·폰트·이미지 패치는 유지합니다.
- `LiveTextRefresh=0`: 번역 결과를 다음 정상 텍스트 갱신에 적용합니다.
- 외부 hdTrans를 사용하려면 `HdTransPath`에 설치 폴더를 지정합니다. 기본 `Auto`는 유효한 외부 경로를 우선 사용하고 실패하면 번들을 사용합니다.
- 사용자 번역 수정은 `scripts/AigisKR/data/overrides.json`으로 관리할 수 있습니다.

## 번역 범위

주요 메뉴·버튼·텍스트 이미지와 요일·연습·챌린지·전술 지도 배너를 포함합니다. 이번 버전의 현재 이벤트·소환·상품 배너도 반영했습니다. 모든 서버 이벤트 일러스트와 이미지가 한국어화된 것은 아닙니다. 기계번역 문장에는 어색한 표현이 남을 수 있습니다.

잠금 전장 변형과 별도 홈 배너는 번역 파일 및 로딩까지 확인했고 개별 표시 상태는 보류했습니다. 이전에 확인하지 못한 이벤트·특수 스킬 상태도 알려진 검수 범위로 기록했습니다. 자세한 내용은 [제한 사항](KNOWN_LIMITATIONS.md)을 참조하세요.

## 제거

게임을 종료한 뒤 배포 ZIP의 파일 목록에 해당하는 **AigisKR 추가 파일만** 제거합니다. `Aigis_r.exe`, `.cache`와 게임의 다른 파일은 유지하세요. 다른 모드와 `winmm.dll`·`scripts`를 공유한다면 그 모드의 파일을 구분해야 합니다.

자신의 번역과 설정을 보관하려면 `scripts/AigisKR/cache/hdtrans_cache.jsonl`, `scripts/AigisKR/data/overrides.json`, `scripts/AigisKR.ini`를 먼저 백업합니다.

## 문제 보고

게임 폴더의 `AigisKR_Diagnostics.cmd`를 실행하면 `AigisKR_Support_*.zip`이 생성됩니다. 파일을 확인한 뒤 [이슈](https://github.com/kekwkekw/aigiskr/issues)에 증상과 진입 경로를 함께 첨부하세요.

지원 번들은 설정·파일 해시·상태·로그를 수집하며, 사용자 이름과 계정 식별자를 마스킹합니다. 게임 캐시·자동번역 캐시 본문·DMM 인증 정보·스크린샷은 수집하지 않습니다.

## 라이선스와 대응 소스

Copyright (c) 2026 kekwkekw. 이 저장소의 자체 코드·도구·독자 작성 번역 데이터는 **AGPL-3.0-only**로 제공합니다. [LICENSE](LICENSE)를 참고하세요. 게임 원문·고유 명칭 및 가공 UI 이미지의 원작 권리는 각 게임 권리자에게 있으며, 이 저장소의 오픈소스 라이선스로 재허가하지 않습니다.

동반 배포한 **`AigisKR-0.1.0-sources.zip`**에는 번들 hdTrans와 별도 번역 호스트의 대응 소스 및 빌드 안내가 있습니다. 이 소스 ZIP은 게임 설치용 파일이 아닙니다.

`hdTrans`와 `AigisKR.HdTransHost`는 AGPL-3.0-only 구성 요소이며, 그 밖의 외부 구성요소는 각각의 고유 라이선스를 따릅니다. 라이선스·고지·출처는 `LICENSES/` 및 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)에 있습니다. 폰트 파일은 배포하지 않으며 실행 시 Windows 시스템 폰트를 사용합니다.

패키지 버전은 **v0.1.0**입니다. rc.11에서 검증한 바이너리를 보존했으므로 진단 로그의 내부 구성 요소 버전은 ASI `0.1.0-rc.9`, 번역 호스트 `0.1.0-rc.2`로 표시될 수 있습니다.
