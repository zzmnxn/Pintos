# 최종 수정 요약 및 원인 분석

## 발견된 문제

### Kernel Panic 발생 위치
```
PANIC at ../../threads/thread.c:433 in thread_current(): assertion `t->status == THREAD_RUNNING' failed.
```

### 로그 분석
- `running_thread()`가 두 번 호출됨
- `process_execute`의 디버깅 메시지가 출력되지 않음
- **`thread_schedule_tail()`의 디버깅 메시지도 출력되지 않음**

## 근본 원인

### 발견된 `thread_current()` 호출 지점들:

1. **`tss_update()`** (발견!)
   - `userprog/tss.c:105`: `tss->esp0 = (uint8_t *) thread_current () + PGSIZE;`
   - `process_activate()` → `tss_update()` 호출
   - `thread_schedule_tail()` → `process_activate()` 호출
   - **컨텍스트 스위치 중에 호출되므로 BLOCKED 상태에서 실행될 수 있음**

2. **`process_execute()`**
   - line 43: `cur = thread_current();`

3. **`thread_create()`**
   - line 300, 301, 329: `thread_current()` 호출

4. **`thread_unblock()`**
   - line 387: `thread_current()` 호출

5. **`load()`**
   - line 455: `t = thread_current();`

## 적용된 수정사항

### 1. `userprog/tss.c` - `tss_update()`
```c
// 변경 전
tss->esp0 = (uint8_t *) thread_current () + PGSIZE;

// 변경 후
tss->esp0 = (uint8_t *) running_thread () + PGSIZE;
```

### 2. `userprog/process.c` - `process_execute()`
```c
// 변경 전
cur = thread_current();

// 변경 후
cur = running_thread();
```

### 3. `threads/thread.c` - `thread_create()`
- line 300, 301: `thread_current()` → `running_thread()` 변경
- line 329: `thread_current()` → `running_thread()` 변경

### 4. `threads/thread.c` - `thread_unblock()`
- line 387: `thread_current()` → `running_thread()` 변경

### 5. `userprog/process.c` - `process_wait()`
- `running_thread()` 직접 사용으로 변경

### 6. `userprog/process.c` - `process_activate()`
- 이미 `running_thread()` 사용 중 (이전 수정 완료)

## 핵심 발견

**가장 중요한 발견: `tss_update()`에서 `thread_current()` 호출**

`tss_update()`는 `process_activate()`에서 호출되고, `process_activate()`는 `thread_schedule_tail()`에서 호출됩니다. 컨텍스트 스위치 중에는 스레드가 아직 `THREAD_RUNNING` 상태가 아닐 수 있어 assertion이 실패할 수 있습니다.

## 테스트 준비

모든 수정이 완료되었고 빌드가 성공적으로 완료되었습니다.

테스트 실행:
```bash
cd /sogang/under/cse20231552/pintos/src/userprog/build
make tests/userprog/read-normal.result
```

