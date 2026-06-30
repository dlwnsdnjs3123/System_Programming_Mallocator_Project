# System_Programming_Mallocator_Project

시스템프로그래밍 수업의 동적 메모리 할당기 프로젝트입니다. `mm_malloc`, `mm_free`, `mm_realloc`을 직접 구현하며 메모리 이용률과 처리 속도를 함께 개선하는 것을 목표로 했습니다.

## 개요

- `mm_malloc`, `mm_free`, `mm_realloc` 구현
- segregated explicit free list 기반 할당기 설계
- 인접 free block 즉시 coalescing
- 크기 클래스 내부에서 best-fit 성격의 탐색 적용
- 작은 크기 요청을 위한 별도 pool 최적화
- 가능할 때 `realloc`의 in-place 확장 시도

## 파일 구성

- `mm.c`: 할당기 핵심 구현
- `mm.h`: 인터페이스 및 구조체 정의
- `mdriver.c`: 정합성과 성능을 측정하는 드라이버
- `memlib.*`, `fsecs.*`, `fcyc.*`, `clock.*`, `ftimer.*`: 실습 환경 지원 코드
- `tracefiles/`: 드라이버 테스트용 trace 데이터

## 구현 요약

이 할당기는 free block을 크기별 segregated free list로 나누어 관리합니다. 이를 통해 요청 크기에 따라 탐색 범위를 줄이고, 불필요한 전체 탐색 비용을 낮췄습니다.

또한 free block이 생기면 즉시 coalescing을 수행해 단편화를 줄였고, 더 큰 block을 작은 요청에 사용할 때는 splitting을 통해 공간 낭비를 줄였습니다.

작은 크기의 반복 할당이 많은 workload를 고려해 전용 small-object pool도 추가했습니다. `realloc`은 가능하면 기존 block을 바로 확장하고, 불가능할 때만 새 공간을 할당해 복사하도록 구성했습니다.

## 보고서 기준 결과

제출 보고서 기준 성능은 다음과 같습니다.

- 전체 benchmark trace 통과
- 메모리 이용률: `98%`
- 처리량: `10,630 Kops`
- Performance Index: `99/100`

## 빌드

원래 32비트 실습 환경 기준으로 작성된 프로젝트입니다.

```bash
make
./mdriver
```

로컬 환경에 따라 `-m32` 지원이 필요할 수 있습니다.

## 메모

원본 제출 보고서는 개인 학사 정보가 포함되어 있어 공개 저장소에는 포함하지 않았습니다. 이 저장소는 구현 코드 자체를 포트폴리오 용도로 정리한 버전입니다.
