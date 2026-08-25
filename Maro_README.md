# CLive_Maro

Windows에서 C/C++ 코드를 작성하고 빌드 결과와 진단 설명을 확인하는 간단한 Live IDE입니다.

## 요구 도구

- Windows 10/11 x64
- Visual Studio 2026의 **C++를 사용한 데스크톱 개발** 워크로드
- MSVC v145 및 Windows 10/11 SDK

## 빌드

1. **x64 Native Tools Command Prompt for VS 2026**을 엽니다.
2. 저장소 폴더에서 다음 명령을 실행합니다.

```bat
msbuild Maro_CLive_Maro.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
```

3. `Maro_Build\x64\Release\Maro_CLive_Maro.exe`를 실행합니다.

## 사용법

오른쪽 편집기에 C 또는 C++ 코드를 입력합니다. 코드는 자동으로 분석·실행되며, `Run` 또는 `F5`로 즉시 다시 실행할 수 있습니다. 왼쪽 `OUTPUT`과 `CODE ANALYSIS`에서 실행 결과, 오류 위치와 간단한 설명을 확인합니다.

테스트는 다음과 같이 실행합니다.

```bat
msbuild Maro_CLive_Maro_Tests.vcxproj /m /t:Build /p:Configuration=Release /p:Platform=x64
Maro_Build\x64\Release\Maro_CLive_Maro_Tests.exe
```
