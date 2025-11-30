# Kernel Panic 원인 분석 및 해결책

## 로그 분석 결과

### 출력된 디버깅 메시지:
```
[DEBUG] running_thread: esp=0xc000ef30, t=0xc000e000
[DEBUG] running_thread: esp=0xc000edd0, t=0xc000e000
Kernel PANIC at ../../threads/thread.c:418 in thread_current(): assertion `t->status == THREAD_RUNNING' failed.
```

### 분석:
1. `running_thread()`가 두 번 호출됨 → `thread_schedule_tail()` → `process_activate()` 경로
2. `thread_current()`에서 Panic 발생
3. `process_execute`의 디버깅 메시지가 출력되지 않음 → **호출 전에 Panic 발생**

## 근본 원인

### 문제 지점:
1. **`process_execute()`의 line 43**: `cur = thread_current();`
   - 이 시점에 main 스레드가 BLOCKED 상태일 수 있음

2. **`thread_create()` 내부의 `thread_current()` 호출**:
   - line 300: `t->nice = thread_current()->nice;`
   - line 301: `t->recent_cpu = thread_current()->recent_cpu;`
   - line 329: `if (t->priority > thread_current()->priority)`
   - `process_execute()`에서 `thread_create()` 호출 시 BLOCKED 상태에서 실행될 수 있음

3. **`thread_unblock()` 내부의 `thread_current()` 호출**:
   - line 387: `if (thread_current() != idle_thread && ...)`

## 해결책

### 1. `process_execute()` 수정 (필수)
- `thread_current()` → `running_thread()` 변경
- BLOCKED 상태에서도 안전하게 동작

### 2. `thread_create()` 수정 (권장)
- `thread_current()` 호출을 `running_thread()`로 변경
- 또는 인터럽트 레벨 확인 후 안전하게 호출

### 3. `process_wait()` 수정 (이미 완료)
- `running_thread()` 직접 사용하도록 수정 완료

