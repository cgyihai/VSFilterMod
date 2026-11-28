# VSFilterMod 빌드

[English](BUILD.md) | [简体中文](BUILD.zh-CN.md) | [日本語](BUILD.ja.md) | **한국어**

이 프로젝트의 빌드 결과물은 두 가지입니다:

| 결과물 | 설명 |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | **CSRI** 자막 렌더링 플러그인. Aegisub는 모든 플랫폼에서 이 형식으로 로드합니다. **AviSynth+** 플러그인(`TextSubMod` / `MaskSubMod`)과 **VapourSynth** 플러그인(`vsfm.TextSubMod`)이기도 합니다. |
| `vsfmrender` | 커맨드라인 도구. 자막을 **BDN+XML**(인덱스 파일과 이벤트별 PNG)로 렌더링합니다. |

둘은 같은 렌더러 소스를 사용하므로, BDN+XML로 내보낸 자막은 Aegisub
미리보기와 픽셀 단위로 같습니다.

## 환경 요구 사항

* Meson ≥ 0.59와 Ninja
* C++17 지원 컴파일러(GCC 9+, Clang 10+, MSVC 2019+)
* FreeType ≥ 2.8과 libpng ≥ 1.6: 시스템에 있으면 그대로 사용하고, 없으면
  Meson 서브프로젝트로 자동 다운로드
* Linux/BSD만: **fontconfig** ≥ 2.11
* macOS와 Windows는 각각 CoreText와 DirectWrite를 사용. 추가 설치 불필요

## 빌드

```bash
meson setup build
ninja -C build
meson test -C build
```

자주 쓰는 옵션:

```bash
meson setup build -Dfont_backend=fontconfig   # 플랫폼 기본 글꼴 백엔드 덮어쓰기
meson setup build -Dbdnxml=false              # 플러그인만 빌드
meson setup build -Dtests=false
```

### 모든 플랫폼에서 출력 일치

렌더링 코드는 순수한 이식 가능 C++입니다: SIMD 지원, CPU 벤더, 컴파일러에
의존하는 코드 경로가 없습니다. 빌드 시 `-ffp-contract=off`가 전달되어 GCC와
Clang이 `a * b + c`를 FMA 명령으로 합치는 것을 금지합니다(모든 AArch64 CPU는
FMA를 가지며 그 반올림 방식이 다릅니다). `-ffast-math`, `/fp:fast`,
`-ffp-contract=fast`로 컴파일하지 마세요. `render-consistency` 테스트가
실패합니다.

`meson test`에는 이 테스트가 포함됩니다: 내장 Caladea 글꼴을
`tests/consistency/*.ass`에 내장해 렌더링하고(설치된 글꼴과 무관),
각 이벤트의 픽셀이 `tests/consistency/expected.txt`와 완전히 일치할 것을
요구합니다——모든 플랫폼에서 같은 파일을 사용합니다. 의도적으로 렌더링
결과를 바꾼 경우 이 파일을 재생성해 변경과 함께 커밋하세요:

```bash
python3 tests/consistency/run.py --renderer build/src/bdnxml/vsfmrender --out build/consistency --update
```

## Aegisub에 설치

### Linux / macOS

Aegisub 3.5.6은 이 두 플랫폼에서 기본적으로 CSRI 지원을 컴파일하지 않습니다.
두 가지 장애물이 있으며, `docs/aegisub-csri-posix.patch`가 둘 다
수정합니다:

* `meson.build`는 `host_machine.system() == 'windows'`일 때만 이 기능을
  활성화
* `subprojects/csri/meson.build`는 `CSRI_PATH`를 한 번도 정의하지 않아
  POSIX 열거자(`lib/posix/enumerate.c`)를 컴파일할 수 없음——그 첫 문이
  `static const char csri_path[] = CSRI_PATH;`

```bash
cd /path/to/Aegisub
git apply /path/to/VSFilterMod/docs/aegisub-csri-posix.patch
meson setup build && ninja -C build
```

그다음 플러그인을 검색 경로 내 아무 위치에 두세요. 컴파일 시 내장된 기본
경로는 `~/.csri`와 `<prefix>/<libdir>/csri`이며, 환경 변수 `CSRI_PATH`가
그 뒤에 추가됩니다:

```bash
mkdir -p ~/.csri
cp build/vsfiltermod.so ~/.csri/        # macOS에서는 .dylib
chmod +x ~/.csri/vsfiltermod.so         # 아래 설명 참조
```

> POSIX 열거자는 `access(path, X_OK)` 검사에 실패한 파일을 건너뛰므로,
> 플러그인은 **반드시** 실행 권한을 가져야 합니다.
> `meson install`은 자동으로 설정하지만, 수동 `cp`는 설정되지 않을 수 있습니다.

