# stat shadow 변경 및 검증 기록

## 수정 파일

| 파일 | 변경 |
| --- | --- |
| `Source/Runtime/Core/Stats/LightweightStats.h` | 등록된 모든 Stat을 매 프레임 갱신. 프레임 시간과 실제 호출 횟수를 분리하고 임시 Map 재할당 제거 |
| `Source/Runtime/Core/Stats/ShadowStats.h` | CPU Total ID와 작업량/리소스의 수집 중·완료 프레임 데이터 |
| `Source/Editor/HitoriEd/EditorEngine.cpp` | 프레임 초기화/완료, View별 정보 기록, CPU Total 범위 |
| `Source/Runtime/Render/Renderer.h`, `.cpp` | 실제 Caster/Section Draw, Cascade/Directional/Point Face/Spot Tile Pass 계측과 리소스 스냅샷 |
| `Source/Runtime/Render/Shadow/ShadowRenderer.h`, `.cpp` | 타일 초기화 Draw 계측, Allocator의 현재 점유 타일 수/면적 추적 |
| `Source/Runtime/Render/Shadow/CascadeShadowMaps.h` | 실제 배열 텍스처 Desc 조회용 읽기 접근자 |
| `Source/Editor/Viewports/ViewportsPanel.cpp` | 기존 Overlay에 성능/작업량/리소스 표시, Profile과 중복 제거, 정보 행의 너비 처리 |
| `Tests/ShadowStatsTests.cpp` | Registry/Allocator/프레임 게시 회귀 테스트 |
| `Tests/ShadowRenderingStatsTests.cpp` | 창을 띄우지 않고 실제 D3D11 렌더러를 호출하는 통합 테스트 |

## 시간 측정의 정의

- CPU Total: 에디터의 `bRenderShadows` 블록 전체를 View마다 한 번 측정하고 프레임 단위로 합산한다. 그림자 설정, CSM 준비/수집/Draw, 일반 Directional 준비/수집/Draw, Atlas 컬링/캐싱/할당/초기화/Draw/Light Buffer 업로드, `PostUpdateLightComponents`, Cascade 미리보기를 포함한다. 메인 패스, 그림자 이전의 Scene/Light 수집, 통계 스냅샷 생성은 포함하지 않는다.
- Directional CPU: `RenderDirectionalShadowMap` 함수 호출 전체. 행렬 준비, 기존 BVH Caster 수집, Depth 초기화/Draw 제출, 상수 복구를 포함한다.
- CSM CPU: `PrepareCascadeShadows` 함수 호출 전체. 설정 적용, Cascade 행렬/경계 계산, 기존 Caster 수집, Cascade Depth 초기화/Draw, 상수 복구를 포함한다. 에디터용 축소 미리보기 생성은 CPU Total에만 포함된다.
- Atlas CPU: `RenderShadowMap` 함수 호출 전체. 컬링/캐시 판정/할당/타일 초기화/Draw와 함수 내부 Light Buffer 업로드를 포함한다.
- Total은 부모 구간의 실측값이다. 하위 항목을 Total에 다시 더하지 않는다. Total은 하위 항목의 단순 합보다 클 수 있다.
- Last/Avg/Max 단위는 ms. Last는 마지막 완료 프레임, Avg는 등록 이후 최대 최근 120프레임의 평균, Max는 Reset 이후 최대 프레임 합계다.
- 미호출 Stat에도 0 프레임을 추가한다. `CallCount`는 실제 `AddCycles` 호출 횟수만 누적한다. 여러 View 호출은 시간 샘플 하나로 합산하되 호출 횟수는 각각 센다. Picking의 Attempts에는 미호출 프레임이 추가되지 않는다.
- Depth Pass가 생략돼도 CPU에서 설정/컬링/캐시 검사/Light Buffer 갱신을 실행했다면 그 CPU 시간은 0이 아닐 수 있다. 해당 타이머 자체가 호출되지 않은 프레임만 0이다.

## 작업량과 리소스의 정의

작업량은 매 에디터 프레임 시작에 초기화하고 모든 View의 실제 제출을 합산한다. 프레임 끝에 리소스 상태와 함께 게시한다. Overlay는 CPU와 마찬가지로 마지막 완료 프레임을 읽는다. 통계가 꺼져 있어도 수집 경로는 같으며 Scene/BVH 추가 조회나 별도 Caster 목록을 만들지 않는다.

