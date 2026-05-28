# 실행 검증 기록

제출 원본 반입 시점 `40bad57`과 정리한 코드를 같은 Docker 이미지에서 빌드하고, 과제에 포함된 QEMU 테스트로 비교했습니다. 기존 테스트 소스와 기대값은 유지했고, 검토에서 발견한 문제를 재현하는 테스트 4개를 추가했습니다.

## 결과

| 과제 | 원본 기존 테스트 | 수정본 전체 테스트 | 추가 테스트 |
| --- | --- | --- | --- |
| PRJ2 | 78 / 80 | 80 / 80 | 없음 |
| PRJ3 | 22 / 29 | 30 / 30 | 잠금 소유권 변경과 재획득 시 우선순위 기부 |
| PRJ4 | 95 / 113 | 116 / 116 | 큰 CREATE 크기, 큰 SEEK 위치, EOF 읽기의 잘못된 포인터 |

최종 결과는 **226 / 226 통과**입니다. 기존 222개와 새 회귀 테스트 4개를 포함합니다. 최종 판정 목록: [PRJ2](validation/prj2-final.results), [PRJ3](validation/prj3-final.results), [PRJ4](validation/prj4-final.results).

원본 실패 목록은 [PRJ2](validation/prj2-baseline.results), [PRJ3](validation/prj3-baseline.results), [PRJ4](validation/prj4-baseline.results)에 보관했습니다. 원본의 실패 원인은 다음과 같습니다.

- PRJ2: `no-vm/multi-oom`, `syn-read` 실패. 종료 시 열린 파일과 자식 기록을 회수하고, 자원 정리를 마친 뒤 대기 중인 부모를 깨우도록 수정했습니다.
- PRJ3: MLFQS의 load/recent_cpu/nice/block 관련 7개 실패. 원본에서 비어 있던 통계와 우선순위 계산을 구현했습니다.
- PRJ4: 페이지 병합 3개와 메모리 매핑 15개 실패. 프레임 교체와 파일 잠금의 경합을 수정하고, 미구현 매핑 호출을 추가했습니다.

## 추가 회귀 테스트

각 테스트를 해당 보완 직전 코드에 먼저 적용하여 실패를 확인했습니다. 이 비교 대상은 최초 제출 원본과 별개인 정리 도중의 소스입니다.

| 테스트 | 수정 전 관찰 | 수정 후 통과한 동작 |
| --- | --- | --- |
| `priority-donate-handoff` | 새 잠금 소유자의 우선순위가 32 대신 30, 재획득 시 33 대신 0 | 남은 대기자의 기부를 새 소유자에게 옮기고, 깨어난 대기자의 재시도 때 다시 등록 |
| `create-u32` | `inode_create()`의 음수 길이 ASSERT로 커널 중단 | INT_MAX 초과 크기 거부, 파일을 만들지 않으며 다음 정상 호출 성공 |
| `seek-u32` | `file_seek()`의 음수 위치 ASSERT로 커널 중단 | 표현 가능한 위치 범위를 검사하고 잘못된 위치는 기존 위치 유지 |
| `read-eof-ptr` | 새 버퍼 복사 경로에서 EOF를 만나면 잘못된 포인터를 검사하지 않고 반환 | 실제 읽기 전에 대상 전체의 매핑과 쓰기 권한 검증 후 잘못된 호출자를 종료 |

수정 전 판정 로그: [기부 전달](validation/priority-donate-handoff-before.result), [CREATE](validation/create-u32-before.result), [SEEK](validation/seek-u32-before.result), [EOF 포인터](validation/read-eof-ptr-before.result).

## 환경과 재현

- 호스트: Apple Silicon macOS, Docker Desktop.
- 컨테이너: Ubuntu 22.04, Linux ARM64.
- 컴파일러: `i686-linux-gnu-gcc` 11.4.0, `-fno-pie -no-pie` 및 과제의 기존 빌드 옵션.
- 실행기: `qemu-system-i386` 6.2.0.
- 실제 패키지 버전: [toolchain.txt](validation/toolchain.txt).
- 사용한 로컬 이미지: `sha256:1236645a5bea64ebd0b17d1b624eba4213d82c8165d43fc1e915572c6f9729e8`.

```sh
./tools/run-tests.sh
```

각 실행은 저장소를 읽기 전용으로 연결하고 소스를 컨테이너 안에 복사합니다. 사본의 이전 빌드 결과를 제거한 뒤 새로 컴파일합니다. 실행 중 컨테이너 네트워크는 꺼져 있습니다. Dockerfile의 패키지는 고정 버전이 아니므로 나중에 이미지를 다시 만들면 패키지 버전이 달라질 수 있습니다.

`test-results/<과제>/`에는 `results`, `build.log`, `check.log`, 테스트별 `.output`/`.result`/`.errors`, `toolchain.txt`, 입력 소스의 `source.sha256`이 남습니다. 장시간 MLFQS 테스트에는 과제에서 정한 480초 제한을 그대로 사용합니다. 최종 입력 파일 1,878개는 현재 소스와 SHA-256으로 일치함을 확인했으며, 입력 목록의 해시는 [source-manifests.sha256](validation/source-manifests.sha256)에 보관했습니다.

## 확인 범위

테스트가 통과한 QEMU 환경과 포함된 시나리오를 확인했습니다. 모든 실행 순서와 메모리 고갈 상황에 대한 완전성 증명은 아닙니다. PRJ4에서 모든 프레임을 사용할 수 없거나 스왑 공간이 부족하면 할당 실패를 호출자에게 전달합니다. 기반 파일 시스템의 고정 파일 크기 등 과제 기본 제약은 유지합니다.

기존 Pintos 기반 코드와 일부 기존 테스트의 컴파일 경고는 남아 있습니다. 변경한 구현과 새 테스트는 i386 대상 경고 검사도 별도로 통과했습니다. Bochs, 실제 하드웨어, x86-64 호스트에서는 실행하지 않았습니다.
