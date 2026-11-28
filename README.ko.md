# VSFilterMod — 크로스플랫폼 버전

[![CI](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml/badge.svg)](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | **한국어**

VSFilterMod 자막 렌더러를 **Windows, Linux, macOS**로 이식한 프로젝트입니다.
빌드 결과물은 하나의 라이브러리로, Aegisub용 **CSRI** 플러그인,
**AviSynth+** 플러그인(`TextSubMod` / `MaskSubMod`), **VapourSynth**
플러그인(`vsfm.TextSubMod`)으로 동작합니다. 또한 커맨드라인 도구
`vsfmrender`도 포함되어 자막을 Blu-ray 제작용 **BDN+XML**로 바로
렌더링할 수 있습니다.

ASS 파서와 래스터라이저는 VSFilterMod 원본 코드이므로 모든 확장 태그를
그대로 사용할 수 있습니다: `\pngimg` 이미지 배경, `\glow` 글로우, 그라디언트,
이동 가능한 벡터 clip, `\rnd` 지터, distort, `\frs` 문자 회전, Z 좌표,
세로 간격 등.

## 목차

- [특징](#특징)
- [다운로드](#다운로드)
- [사용 방법](#사용-방법)
- [빌드](#빌드)
- [원본과의 일치 정도](#원본과의-일치-정도)
- [원본과의 차이점](#원본과의-차이점)
- [알려진 문제](#알려진-문제)
- [감사와 라이선스](#감사와-라이선스)

## 특징

- **세 플랫폼에서 동일한 렌더러.** 글리프는 모두 FreeType이 제공하므로
  같은 자막이 Windows, Linux, macOS에서 동일한 결과를 냅니다.
- **원본 동작을 재현하고 실측으로 검증.** VSFilterMod의 조판이 GDI에 의존하는
  부분——셀 높이 기준 글꼴 크기 결정, 합성 볼드·이탤릭, `\frs`, `@` 세로쓰기
  글꼴, 글리프 폴백——은 엔진이 GDI의 실제 동작을 재현합니다. 이 규칙들은
  GDI를 직접 계측해 얻은 것으로, 원본과 픽셀 단위로 비교 검증되었습니다.
- **Aegisub 플러그인**(CSRI). 실시간 미리보기용.
- **AviSynth+ 플러그인**. `TextSubMod` / `MaskSubMod`의 인수는 원본과
  동일하며 기존 스크립트와 avs2bdnxml을 그대로 사용할 수 있습니다.
- **VapourSynth 플러그인**(API 4). `vsfm.TextSubMod`의 인수는 원본과 동일:
  8비트 출력은 원본과 같고, 9~16비트 YUV와 RGB는 최대 정밀도로 혼합되며,
  4:2:2와 그레이스케일도 지원합니다.
- **BDN+XML 출력**: `vsfmrender`의 결과는 avs2bdnxml이 이 플러그인의
  `MaskSubMod`를 호출한 결과와 바이트 단위로 동일합니다.
- **내장 글꼴**(`[Fonts]` 첨부)을 CSRI로도 사용할 수 있습니다.
- **출력이 머신에 의존하지 않습니다.** DPI와 코드 페이지가 고정되어 있으며
  로컬 설정을 읽지 않습니다.

## 다운로드

푸시할 때마다 GitHub Actions가 모든 플랫폼에서 자동으로 빌드합니다.
버전 태그가 붙은 정식 릴리스는
[Releases](https://github.com/cgyihai/VSFilterMod/releases) 페이지에
공개되며, 임의 커밋의 빌드 결과물은 [Actions](https://github.com/cgyihai/VSFilterMod/actions)
페이지의 해당 실행의 *Artifacts*에서 다운로드할 수 있습니다.

| 플랫폼 | 패키지 | 내용 |
| --- | --- | --- |
| Windows x86_64 | `vsfiltermod-windows-x86_64-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` |
| Windows x86(32비트) | `vsfiltermod-windows-x86-msvc` | `vsfiltermod.dll`, `vsfmrender.exe`. 32비트 Aegisub, AviSynth+, avs2bdnxml용 |
| Windows ARM64 | `vsfiltermod-windows-arm64-msvc` | `vsfiltermod.dll`, `vsfmrender.exe` |
| Linux x86_64 | `vsfiltermod-linux-x86_64` | `vsfiltermod.so`, `vsfmrender` |
| Linux aarch64 | `vsfiltermod-linux-aarch64` | `vsfiltermod.so`, `vsfmrender` |
| macOS 유니버설 | `vsfiltermod-macos-universal` | `vsfiltermod.dylib`, `vsfmrender`. Apple Silicon과 Intel 통합 |
| macOS Apple Silicon | `vsfiltermod-macos-arm64` | `vsfiltermod.dylib`, `vsfmrender` |
| macOS Intel | `vsfiltermod-macos-x86_64` | `vsfiltermod.dylib`, `vsfmrender` |

모든 빌드는 동일한 고정 버전의 FreeType과 libpng를 정적으로 링크하므로
렌더링 결과가 완전히 동일합니다. Windows 버전은 추가 DLL이 필요 없습니다.
macOS 버전은 시스템 외 라이브러리에 의존하지 않으며 macOS 11 이상
(Apple Silicon)과 10.15 이상(Intel)을 지원합니다. Linux 버전은 fontconfig와
zlib(모든 데스크톱 배포판에 존재)에만 의존하며 glibc 2.35 이상
(Ubuntu 22.04, Debian 12, Fedora 36 이상)을 지원합니다. 각 릴리스에는
`SHA256SUMS` 검증 파일이 포함됩니다.

## 사용 방법

### Aegisub

**Windows:** `vsfiltermod.dll`을 `aegisub.exe` 옆의 `csri` 폴더에 넣고
Aegisub를 재시작한 뒤 *옵션 → 고급 → 비디오 → 자막 렌더러*에서
**CSRI/vsfiltermod_textsub**를 선택합니다.

**Linux / macOS:** 공식 Aegisub는 이 플랫폼들에서 CSRI 플러그인을
로드하지 않습니다. 먼저 Aegisub에
[`docs/aegisub-csri-posix.patch`](docs/aegisub-csri-posix.patch)
를 적용해 다시 빌드한 다음:

```bash
mkdir -p ~/.csri
cp vsfiltermod.so ~/.csri/          # macOS에서는 vsfiltermod.dylib
chmod +x ~/.csri/vsfiltermod.so     # 로더는 실행 권한이 없는 파일을 건너뜀
```

자세한 내용은 [BUILD.ko.md](BUILD.ko.md#aegisub에-설치)를 참조하세요.

### AviSynth+와 avs2bdnxml

같은 라이브러리가 AviSynth+ 플러그인으로도 동작합니다:

```avs
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subtitle.ass", 1920, 1080, 23.976, 3000)   # RGBA 출력, avs2bdnxml용
# 또는 비디오 위에 오버레이:
# TextSubMod("subtitle.ass")
```

AviSynth+로 스크립트를 로드하는 모든 avs2bdnxml에서 사용할 수 있습니다.
32비트 avs2bdnxml 또는 AviSynth+에는 32비트(`windows-x86`) 버전을,
그 외에는 64비트 버전을 사용하세요.

### VapourSynth

같은 라이브러리가 VapourSynth 플러그인(API 4, VapourSynth R55 이상)으로도
동작하며, 네임스페이스와 인수는 원본과 동일합니다:

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("vsfiltermod.dll")      # 플러그인 디렉터리에 넣었다면 생략 가능
clip = core.vsfm.TextSubMod(clip, "subtitle.ass")
# TextSubMod(clip, file, charset=1, fps=..., vfr="timecodes.txt")
```

4:2:0 또는 4:2:2 YUV, 그레이스케일, RGB를 샘플당 8~16비트로 지원합니다.
YUV420P8과 RGB24의 출력은 원본 필터와 완전히 동일합니다. 원본은 10비트와
16비트 클립을 두 개의 8비트 절반으로 나눠 그렸지만, 여기서는 각 비트 깊이를
그 정밀도로 혼합하므로 `accurate=`는 여전히 전달할 수 있지만 더 이상 필요하지
않습니다. 4:4:4는 먼저 RGB로 변환하세요.

### vsfmrender

AviSynth 없이 자막을 바로 BDN+XML로 렌더링합니다:

```bash
vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

| 옵션 | 의미 |
| --- | --- |
| `-s, --size WxH` | 프레임 크기(기본값 `1920x1080`) |
| `-f, --fps RATE` | `23.976`, `24`, `25`, `29.97`, `50`, `59.94` 또는 정확한 분수 |
| `-l, --lang CODE` | ISO 639-2 언어 코드(기본값 `und`) |
| `-t, --title TEXT` | 인덱스에 기록할 제목 |
| `--first N` / `--last N` | 내보낼 프레임 범위 |
| `--no-crop` | 전체 프레임 이미지 출력(avs2bdnxml의 `-a0`와 동일) |
| `--fontdir DIR` | `DIR`(하위 디렉터리 포함)의 글꼴을 설치된 글꼴보다 우선 사용. 반복 지정 가능 |
| `--font-report` | 스크립트가 사용하는 각 글꼴이 실제로 어떤 글꼴 파일로 해석되었는지 나열 |
| `--strict-fonts` | 글꼴이 하나라도 대체되면 종료 코드 3으로 실패 |
| `-q, --quiet` | 진행 상황을 표시하지 않음 |

### 에디터 통합(태그 메타데이터)

자막 에디터는 에디터의 동기 릴리스 없이 두 가지 동등한 채널로 이 렌더러의
확장 태그(`\glow`, `\imgscale`, `\matte` 등)를 알아낼 수 있습니다:

- **파일**: 빌드할 때마다 렌더러 출력 디렉터리에 `vsfiltermod-tags.json`이
  생성됩니다(Windows에서는 `VSFilterMod.dll` 옆). 파일은 UTF-8 JSON 문서
  `{ "version": 1, "renderer": ..., "tags": [...] }`이며, 각 태그는 `name`,
  `signature`, 한 줄 `help`를 가집니다. 에디터는 일반 파일 IO로 태그 배열을
  자동 완성에 통합할 수 있습니다.
- **플러그인 익스포트**: 라이브러리는
  `const char *csri_typesub_tags_json(void)`도 익스포트하며 동일한 문서를
  반환합니다. 플러그인을 로드한 후 `GetProcAddress`/`dlsym`으로 심볼을
  찾는 방식을 선호하는 에디터용입니다.

두 채널의 내용은 바이트 단위로 동일하며(테스트 스위트로 검증), 에디터는
편한 쪽을 **한 번만** 구현하면 됩니다. 이후 새 렌더러 빌드에서 태그가
추가·갱신되어도 새 바이너리(또는 새 JSON 파일)로 교체하기만 하면 되고,
에디터 측 갱신은 전혀 필요 없습니다. 알 수 없거나 누락된 메타데이터는
그냥 무시하면 됩니다. 파일과 익스포트 심볼은 모두 선택 사항이며,
`version` 필드는 향후 포맷 확장을 위해 마련되어 있습니다.

### 이펙트 패키지(\glowc, \glowa, \iglow, \mblur, \bevel, \clipf)

\glow의 선택적 동반자와 4개의 새 이펙트. 모두 \t 애니메이션을 지원하고
값 0이면 비활성화됩니다:

```ass
{\glowc&HBBGGRR&}    글로우 색상 오버라이드(기본값: 기본 색상을 따름)
{\glowa&HAA&}        글로우 불투명도 오버라이드(ASS 관례: 00 불투명, FF 투명)
{\iglow8}            내부 글로우 반경 8: 글리프 가장자리에서 안쪽으로 빛이 스밈
{\iglow(8,1.5)}      강도 지정. \iglowc / \iglowa로 색상/불투명도 오버라이드(기본 흰색)
{\mblur45,10}        방향성 모션 블러: 45도 방향으로 10 출력 픽셀 잔상
{\bevel2}            부드러운 베벨/엠보스, 크기 2
{\clipf15}           페더 클립 반경 15: 사각형, 벡터, 반전 클립 모두
```

\iglow는 본문 뒤에 합성되며 글리프 내부에 엄격히 제한되어 밖으로 새지
않습니다. \mblur는 완성된 비트맵을 결정론적 정수 샘플링으로 처리합니다.
\clipf는 해당 줄의 클립을 부드럽게 합니다: 사각형 클립은 래스터 영역을 바깥으로
넓히고 명목 사각형 밖에서 페이드됩니다. 벡터 클립 마스크는 파싱 완료 후 한 번만
블러되며, 모든 그리기와 글로우 경로가 자동으로 부드러운 가장자리를 상속합니다.
회귀 테스트 tests/fxpackage.py가 모든 이펙트를 다룹니다.

### 그라디언트(\1vc, \vgrad, \rgrad)

기존 \1vc/\2vc/\3vc/\4vc 태그는 각 단어에서 네 모서리 색상을 쌍선형
보간합니다. 두 가지 수식 태그로 보간 방식을 바꿀 수 있습니다. 둘 다 vc의
처음 두 색상을 시작·끝 색으로 사용하고, \t 애니메이션을 지원하며 상호
배타적입니다——나중에 쓴 태그가 적용됩니다:

```ass
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\vgrad45}45도 방향 선형 그라디언트
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\rgrad1}중앙 방사형 그라디언트
{\rgrad(1.5,0.5,0.2)}반경 1.5배, 중심은 너비 50% / 높이 20% 위치
{\rgrad0}쌍선형 모서리 혼합으로 복귀
```

\vgrad(angle)는 글리프 경계 상자를 지정 각도 방향(0은 왼쪽에서 오른쪽,
90은 위에서 아래)으로 투영합니다. \rgrad(scale[,cx,cy])는 중심점에서 바깥으로
방사합니다: 첫 번째 vc 색상이 중심에 위치하고 선형 거리에 따라 두 번째 색상으로
전환되며, 반경 상한은 중심에서 경계 상자의 가장 먼 모서리까지 거리의 scale배입니다.
cx/cy는 화면 방향의 UV 비율로 중심을 정하고 기본값은 상자 중심입니다. 회귀
테스트 tests/vgrad.py와 tests/rgrad.py가 이 의미를 고정합니다.

### 원호 텍스트

`\arc(radius[,angle])`는 줄 기준선을 줄 앵커(위치 지정 줄은 `\pos` 점)를
중심으로 하는 `radius` 출력 픽셀 원주에 감습니다. angle이 0이면 줄 중심이
원 꼭짓점에 오고 글자는 시계 방향으로 배열됩니다: 짧은 텍스트는 무지개
아치 모양이 되고, 긴 텍스트는 원반처럼 한 바퀴를 돌 수 있습니다. 반경이
양수면 글자가 원 바깥쪽에, 음수면 안쪽에 옵니다. `\arc120`은 약식이고
`\t` 애니메이션을 지원하며 `\arc0`으로 끕니다. 조판이 문자 단위로
분리되므로 라틴 리거처와 커닝은 비활성화됩니다(CJK는 영향 없음).
`\fsp` 자간은 원호를 따라 그대로 적용됩니다.

### `\wave(amp,freq[,phase])` — 사인파 텍스트

줄 기준선을 사인파에 감습니다: 각 글리프가 전진 위치에 따라 `amp*sin`
수직 변위를 받고 파동의 접선 방향으로 회전합니다(변위된 기준선 중점이
피벗). `amp`/`freq`는 출력 픽셀(`freq`는 전체 파장), `phase`는 도입니다.
`\wave0`으로 끕니다. `\t`는 세 매개변수 모두를 애니메이션할 수 있습니다
(`phase`를 애니메이션하면 파동이 흐릅니다). `\arc`와 마찬가지로 문자
단위로 분리되며(라틴 리거처 비활성화, CJK 영향 없음), `\arc`와 모든
비트맵 이펙트와 조합할 수 있습니다.

### 트랙 매트(\matte, \imatte)

트랙 매트는 한 이벤트의 실루엣으로 그 위 레이어의 이벤트를 마스킹하는
After Effects 방식입니다. 매트 이벤트 자체는 숨겨집니다. 대상 레이어의
이벤트는 완전히 렌더링되지만——본문, 아웃라인, 섀도, 글로우와 모든
후처리 이펙트——매트가 불투명한 곳에서만 보입니다:

```ass
Dialogue: 0,...,{\pos(320,180)}TEXT        보이는 베이스 레이어
Dialogue: 1,...,{\pos(320,180)\matte}TEXT  숨겨진 매트 사본, 레이어 2를 마스킹
Dialogue: 2,...,{\pos(320,180)\1vc(...)\rgrad1}WWWWW  그라디언트는 TEXT 내부에만 표시
```

- `\matte` / `\matte1` / `\matte(1)`은 layer+1층을 마스킹합니다.
  `\matte2`는 layer+2층(드문 레이어 번호용). `\matte0`으로 비활성화.
- `\imatte`는 반전: 매트 바깥쪽에 이펙트가 표시됩니다.
- 같은 레이어의 여러 매트 이벤트는 합집합으로 하나의 마스크가 됩니다.
- 마스크는 매트의 렌더링된 실루엣에서 가져옵니다(섀도, 아웃라인, 본문 포함).
  매트 위의 블러나 글로우는 마스크 가장자리를 그에 맞게 부드럽게 합니다.
- 매트와 피매트 이벤트는 각각 프레임별로 애니메이션되며 \move/\t 양쪽이
  동기를 유지합니다.

회귀 테스트 tests/matte.py는 포함, 반전, 레이어 오프셋, 숨김, 문법 변형을
다룹니다.

### `\extrude(depth[,angle])` — 입체 돌출

돌출 방향을 따라 1px 간격으로 섀도 색상 사본을 그려(기본값은 화면 좌표
45도) 실체 측면을 형성합니다. 색상/불투명도는 섀도 스타일(`\4c`/`\4a`)에서
가져오며, 섀도 깊이가 0이어도 독립적으로 사용할 수 있습니다. `\extrude0`으로
끕니다. `\t`는 depth와 angle을 애니메이션합니다.

### `\echo(count,dx,dy[,falloff])` — 잔상 트레일

완전히 렌더링된 이벤트를 단계별 정수 오프셋으로 사본마다 합성하고, alpha를
레이어마다 감쇠시킵니다(기본값 0.5). 메인 사본은 마지막에 합성됩니다.
글로우, 그라디언트, 블러 등 모든 이펙트가 정확히 포착됩니다. count 상한은
32입니다. `\echo0`으로 끕니다. `\t`는 모든 매개변수를 애니메이션합니다.
이벤트 레벨: 줄에는 마지막 오버라이드 블록의 매개변수가 적용됩니다.

### `\persp(dist)` — 실제 원근 카메라 거리

`\frx`/`\fry` 3D 회전의 카메라 거리(출력 픽셀). 작은 거리는 강한 원근
단축을 만들고(가까운 쪽이 확대, 먼 쪽이 압축), 큰 거리는 기존 ASS의
직교 투영에 가까워집니다. `\persp0`으로 기존 투영을 복원합니다.
`\t` 애니메이션 지원.

### 줄 연속 이미지 채우기와 `\imgscale(sx[,sy])`

`\1img`–`\4img` PNG 채우기는 이제 줄 수준의 화면 좌표로 샘플링하며, 각
단어의 오버레이 경계 상자에서 텍스처가 다시 시작되지 않습니다. 타일 이미지는
줄 안의 모든 단어를 가로질러 연속적으로 흐르고, 사각형 클립에서도 앵커가
안정적입니다. 기존 이미지 인수 순서 `file,xoffset,yoffset[,angle]`은
변경되지 않았습니다.

`\imgscale(sx[,sy])`는 래핑 전에 텍스처 샘플 좌표를 나눕니다: `2`는 텍스처를
2배 확대, `0.5`는 2배 축소합니다. `sy`를 생략하면 두 축에 `sx`가 사용됩니다.
음수는 절댓값을 취하고, 0이나 잘못된 입력은 1:1로 복원되며, 값은
[1/256,256]으로 클램프됩니다. `\imgscale0`은 간결한 리셋 형태이며
`\t` 애니메이션을 지원합니다.

이미지 저장과 샘플링은 출력 픽셀을 바꾸지 않고 최적화되었습니다: 디코딩된
PNG 행은 하나의 연속 메모리를 공유하고, 텍스처 좌표는 오버레이 행/열마다
캐시되며, 정수 좌표나 단일 축 보간은 수학적으로 동등한 고속 경로를 사용합니다.

### `\shadblur(radius)` / `\bordblur(radius)` — 레이어별 블러

단일 레이어에만 작용하는 가우시안 블러: 섀도/아웃라인 패스가 해당 레이어
반경을 기본 `\blur`에 더해 오버레이를 다시 래스터화합니다. 본문은 선명한
채로 섀도가 퍼지거나, 아웃라인이 부드러운 채로 채우기가 선명하게 할 수
있습니다. 간결한 표기 `\shadblur4`, `\t` 애니메이션 지원, 0이면 비활성화.

### `\mgrad(c1,c2[,c3,...])` — 멀티스톱 그라디언트

2~8개의 ASS 16진수 색상 스톱을 현재 그라디언트 방향을 따라 균등 배치합니다:
`\vgrad` 각도나 `\rgrad` 방사가 설정되어 있으면 그 방향을 따르고, 없으면
수평입니다. 구간별 선형 보간이 본문 채널에 작용합니다. `\mgrad0`으로 끕니다.

### `\pin(x1,y1,x2,y2,x3,y3,x4,y4)` — 코너 핀 원근

줄을 임의의 4점 사각형에 맞추는 투영(호모그래피) 워프: 줄 레이아웃 상자의
네 모서리가 왼쪽 위부터 시계 방향으로 지정된 네 개의 출력 픽셀 모서리 점에
매핑됩니다. 텍스트를 기울어진 간판, 화면, 책 표지에 실제 원근 단축으로
고정할 수 있습니다. 워프는 다른 모든 변형 위에 적용됩니다(회전, 원호, 파동은
그 아래에서 합성). `\pin0`으로 끕니다. `\t`는 8개 좌표 모두를
애니메이션합니다. 기존 `\distort`(3점 쌍선형, 상자 상대)와 달리 `\pin`은
절대 출력 픽셀의 실제 원근입니다.

### RGB 색수차 분리(\rgb)

\rgb 태그는 글자의 빨간색과 파란색 채널을 반대 방향으로 어긋나게 하여
색수차/글리치 효과를 냅니다. 다음 두 가지 쓰기는 동등합니다:

```ass
{\rgb4}텍스트
{\rgb(4)}텍스트
```

거리는 출력 픽셀 단위이며, 음수로 가장자리 색 방향을 바꿀 수 있습니다.
\t 애니메이션을 지원하고 상한은 4096이며 \rgb0으로 끕니다. 효과는 문장
전체에 작용하며 각 단어의 값이 다르면 절댓값이 가장 큰 것이 사용됩니다.

### 텍스트 글로우(\glow)

\glow는 글자 뒤에 부드러운 광륜을 그립니다. 다음 두 가지 쓰기는 동등합니다:

```ass
{\glow20}텍스트
{\glow(20)}텍스트
```

두 번째 값은 선형 강도 배율입니다(기본값 `1`):

```ass
{\glow(20,1.5)}더 강한 글로우
{\glow0}글로우 끄기
```

반경은 출력 프레임의 픽셀로 측정됩니다(\blur와 동일). 글로우는 기본적으로
글자 본문 색상을 사용합니다. 그라디언트와 이미지 채우기는 자체 채우기 색상을
유지하고, 아웃라인은 글로우 모양만 제공하며, 섀도는 글로우에 참여하지 않습니다.
글자 불투명도, \fade/\fad, 사각형/벡터/이동 \clip/\iclip, \t 애니메이션은
모두 글로우에 정확히 영향을 줍니다. 반경 상한은 4096 출력 픽셀, 강도 배율
상한은 64입니다.

글로우는 블룸식 멀티스케일 알고리즘을 사용합니다: 발광 형태를 세 가지 반경
(조밀한 밝은 코어, 중간, 넓고 부드러운 외곽 광륜)으로 가우스 근사 블러하고
16bit 선형 광 파이프라인에서 가산 누적한 뒤 Reinhard 톤 곡선으로 압축하므로,
광륜이 죽은 흰색 덩어리로 포화되지 않고 부드러운 밝기 그라디언트를 유지합니다.
강도 배율은 노출에 해당하며 톤 매핑 전에 선형 광량을 스케일합니다. 이것은
여전히 알파 블렌딩 래스터 이펙트이며, After Effects Deep Glow의 진정한 HDR
가산 복제는 아닙니다.

### 모든 머신에서 같은 글꼴 사용하기

스크립트가 지정한 글꼴이 설치되어 있지 않으면 플랫폼이 자동으로 대체합니다
——Windows, macOS, Linux 각각 다른 글꼴로. 세 가지 회피 방법이 있습니다:

- **글꼴을 스크립트에 내장**(`[Fonts]`. 예: Aegisub의 "첨부" 기능).
- **글꼴 폴더 공유.** 그 안의 글꼴은 내장 글꼴처럼 검색되며 설치된 글꼴보다
  우선합니다. vsfmrender는 `--fontdir DIR`, Aegisub와 AviSynth는 환경 변수
  `VSFM_FONT_DIRS`(여러 폴더는 Windows에서는 `;`, 다른 플랫폼에서는 `:`로 구분).
- **확인.** vsfmrender는 렌더링 후 대체된 글꼴을 모두 나열하며,
  `--strict-fonts`를 붙이면 오류로 처리합니다. Aegisub와 AviSynth에서는
  `VSFM_FONT_LOG=1`을 설정하면 각 글꼴 해석 시 결과를 출력합니다.

## 빌드

```bash
meson setup build
meson compile -C build
meson test -C build
```

필요한 것: Meson ≥ 0.59, Ninja, C++17 지원 컴파일러, FreeType ≥ 2.8,
libpng ≥ 1.6. Linux에서는 fontconfig도 필요합니다. Windows에서는
FreeType, libpng, zlib가 Meson 서브프로젝트로 자동 다운로드됩니다.
전체 설명, 빌드 옵션, CI 매트릭스는 [BUILD.ko.md](BUILD.ko.md)를 참조하세요.

## 원본과의 일치 정도

Windows에서 원본 VSFilterMod의 CSRI 플러그인과 비교하여 양쪽 글리프
커버리지 영역의 일치율(IoU)을 이벤트별로 계산합니다. 1.000은 픽셀 완전
일치를 의미합니다.

| 테스트 스크립트 | 커버 내용 | IoU |
| --- | --- | --- |
| `tests/smoke.ass` | 기본 스타일과 배치 | 1.000 |
| `tests/frs.ass` | 각 각도의 `\frs`. CJK 문자, `\fsp`, 볼드, 이탤릭, 줄바꿈 | 0.977–0.999 |
| `tests/vertical.ass` | `@` 세로쓰기 글꼴. 문장 부호, 병음, 그리스 문자, 전각 문자 | 0.967–1.000 |
| `tests/fallback.ass` | 라틴 글꼴의 CJK·한글 문자(글리프 폴백) | 0.985–0.999 |
| `tests/bold.ass` | 합성 볼드. 이탤릭, `\frs`, 폴백 포함 | 0.977–1.000 |

이벤트 타이밍은 모두 `MaskSubMod`의 클록(단정밀도 반올림 포함)으로
계산되므로 BDN+XML의 각 이벤트의 시작·종료 프레임은 원본과 동일합니다.
남은 차이는 글리프 가장자리의 앤티앨리어싱으로, 1픽셀 이내입니다.

**플랫폼 간 출력은 "비슷한" 것이 아니라 완전히 동일합니다.** 글꼴 파일이
같다면 Windows, Linux, macOS, x86-64와 ARM의 모든 빌드가 같은 픽셀을
출력합니다. 렌더링에 SIMD, CPU, 컴파일러에 의존하는 코드 경로가 없기
때문입니다. CI는 모든 플랫폼에서 `tests/consistency/`를 렌더링하고,
공통의 `expected.txt`와 다른 픽셀이 있으면 실패합니다. 머신 간에 여전히
달라질 수 있는 것은 설치된 글꼴뿐입니다("알려진 문제" 참조).

## 원본과의 차이점

**글리프는 GDI가 아닌 FreeType에서 옵니다.** 조판 코드가 GDI에 의존하는
부분은 엔진이 모두 에뮬레이션합니다: `LOGFONT`의 양의 높이는 em 크기가 아닌
셀 높이입니다(이것이 바로 다른 렌더러에서 VSFilter 자막 글꼴 크기가 틀어지는
전형적인 원인). `\frs`에서 GDI는 글꼴 메트릭스를 바꾸고 비공개 규칙으로
글리프를 배치·회전·측정합니다. `@` 글꼴은 CJK 글리프를 옆으로 눕히고
GSUB `vert` 세로쓰기 글리프로 전환합니다. 이 모든 것이 재현되어 있습니다
(자세한 내용은 `src/font/font_engine.cpp`). 이름으로 글꼴을 찾는 것은
계속 각 플랫폼 네이티브 API를 사용합니다: DirectWrite, CoreText, fontconfig.

**글리프 폴백은 GDI와 일치합니다.** 스타일 글꼴에 없는 문자는 해당 글꼴의
Windows SystemLink 목록, 宋体와 Malgun Gothic, macOS와 Linux의 CJK 글꼴,
플랫폼 자체의 폴백 메커니즘 순서로 가져옵니다. 폴백 글리프의 크기, 기준선,
GDI가 이런 문자에 적용하는 자간 조정은 모두 GDI와 동일합니다.

**합성 볼드는 GDI 규칙으로 구현**되어 회전한 글자에서 더 굵은 획을 포함해
포인트별로 계측되었습니다.

**내장 글꼴을 사용할 수 있습니다**. CSRI로도 모든 플랫폼에서: 첨부 글꼴은
FreeType에 바로 전달되고 시스템에 설치되지 않습니다. GDI가 글꼴을 찾을 수
있는 이름이면 여기서도 찾을 수 있습니다: Windows 패밀리명
(예: "Source Han Sans SC Medium"), 타이포그래피 패밀리명, 전체 이름, 모든
언어의 로컬라이즈된 이름(구식 CJK 글꼴의 GBK/Big5/Shift-JIS 인코딩 이름 포함).
같은 패밀리에 여러 웨이트가 내장된 경우 GDI처럼 요청된 웨이트와 이탤릭에 맞는
글꼴 파일이 선택됩니다. 내장 가변 글꼴의 명명된 인스턴스도 GDI 방식으로
열거됩니다(예: "Family Black"). 일반 폴백 글꼴에도 없는 문자는 플랫폼 자체의
폴백 전에 내장 글꼴(과 `--fontdir` 폴더)을 먼저 검색하므로 어떤 머신에서도
결과가 같습니다.

**DirectShow 부분을 제거했습니다.** DirectShow 필터와 그 프로퍼티 페이지,
VobSub/DVB/HDMV/CC/USF/SSF/RealText 파서, DirectX 자막 레이어 백엔드가
제거되었습니다. Aegisub, AviSynth, BDN+XML 출력 모두에 불필요합니다.
AviSynth와 VapourSynth의 `VobSub()`는 모두 이식되지 않았습니다.

**VapourSynth 플러그인은 API 4 기반**(원본은 폐기된 API 3)이며, 고비트
깊이는 최대 정밀도로 혼합되고 샘플을 반으로 나누지 않습니다.

**렌더링 결과는 머신에 의존하지 않습니다.** DPI는 96으로 고정되고 문자
세트에서 코드 페이지로의 매핑은 고정 테이블을 사용합니다. 유일한 예외는
Windows에서의 글리프 폴백으로, GDI와 마찬가지로 해당 머신의 SystemLink
설정을 따릅니다.

## 알려진 문제

- OpenType-CFF 형식의 `@` 세로쓰기 글꼴(예: `@Source Han Sans`)은 여기서는
  해당 글꼴로 렌더링됩니다. 원본의 GDI는 CFF 글꼴을 세로쓰기로 열지 못하고
  몰래 시스템 글꼴로 바꿉니다(중국어 Windows에서는 宋体). 이것을 그대로
  흉내 내면 출력이 Windows 언어 설정에 의존하게 됩니다.
- `@MS Gothic`과 `@Malgun Gothic`은 그리스 문자와 키릴 문자를 회전하지
  않지만, 이 포트는 옆으로 눕힙니다. 또한 宋体와 黑体는 `\frs`에서의 가나
  간격이 다른 글꼴과 다릅니다. 이 두 가지는 재현 불가능한 GDI 내부
  테이블 때문입니다.
- `@Arial`처럼 GDI가 인식하지 못하는 글꼴 이름은 GDI가 줄 전체(라틴 문자
  포함)를 宋体로 바꾸게 하지만, 여기서는 라틴 문자가 스타일 글꼴을 유지합니다.
- Linux와 macOS 빌드는 Windows 빌드와 픽셀 단위로 일치합니다(CI로 검증).
  다만 스크립트가 지정한 글꼴이 설치되어 있지 않으면 각 플랫폼이 각기 다른
  글꼴로 대체합니다. 글꼴을 스크립트에 내장하면(`[Fonts]`) 피할 수 있습니다.

## 감사와 라이선스

Gabest(Guliverkli)의 **VSFilter**와 **VSFilterMod**를 기반으로 합니다.
`include/avisynth`의 AviSynth+ C API 헤더는 AviSynth+ 프로젝트에서,
`include/vapoursynth/VapourSynth4.h`는 VapourSynth(LGPL 2.1)에서
가져왔습니다.

원본과 동일하게 [GNU 일반 공중 사용 허가서 2.0 이상](LICENSE)으로
제공됩니다.