| 항목 | 정확한 의미 |
| --- | --- |
| Draw Calls | Shadow의 실제 `DrawIndexed` + Atlas 타일 초기화용 `Draw(3)` 호출 횟수. 메인 패스/미리보기 Draw 제외 |
| Triangles | 실제 Depth Draw의 LOD0 Section별 `IndexCount / 3` 합 + 타일 초기화 Draw당 1. 현재 Shadow 렌더러 자체가 LOD0을 사용한다 |
| Tile Clear Draws | 위 Draw Calls/Triangles에 포함된 Atlas 초기화 Draw의 개수 |
| Caster Submissions | 실제 Section Draw를 한 번 이상 제출한 Proxy의 Pass별 제출 횟수. Section 여러 개는 Caster 1회, 여러 Cascade/Face/View 재제출은 각각 1회 |
| Passes | 실제 Begin/Draw/End한 Directional/CSM Pass와 실제 갱신한 Atlas Face/Tile Pass의 합. Atlas 공통 바인딩이나 캐시 판정만으로 증가하지 않음 |
| Passes D/CSM/Point Faces/Spot Tiles | 위 Passes의 종류별 분해. Point는 실제 렌더한 Face만 집계 |
| Depth Clears | 실제 `ClearDepthStencilView` 호출 횟수. Draw Calls/Passes에 별도로 더하지 않음. Caster 없는 Directional/Cascade의 초기화도 포함 |
| Casting Lights D/P/S | 각 View에 이미 수집된 Light Buffer 중 CastShadow 및 Intensity > 0인 광원 수의 합. 시야/크기 컬링 전 후보 수이며 고유 광원 수가 아님 |
| Map Updates D/P/S | 해당 View에서 맵을 실제 갱신한 광원 수의 합. Point는 여러 Face여도 한 광원으로 집계. 캐시 재사용은 제외, Directional/CSM의 Depth 초기화만 수행한 갱신은 포함 |
| CSM Cascades | 이번 프레임의 활성 Cascade 수 최댓값/View. 활성 View 수와 설정 개수, 설정 비활성 상태도 별도 표시 |
| CSM Resolution / Shadow Distance | 현재 할당된 각 Cascade의 해상도 / 설정된 최대 거리(world units). 각 View의 실제 끝 거리는 CameraFar로 제한될 수 있음 |
| Atlas Tiles / Utilization | 프레임 끝 Allocator의 실제 Occupied 노드 수 / 전체 노드 면적 합 ÷ Atlas 면적 × 100. Spot Border 포함, 캐시 타일 포함 |
| Estimated Memory | 각 소유 텍스처의 실제 Desc에서 mip별 가로×세로×ArraySize×SampleCount×픽셀 바이트 수를 합산. 현재 포맷은 비압축. MB는 1024² bytes 기준 |

메모리에는 Directional, Atlas, CSM 배열, 아직 할당되어 있는 전용 Spot/Point Cube, 축소 미리보기 텍스처를 포함한다. 미리보기 메모리는 별도로 병기한다. SRV/DSV/Slice View는 중복 집계하지 않는다. CSM을 꺼도 해제되지 않은 리소스는 계속 계산한다. 드라이버 정렬/압축/실제 VRAM 할당량을 나타내는 값은 아니다.

## GPU 시간 보류

현재 엔진에는 재사용할 비동기 Timestamp/Disjoint 프레임 측정 인프라가 없다. 기존 Query 코드는 Occlusion 결과를 같은 프레임에 기다리거나 Readback하는 용도다. 이를 Shadow 시간 측정에 재사용하지 않았다. Overlay에는 `GPU Depth: N/A`를 표시하며 CPU 시간을 대신 표시하지 않는다.

후속 작업은 Timestamp/Disjoint Query의 프레임 간 수명 및 고정 용량 풀, 여러 View/Face/Tile 구간 합산, `D3D11_ASYNC_GETDATA_DONOTFLUSH` 기반 이전 프레임 비동기 회수, 미완료/Disjoint/용량 초과 결과의 N/A 처리다. 범위는 Depth Map 생성에 한정하고 메인 패스의 PCF/PCSS 비용과 분리해야 한다. 이 작업에서는 해당 구조를 새로 추가하지 않았다.

## 검증 결과