### Windows

Aegisub의 Windows 열거자는 프로그램 옆의 `csri` 폴더에서 플러그인을
찾습니다:

```
Aegisub\
  aegisub.exe
  csri\
    vsfiltermod.dll
```

### 플러그인 선택

Aegisub를 재시작하고 *옵션 → 고급 → 비디오 → 자막 렌더러*에서
**CSRI/vsfiltermod_textsub**를 선택합니다. 목록에 없으면 플러그인을 못
찾았거나 로드에 실패한 것입니다. Linux와 macOS에서는 터미널에서 Aegisub를
실행해 로더의 경고 메시지를 확인하세요.

## 지속적 통합

`.github/workflows/ci.yml`은 8가지 구성을 빌드하고 테스트합니다:

| 잡 | 실행 환경 | 툴체인 | 글꼴 백엔드 |
| --- | --- | --- | --- |
| Linux x86_64 | `ubuntu-22.04` | GCC | fontconfig |
| Linux aarch64 | `ubuntu-22.04-arm` | GCC | fontconfig |
| macOS arm64 | `macos-15` | Clang | CoreText |
| macOS x86_64 | `macos-15-intel` | Clang | CoreText |
| Windows x86_64 | `windows-latest` | MSVC | DirectWrite |
| Windows x86 | `windows-latest` | MSVC(x64 → x86) | DirectWrite |
| Windows ARM64 | `windows-11-arm` | MSVC | DirectWrite |
| Windows x86_64(MinGW) | `windows-latest` | GCC, MSYS2 패키지 | DirectWrite |

MinGW 외에는 모두 고정 버전의 서브프로젝트에서 FreeType과 libpng를 빌드해
정적 링크하고(Linux에서는 `--exclude-libs`로 프라이빗 링크), 각 릴리스
바이너리가 같은 FreeType을 사용하도록 합니다. `macos-universal` 잡은
`lipo`로 두 macOS 버전을 통합합니다. `sanitizers` 잡은 AddressSanitizer와
UndefinedBehaviorSanitizer 하에서 테스트와 퍼징을 실행합니다.

모든 잡이 `csri-abi` 테스트(Aegisub 로더 방식으로 플러그인을 로드해
렌더링)와 `render-consistency` 테스트(이 플랫폼의 `tests/consistency/*.ass`
렌더링이 `expected.txt`와 픽셀 단위로 일치해야 함)를 실행하고,
`tests/smoke.ass`를 BDN+XML로 렌더링한 뒤 `tests/check_bdnxml.py`로 결과를
검증합니다. 검증기는 완전히 투명한 이미지를 거부하므로, 어떤 플랫폼의 글꼴
검색이 조용히 실패하면 빈 자막을 출력하는 대신 빌드가 오류가 됩니다.

마지막 `consistency` 잡은 모든 플랫폼의 결과를 모아 어떤 플랫폼이 일치하고
어떤 이벤트에서 불일치하는지 나열합니다.

`vapoursynth` 잡은 Linux(x86_64와 aarch64), macOS(arm64와 x86_64),
Windows x64에서 PyPI로 VapourSynth를 설치하고, 플러그인을 로드해
`vapoursynth` 테스트를 실행합니다. 각 플랫폼의 출력은
`tests/vsplugin/expected.txt`와 일치해야 합니다. 다른 환경에서 `vapoursynth`
Python 모듈이 설치되어 있지 않으면 이 테스트는 건너뜁니다.

두 가지 참고:

* **ARM Linux 빌드 머신은 공개 리포지토리에서만 무료입니다.** 리포지토리가
  비공개라면 `ubuntu-24.04-arm` 항목을 삭제하세요. 그렇지 않으면 해당 잡은
  오류 대신 계속 대기합니다.
* **배포 가능한 것은 MSVC 빌드 결과물입니다**: freetype, libpng, zlib,
  C 런타임이 모두 정적 링크되고, `vsfiltermod.dll`은 단일 파일로
  `DWrite.dll`, `KERNEL32.dll`, `USER32.dll`, `ADVAPI32.dll`에만 의존합니다.
  MinGW 결과물은 MSYS2 DLL에 의존하며, 컴파일 가능 여부 확인에만 씁니다.

### 릴리스

`v`로 시작하는 태그를 푸시하면 같은 빌드 매트릭스가 실행되고, 그 후
GitHub Release가 공개됩니다. 각 플랫폼에 하나의 압축 패키지(플러그인,
`vsfmrender`, 라이선스와 README)가 붙습니다:

```bash
git tag v1.0.0
git push origin v1.0.0
```

MinGW 버전은 공개되지 않습니다. 컴파일 가능 여부 확인에만 씁니다.

