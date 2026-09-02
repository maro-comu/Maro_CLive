# CLive_Maro

Visual Studio에서 열어 둔 C/C++ 문서를 자동으로 분석하고 실행 결과와 진단 설명을 보여 줍니다.

## 사용법

1. 저장소를 내려받은 뒤 최상위 `Maro_Release` 폴더의 `Maro_CLive_Maro.exe`를 실행합니다.
2. Visual Studio에서 `.c`, `.cpp`, `.h` 또는 `.hpp` 문서를 엽니다.
3. 저장하지 않은 편집 내용도 자동 분석됩니다. C/C++ 소스는 `Run`으로 실행하고 헤더는 `Analyze`로 분석만 하며 실행하지 않습니다. `Cancel`은 현재 작업을 중단합니다.
4. `OUTPUT`에서 실행 결과를, `CODE ANALYSIS`에서 오류 위치·설명·수정 제안을 확인합니다.

`Maro_Release`의 실행 파일은 x64 Windows용이며, 별도의 Visual C++ 런타임 설치 없이 실행할 수 있도록 구성했습니다.

## 빌드

Visual Studio 2026의 **x64 Native Tools Command Prompt**에서 실행합니다.

```bat
msbuild Maro_CLive_Maro.sln /m /p:Configuration=Release /p:Platform=x64
```
