# CLive_Maro

Visual Studio에서 편집 중인 C/C++ 문서를 읽어 분석 결과를 가벼운 콘솔 화면으로 보여 줍니다.

## 사용법

1. Visual Studio에서 `.c`, `.cpp`, `.h` 또는 `.hpp` 파일을 엽니다.
2. `Maro_CLive_Maro.exe`를 실행합니다.
3. 화면 왼쪽 높이에 맞춰 열린 `OUTPUT`과 `ANALYSIS` 창에서 결과를 확인합니다.
4. 아래 키로 조작합니다.

   - `R` 또는 `F5`: 즉시 분석·실행
   - `C` 또는 `Esc`: 현재 작업 취소
   - `I`: 표준 입력 설정
   - `G`: 생성된 소스 보기
   - `F`: 수정 제안 확인 후 적용
   - `Tab`, `↑`, `↓`: 창 선택과 스크롤
   - `Q`: 종료

## 빌드

Visual Studio 2026의 **x64 Native Tools Command Prompt**에서 실행합니다.

```bat
msbuild Maro_CLive_Maro.sln /m /p:Configuration=Release /p:Platform=x64
```