## Aegisub 밖에서 플러그인 검증하기

`tests/csri_loader_test.c`는 CSRI 로더와 완전히 같은 방식으로 동적 라이브러리를
로드하고(경로로 로드, 이름으로 각 진입 함수 해석), 메모리에서 자막을 파싱해
Aegisub가 사용하는 픽셀 포맷을 요청하고 1프레임을 렌더링합니다. Meson 테스트로
등록되어 있습니다:

```bash
meson test -C build csri-abi
```

## BDN+XML 렌더링

```bash
build/vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

같은 디렉터리에 `output.xml`과 `output_NNNN.png`가 기록됩니다.

이벤트 목록은 avs2bdnxml이 원본 `MaskSubMod`에서 얻는 것과 일치합니다:
`(REFERENCE_TIME)(10000000 * n / fps)`로 프레임을 샘플링하고, 원본처럼
단정밀도로 계산합니다. 인접한 같은 프레임은 통합되고, 빈 프레임이 이벤트를
끝냅니다. 픽셀에는 `MaskSubMod`의 역프리멀티플라이 수식을 씁니다.
avs2bdnxml-rs + 원본 VSFilterMod와 `bdn-diff`로 대조 완료: `tests/smoke.ass`의
각 이벤트의 InTC와 OutTC가 모두 같습니다.

`--fps`는 `분자/분모` 또는 소수를 받습니다. 23.976, 29.97, 59.94는 해당하는
`/1001` 분수를 의미하며, 이것이 AviSynth가 `MaskSub`에 전달하는 클립의 프레임
레이트입니다. 스크립트가 다른 프레임 레이트를 쓰면 정확한 분수를 전달하세요.
각 프레임은 `MaskSubMod`와 마찬가지로 각자의 시각에 개별 렌더링됩니다:
그 자막 소스는 모든 줄을 애니메이션으로 보고합니다.

`vsfmrender`가 **하지 않고** avs2bdnxml이 하는 것: 팔레트(8비트 PNG) 출력,
한 이벤트를 두 장의 이미지로 분할, PGS `.sup` 출력, Y 좌표 짝수 강제.
각 이벤트에 RGBA 이미지 한 장을 씁니다.

모든 옵션은 `vsfmrender --help`를 실행하세요.

## AviSynth+

같은 라이브러리는 AviSynth+ C 플러그인이기도 합니다:

```
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subs.ass", width=1920, height=1080, fps=24000.0/1001.0, length=34000)
# 또는 비디오 위에 오버레이:
TextSubMod(clip, "subs.ass", charset=1, fps=-1, vfr="timecodes.txt")
```

`TextSub`, `MaskSub`, `TextSubModSwapUV`도 등록되어 있으며 인수는 원본과
같습니다. 비디오 클립은 RGB32, RGB24, YUY2, YV12, I420이 가능합니다. YUV
출력은 원본처럼 자막의 `YCbCr Matrix` 헤더가 지정한 매트릭스를 씁니다.

렌더링 흐름은 원본의 `CFilter::Render`에 단계적으로 대응합니다——단정밀도
타임스탬프, 메모리 자막 레이어에서 이전 더티 사각형 지우기, 실제 렌더링된
경계 상자만 합성, RGB32와 RGB24는 상하 반전이고 `MaskSubMod`의 RGBA는 반전
안 함——이것들이 avs2bdnxml이 어떤 픽셀을 얻는지를 결정하는 요인이기 때문입니다.
avs2bdnxml-rs와 `tests/avs_dump.c`로 원본과 대조 완료: 이벤트 시간 동일.
`MaskSubMod`의 RGBA, `TextSubMod`의 RGB32, RGB24, YUY2, YV12의 자막 줄 동일
(I420과 YV12는 코드 경로를 공유, 개별 미테스트). VFR 프레임 선택 동일.

AviSynth는 링크 시 의존이 아닙니다. 플러그인은 로드 시 자신을 로드한
AviSynth 모듈에서 C API를 해석하므로, AviSynth가 설치되지 않은 머신에서도
같은 파일이 CSRI 플러그인으로 로드됩니다. `-Davisynth=false`로 컴파일하면
이 진입 함수들을 뺄 수 있습니다.

## VapourSynth

같은 라이브러리는 VapourSynth 플러그인이기도 합니다. API 4(VapourSynth R55
이상) 기반이며, 식별자, 네임스페이스, 인수는 원본과 같습니다:

```python
clip = core.vsfm.TextSubMod(clip, "subs.ass", charset=1, fps=23.976, vfr="timecodes.txt")
```

`file`과 `vfr`은 UTF-8 경로입니다(Windows에서는 원본처럼 ANSI 경로도 받습니다).
`fps` 기본값은 클립의 프레임 레이트. 가변 프레임 레이트 클립에서는 `fps` 또는
`vfr`을 지정해야 합니다.

| 클립 | 렌더링 방식 |
| --- | --- |
| YUV420P8 | 원본과 동일: YV12 자막 레이어 변환과 혼합. |
| RGB24 | 원본과 동일: 원본이 프레임 사본에서 하는 RGB32 팩 혼합. |
| 4:2:0 및 4:2:2 YUV, 9–16비트 | 같은 알고리즘, 완전한 샘플에 작용. |
| YUV422P8, 그레이스케일, 8비트 초과 RGB | 같은 알고리즘, 클립 자체 비트 깊이로 계산. |

원본이 YUV420P10과 P16을 지원한 방식은 각 샘플을 두 개의 8비트 절반으로 나눠
상위 절반에만 그리는 것이었습니다. `accurate=1`이면 두 절반 모두를 그려 색이
틀어집니다. 여기서는 `accurate`를 전달할 수 있지만 무시됩니다.
8비트로 정확한 배경에서 고비트 깊이의 휘도를 8비트로 오른쪽 시프트한 것은
8비트 출력과 완전히 같습니다. `tests/vsplugin/run.py`는 `vapoursynth` 테스트에서
이를 확인하고 출력 다이제스트를 비교합니다.
4:4:4 YUV는 거부됩니다. 자막의 색차는 픽셀 쌍으로 계산되기 때문입니다.
RGB로 변환하세요. 부동소수점 클립도 지원하지 않습니다.

`-Dvapoursynth=false`로 컴파일하면 이 진입 함수를 뺄 수 있습니다.

## 원본과의 비교

`tests/csri_compare.c`는 임의의 CSRI 플러그인으로 자막을 렌더링해 날 프레임을
내보냅니다. `tests/compare_raw.py`는 `MaskSubMod`의 수식으로 두 내보내기 결과를
변환하고, 커버리지 영역의 IoU, 불투명도 차이를 보고하고, 나란한 비교 이미지를
생성합니다. 첫 번째에는 Aegisub의 `csri/VSFilterMod.dll`을, 두 번째에는 이
프로젝트의 빌드 결과물을 지정하세요. 원본에 전달할 자막은 **반드시 UTF-8 BOM
포함**이어야 합니다: CSRI로 BOM 없는 파일을 읽으면 원본은 처리되지 않은
C++ 예외를 던집니다.

`tests/frs.ass`와 `tests/vertical.ass`(매초 1줄, `n.5`초에 샘플링)는 엔진이
GDI의 비공개 조판 규칙을 재현한 두 곳을 다룹니다: 각 각도의 `\frs`(CJK 문자,
`\fsp`, 볼드, 이탤릭, 스케일과 줄바꿈 포함)와 `@` 세로쓰기 글꼴(문장 부호,
병음, 그리스 문자, 전각 문자 포함). 이들은 宋体, 黑体, Microsoft YaHei,
DengXian, MS Gothic, Yu Gothic, Malgun Gothic, Times New Roman을 사용합니다.
이 글꼴들이 설치된 머신에서 실행하세요. 원본과의 비교 IoU는 각각
0.977–0.999와 0.967–1.000입니다.

`tests/fallback.ass`는 글리프 폴백을 다룹니다: Arial, Times New Roman,
Tahoma, Segoe UI의 CJK·한글 문자, 이탤릭, 밑줄·취소선, `\fsp`, `\frz`,
`\frs`, SRT식 기본 스타일 포함. 중국어 Windows 11에서 원본과 비교해
전체 11줄의 IoU는 0.985–0.999(글리프 폴백 추가 전은 0.12–0.67).
`tests/bold.ass`는 黑体, 宋体, 楷体의 합성 볼드를 다루고, 이탤릭과 `\frs`,
`\frs` 하의 볼드 폴백 글리프를 포함합니다: 0.977–1.000(이전 0.93–0.95).

## 삭제된 내용

이 프로젝트는 VSFilterMod의 축소판입니다. DirectShow 재생 전용 부분은
Aegisub도 BDN+XML 출력도 사용하지 않으므로 모두 삭제되었습니다: DirectShow
필터와 그 프로퍼티 페이지, VobSub/DVB/HDMV/CC/USF/SSF/RealText 파서,
DirectX 자막 레이어 백엔드, `VobSub()`, 그리고 VapourSynth 플러그인(구 API).

남은 것은 정말 중요한 부분입니다: 완전한 VSFilterMod 태그 세트(확장 태그
`\pngimg` 이미지 배경, 그라디언트, 이동 가능한 벡터 clip, 지터, distort,
문자 회전, Z 좌표, 세로 간격 포함).