- Debug x64 에디터/엔진 빌드 성공. 제한된 실행 환경의 MSVC PDB C1902 오류는 동일 빌드를 제한 밖에서 실행하여 해소했다. 기존 숫자 변환/중복 import 라이브러리 등의 경고는 남아 있다.
- `ShadowStatsTests`: 성공. 실행 후 미호출 Last=0, 0을 포함한 평균, 120프레임 뒤 오래된 샘플 제거, lifetime Max/Total, Picking Attempts, 여러 호출 프레임 합산, 비활성/Always 타이머, Reset, 타일 할당/실패/해제/병합/전체 점유/Reset 검증.
- `ShadowRenderingStatsTests`: 실제 D3D11 Device 및 FRenderer로 성공. 각 렌더 구간의 Pipeline Statistics `IAPrimitives`와 Stat Triangles를 대조했다. 기대한 Section Draw + 타일 초기화 Draw, Caster/Pass 분해도 assertion으로 검증했다. 테스트에서만 완료 결과 대기를 사용하며 제품 통계에는 Query/대기를 추가하지 않았다.
- `git diff --check`: 성공.

| 요청한 상황 | 검증 범위 |
| --- | --- |
| 1. CSM 활성/비활성 | 실제 CSM Draw, 비활성 시 일반 Directional, 비활성 후에도 유지되는 메모리 확인 |
| 2. Point/Spot 단독 | 실제 Depth Draw/Pass/삼각형 수 확인 |
| 3. 일부 Point Face만 Caster 존재 | +X Face만 1개 Pass/타일을 제출하는 장면 확인 |
| 4. 일부 Atlas 타일 할당 | Point/Spot 타일 1개와 Spot Border 포함 면적, 캐시 유지 확인 |
| 5. Receiver 없음 | 에디터의 판정 이후 사용하는 `SetShadowsEnabled(false)` 경로 확인. 화면 Receiver 판정 자체의 UI 조작 검증은 미실시 |
| 6. CastShadow 비활성/Intensity 0 | 단독 Spot에서 Pass/후보 광원 0 확인 |
| 7. 작은 화면상 광원 | 작은 Spot에서 Pass 0 확인 |
| 8. Orthographic | CSM 설정 활성 상태의 일반 Directional fallback, 비활성 상태의 직교 분기 확인. 창/카메라 UI 검증은 미실시 |
| 9. 여러 Viewport | 동일 프레임에서 두 View 렌더 호출을 합산하고 Caster/광원 제출 및 공유 메모리를 확인. 실제 분할 Viewport UI 조작은 미실시 |
| 10. stat profile + stat shadow | 네 Shadow CPU 항목을 Profile에서 제외하는 조건 및 Total의 Stat 분류 확인. 동시 표시의 화면 배치 시각 검증은 미실시 |
| 11. 실행 직후 완전 생략 | Registry의 Last=0/평균/호출 수와 작업량 다음 프레임 0, 실제 Atlas 캐시 프레임 Draw/Pass=0 확인 |

기존 렌더링 조건, Culling/Cache 판정, 할당/해제 순서, Light Buffer 갱신 및 실제 Draw 인자는 변경하지 않았다. Allocator에 점유량 계수 갱신을 추가했으며 노드 상태 전이와 할당 결정은 기존 그대로다. 통계의 정수 연산/읽기 조회와 CPU 타이머 오버헤드는 추가된다. 에디터 GUI 화면의 시각 검증과 GPU 시간 측정은 완료하지 않았다.

## 재실행

프로젝트 루트에서 VS x64 Developer Command Prompt로 실행한다. 테스트는 독립 실행 파일이므로 에디터/엔진 vcxproj에는 포함하지 않는다.

```bat
MSBuild Source\Editor\HitoriEditor.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
cl /nologo /std:c++20 /EHsc /utf-8 /MDd /D_DEBUG /I Source\Runtime /I Source\ThirdParty\json /I Resources\Shader Tests\ShadowStatsTests.cpp /FoBuild\ShadowStatsTests.obj /FeBuild\ShadowStatsTests.exe /link Build\Bin\Debug-windows-x86_64\HitoriEngine.lib
Build\ShadowStatsTests.exe
cl /nologo /std:c++20 /EHsc /utf-8 /MDd /D_DEBUG /I Source\Runtime /I Source\ThirdParty\json /I Resources\Shader Tests\ShadowRenderingStatsTests.cpp /FoBuild\ShadowRenderingStatsTests.obj /FeBuild\ShadowRenderingStatsTests.exe /link Build\Bin\Debug-windows-x86_64\HitoriEngine.lib
Build\ShadowRenderingStatsTests.exe
```
