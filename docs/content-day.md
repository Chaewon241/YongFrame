# 콘텐츠 조각 2: 하루와 잠 (설계)

> 상태: **서버 S0의 1~5단계(순수 로직, 프로토콜과 GameService, 시계 저장, 도구, 변형 검증) 완료. 6단계(문서), 완료 기준 12의 측정(사용자), Unreal은 남음.** 진행 상태는 13절. 아래 1~12절은 구현 전에 확정한 설계이며, 범위, 기본값, 프로토콜, 완료 기준을 구현 전에 확정한 문서다. 구현 세션은 이 문서와 CLAUDE.md만 읽고 시작할 수 있게 썼다. 기획 배경은 [game-plan.md](game-plan.md), 조각 순서는 [content-roadmap.md](content-roadmap.md)에 있다. "사용자 결정"으로 표시하지 않은 값은 **임의 기본값**이고 `PlayConfig` 한 곳에서 바꾼다. 측정 합격 기준(완료 기준 12)은 측정 전에 쓴 것이라 결과를 본 뒤 바꾸지 않는다. 구현하면서 정한 세부 사항과 검증 결과는 이 문서 맨 아래에 이어서 적는다.

## 1. 목표

게임의 시간 단위를 **틱(초)에서 하루로** 바꾸는 바탕을 만든다. 이 조각이 끝나면:

- 방마다 게임 시계가 돈다(1일차 06:00에서 시작해 02:00에 끝나며, 실시간 10분).
- 플레이어가 집 앞에서 잠들고, **방 안 모두가 잠들면**(또는 02:00이 되면) 서버가 **밤 정산**을 하고 다음 날 06:00으로 넘어간다.
- 지금 틱으로 돌아가는 두 규칙을 밤 정산으로 옮긴다. 고갈된 나무와 돌은 30초 뒤가 아니라 **밤에** 다시 생기고, 지은 집은 5초 뒤가 아니라 **다음 날 아침에** 완공된다.
- 농장은 날짜와 시각을 저장하고, 다시 열면 닫힌 시각부터 이어서 한다.

조각 3(농사), 4(경제), 5(제작)는 모두 이 밤 정산에 규칙을 하나씩 더하는 모양이다. 그래서 이 조각에서 밤 정산의 **순서와 시험 방법**을 단단하게 정한다.

## 2. 사용자 결정 (바꾸지 않는다)

| 항목 | 결정 |
| --- | --- |
| 하루 길이 | 실시간 10분 |
| 하루 넘김 | 방 안 **모두가 잠들면** 넘어간다. 새벽 2시에 깨어 있는 사람도 강제로 잠든다 |
| 기존 채집과 건설 | 하루 단위로 바꾼다. 고갈된 노드는 밤에 다시 생기고 집은 다음 날 아침에 완공된다 |
| 잠드는 곳 | 시작 집이나 **완공된** 집 앞 (game-plan.md 기본값을 사용자가 그대로 두기로 함) |
| 다시 열 때 | 닫힌 날, 닫힌 시각 그대로 이어서 한다 (위와 같음) |
| 강제로 잠들 때의 벌칙 | 없음 (위와 같음) |
| 넣지 않는 것 | 체력, 계절, 날씨 |

## 3. 범위

**포함**

- 방 시계: 날 번호와 그날의 경과 틱. 클라이언트는 스냅샷 틱에서 시각을 계산한다(주기적인 시계 메시지가 없다)
- 시작 집(고정 물체 1개): 이동을 막고, 그 위에는 지을 수 없고, 그 앞에서 잘 수 있다
- 잠자기와 깨어나기 요청, 잠든 사람 목록 알림
- 밤 정산(정해진 순서의 결정적 계산)과 새 날 알림
- 채집 재생과 건설 완공을 밤 정산으로 옮김(틱 기준 재생과 완공은 없앤다)
- 시계 저장과 복원(새 마이그레이션 `004`)
- `game_client` 명령, `game_load_client`가 새 메시지를 견디게 하기, 밤 정산 비용 측정
- Unreal: 시계 HUD, 잠자기 입력, 새 날 화면 전환, 시작 집 액터, 커맨들릿 시험

**제외 (이 조각에서 하지 않는다)**

- 작물 성장, 물 마름, 출하 판매, 스프링클러: 밤 정산에 **빈 자리(주석)만** 둔다. 조각 3~5의 일이다
- 맵 여러 개(조각 2.5, [game-plan.md](game-plan.md) "맵"): 이 조각은 **지금처럼 맵 하나** 기준이다. 밤 정산을 맵마다 반복하고 아침에 모두 농장 맵에서 깨는 변경은 조각 2.5에서 한다. 이 조각의 범위와 완료 기준은 바뀌지 않는다
- 출하 상자, 상점: 고정 물체 목록(`Landmark`)에 나중에 더할 수 있게 열거형만 열어 둔다
- 체력, 계절, 날씨, 요일, 달력, 축제
- 시간 일시정지, 시간 배속, 혼자일 때 시간 멈춤
- 정교한 조명(해의 고도 계산): Unreal에서 시각으로 조명 하나를 돌리는 것은 **선택 사항**이다
- 침대와 실내, 잠자는 애니메이션, 잠든 몸의 충돌 변화(잠든 사람도 지금처럼 다른 플레이어와 충돌하지 않는다)
- 강제 취침 벌칙, 늦게 자면 다음 날 늦게 깨는 규칙
- 집 철거와 이동

## 4. 기본값 (`PlayConfig`, 임의 기본값)

| 항목 | 이름(제안) | 값 | 비고 |
| --- | --- | --- | --- |
| 하루 시작 시각 | `day_start_minute` | 360 (06:00) | 자정부터 센 게임 분 |
| 하루 끝 시각 | `day_end_minute` | 1560 (다음 날 02:00) | 24시간을 넘겨 센다 |
| 게임 1분의 틱 수 | `ticks_per_game_minute` | 10 (0.5초) | 하루 = (1560 - 360) x 10 = **12,000틱 = 10분** |
| 측정용 하루 길이 | `bench_day_ticks` | 0 (끔) | 0보다 크면 하루가 이 틱 수다. **부하 측정과 시험 전용**이고 서버 옵션 `--bench-day-ticks`로 준다 |
| 잠들 수 있는 거리 | `sleep_range_cm` | 150 | 내 중심에서 집 발자국(직사각형)까지의 최단 거리, 경계 포함(150.0 허용). 서버가 아는 위치 기준 |
| 시작 집 중심 | `farmhouse_x_cm`, `farmhouse_y_cm` | (2500, 2850) | 맵 중앙(2500, 2500)의 북쪽. 발자국 x 2300~2700, y 2700~3000(100 cm 격자에 맞음) |
| 시작 집 크기 | `farmhouse_width_cm`, `farmhouse_height_cm` | 400 x 300 | **너비가 0이면 시작 집이 없다**(시작 집과 무관한 기존 시험용) |
| 첫 날 번호 | | 1 | |

값을 이렇게 잡은 이유: 시작 칸은 맵 중앙에서 (±100, ±100) cm다. 북쪽 두 칸(y = 2600)은 시작 집까지 정확히 100 cm라서 **아침에 깨어난 자리에서 바로 잘 수 있다**. 남쪽 두 칸(y = 2400)은 300 cm라서 몇 걸음 걸어야 한다. 시작 집은 건설 구역(중앙 사방 1,200 cm) 안에 있으므로 노드 배치에는 영향이 없다.

**없애는 값:** `respawn_ticks`(600), `build_ticks`(100). 재생할 자리를 못 찾았을 때 다시 찾는 간격 `respawn_retry_ticks`(20)는 남긴다(5.4절).

## 5. 규칙

### 5.1 시계

- 방은 `day`(1부터)와 `day_start_tick`(그날이 시작된 월드 틱, **부호 있는 64비트**)을 가진다. 그날의 경과 틱은 `elapsed = world.tick - day_start_tick`이다.
- 게임 시각(분)은 `day_start_minute + elapsed / ticks_per_game_minute`(정수 나눗셈)이다. 표시는 클라이언트가 한다(06:00 ~ 01:59, 10분 단위로 보여 주는 것은 클라이언트의 선택).
- 하루 길이 `day_length = bench_day_ticks > 0 ? bench_day_ticks : (day_end_minute - day_start_minute) * ticks_per_game_minute`.
- 방을 새로 만들면 `day = 1`, `day_start_tick = 0`(월드 틱도 0에서 시작한다)이다.
- 저장된 농장을 열면 `day`는 저장된 값으로, `day_start_tick = world.tick - 저장된 elapsed`로 둔다. 월드 틱이 0이면 `day_start_tick`이 **음수**가 될 수 있는데, 이는 정상이다(그래서 부호가 있다).

### 5.2 잠

- 요청은 "잔다" 또는 "깬다" 하나다(`SleepRequest{sleep}`).
- **잔다**의 판정 순서(처음 걸리는 것): 방에 없음(`GAME_NOT_IN_ROOM`) → 이미 잠듦(성공으로 답하고 아무것도 바꾸지 않음) → **잘 수 있는 집이 근처에 없음**(`GAME_SLEEP_TOO_FAR`). 잘 수 있는 집은 시작 집과 **완공된** 집이다(건설 중인 집은 안 된다). 거리는 5.4의 `sleep_range_cm`이다.
- 잠들면 이동 방향이 (0, 0)이 된다. 잠든 동안:
  - `MoveInput`은 **통째로 무시**한다(`last_input_seq`도 올리지 않는다)
  - 채집과 건설은 `GAME_SLEEPING`으로 거부하고 상태를 바꾸지 않는다
  - 깨어나기는 언제든 된다
- **깬다**: 방에 없으면 `GAME_NOT_IN_ROOM`, 이미 깨어 있으면 성공(변화 없음), 잠들어 있으면 깨운다(위치는 그대로다).
- 거부된 요청은 상태를 바꾸지 않는다. 연결은 유지한다.
- 방을 나가거나 연결이 끊기면 그 사람의 잠 상태는 사라진다. 들어오는 사람은 깨어 있는 상태로 들어온다.
- 잠든 사람 목록이 바뀐 틱의 끝에 `SleepUpdate`(현재 잠든 사람 전부)를 방 전원에게 보낸다. 바뀌지 않았으면 보내지 않는다.

### 5.3 밤 정산이 일어나는 때

틱마다, 방마다 이동 계산(`Step`)이 끝난 **뒤에** 판정한다.

- (가) 방에 사람이 있고 **모두** 잠들었으면 밤 정산을 한다(`forced = false`)
- (나) 아니면, `elapsed >= day_length`이면 밤 정산을 한다(`forced = true`). 이때 깨어 있는 사람도 그 자리에서 강제로 잠든다(벌칙 없음)
- 한 틱에 밤 정산은 많아야 한 번이다. 따라잡기 틱(`Tick(steps > 1)`)이 하루 끝을 넘어도 정산은 한 번이고, 새 날은 **그 틱**에서 시작한다(경계를 넘은 틱 수만큼의 차이는 버린다. 결정성 시험은 `steps = 1`로 한다)
- 잠자기 요청은 틱과 틱 사이에 처리되므로, 마지막 사람이 잠든 **다음 틱**에 정산된다
- 마지막으로 깨어 있던 사람이 방을 나가서 남은 사람이 모두 잠든 상태가 되면, 다음 틱에 (가)로 정산된다

### 5.4 밤 정산의 내용 (이 순서대로, 한 틱 안에서)

1. **완공:** 건설 중인 모든 집을 완공으로 바꾼다(`ready_tick` = 이 틱, 바뀐 구조물로 기록)
2. **(조각 3~5 자리)** 작물 성장, 스프링클러 물 주기, 물 마름, 출하 판매. 이 조각에서는 **빈 자리에 주석만** 둔다
3. **깨우기와 시작 위치:** 모든 사람의 잠을 깨우고, 이동 방향을 (0, 0)으로 하고, 각자 자기 칸(slot)의 시작 위치로 옮긴다(막혀 있으면 지금 `AddPlayer`와 같은 "가장 가까운 빈 자리" 규칙). `last_input_seq`는 그대로 둔다
4. **재생:** 고갈된 **모든** 노드를 체력을 가득 채워 되살린다. 자리 고르기는 지금 규칙 그대로다(건설 구역 밖, 가장자리 여백, 최소 간격, 구조물/시작 집/플레이어를 피하는 결정적 난수). 자리를 못 찾은 노드는 고갈 상태로 남고, 낮 동안 `respawn_retry_ticks`(20틱)마다 다시 찾는다(지금의 재시도 장치를 그대로 쓴다)
5. **날 넘김:** `day += 1`, `day_start_tick = 이 틱`
6. **알림과 저장:** `DayStarted`를 방 전원에게 보낸다(5.5). 농장이면 구조물과 시계를 저장 대기열에 넣고 **바로** 저장한다(`Flush`)

깨우기(3)를 재생(4)보다 먼저 하는 이유: 재생 자리가 플레이어 몸을 피하는데, 그 기준이 아침 위치여야 한다(지금 기본값에서는 노드가 건설 구역 밖에만 있어서 결과가 같지만, 순서를 고정해 둔다).

**낮 동안 바뀌는 것:** 채집을 해도 노드에 재생 시각을 예약하지 않는다(고갈되면 밤까지 고갈 상태다). 건설은 건설 중 구조물을 만들고 완공 시각을 정하지 않는다. 충돌은 지금처럼 건설 중일 때부터 있다.

### 5.5 알림 순서

밤 정산이 있었던 틱에는 그 방 사람마다 **`DayStarted`를 가장 먼저** 보낸다. 그 뒤는 지금 순서를 따른다: `WorldSnapshot`(아침 위치) → `ResourceUpdate`(되살아난 노드) → `StructureUpdate`(완공된 집). 그다음 `SleepUpdate`가 있으면 보낸다(밤 정산 뒤에는 잠든 사람이 없으므로 빈 목록이다). 클라이언트는 `DayStarted`를 받으면 보간을 초기화한다. 그래야 순간이동을 미끄러지듯 그리지 않는다.

### 5.6 시작 집

- 모든 방(농장과 게스트 방)에 같은 자리로 있는 고정 물체다. 저장하지 않고 설정에서 다시 만든다.
- **이동을 막는다**(구조물과 같은 직사각형 장애물, `World::AttachObstacles`에 고정 직사각형 목록을 더한다).
- **겹치게 지을 수 없다**(`GAME_BUILD_BLOCKED`). 노드 재생 자리도 피한다(지금 기본값에서는 건설 구역 안이라 원래 생기지 않는다).
- 구조물 수 상한(20)에 세지 않고, 구조물 번호(1부터 이어지는 저장 번호)를 쓰지 않는다. 프로토콜에서도 구조물이 아니라 `Landmark`로 따로 보낸다.
- **옛 농장 데이터:** 이 조각 전에 저장된 농장에는 시작 집 자리에 집이 있을 수 있다. 복원은 그대로 하고(겹친 채로 둔다), 겹친 장애물에 갇히지 않는 지금의 규칙("이미 겹친 장애물과는 충돌하지 않는다")에 맡긴다. 개발용 데이터의 한계로 적고, 겹친 채 복원해도 죽지 않는다는 시험을 하나 둔다.

## 6. 프로토콜 (`proto/game.proto`, 하위 호환으로 더한다)

메시지 ID는 `src/protocol/message_ids.h`가 기준이다. 116~119는 비어 있다(120부터 계정).

| ID | 메시지 | 방향 | 설명 |
| ---: | --- | --- | --- |
| 116 | `SleepRequest{bool sleep}` | C→S | true면 잔다, false면 깬다 |
| 117 | `SleepResponse{GameResult result, bool sleeping}` | S→C | 결과와 요청 뒤의 내 잠 상태(성공/실패 모두 현재 값) |
| 118 | `SleepUpdate{uint64 tick, repeated uint32 sleeping_player_ids}` | S→C | 잠든 사람 목록이 바뀐 틱에만, 방 전원(본인 포함)에게. 현재 잠든 사람 **전부**(최대 4명) |
| 119 | `DayStarted{uint64 tick, DayClock clock, bool forced}` | S→C | 밤 정산이 끝났다. `forced`는 02:00 강제 정산인지 여부 |

새 메시지 형:

```proto
// 방의 시계. 클라이언트는 받은 스냅샷 틱으로 elapsed = tick - day_start_tick을 계산해 시각을 그린다.
message DayClock {
  uint32 day = 1;                    // 1부터
  int64 day_start_tick = 2;          // 그날이 시작된 월드 틱. 저장된 농장을 열면 음수일 수 있다
  uint32 day_length_ticks = 3;       // 하루의 틱 수 (기본 12000)
  uint32 ticks_per_game_minute = 4;  // 기본 10
  uint32 day_start_minute = 5;       // 기본 360 (06:00)
}

enum LandmarkKind {
  LANDMARK_FARMHOUSE = 0;  // 시작 집. 조각 4에서 출하 상자, 상점이 더해진다
}

// 모든 방의 같은 자리에 있는 고정 물체. 축에 정렬된 직사각형이고 (x, y)는 중심이다.
message Landmark {
  LandmarkKind kind = 1;
  float x = 2;
  float y = 3;
  float width = 4;
  float height = 5;
}
```

기존 메시지에 더하는 필드:

- `RoomState.clock = 7` (`DayClock`), `RoomState.landmarks = 8` (`repeated Landmark`), `RoomState.sleeping_player_ids = 9` (`repeated uint32`). 늦게 들어온 사람도 이것으로 지금 시각과 잠든 사람을 안다.
- `Structure.ready_tick`의 뜻이 바뀐다: **건설 중이면 0**(다음 밤 정산에 완공), 완공이면 완공된 틱. 필드 번호와 형은 그대로다. Unreal이 이 값으로 진행률을 그리고 있다면 고쳐야 한다(10절).

결과 코드(`GameResult`)에 더한다:

| 값 | 이름 | 뜻 |
| ---: | --- | --- |
| 21 | `GAME_SLEEP_TOO_FAR` | 시작 집이나 완공된 집 앞(`sleep_range_cm` 안)이 아니다 |
| 22 | `GAME_SLEEPING` | 잠든 상태에서는 채집과 건설을 할 수 없다 |

판정 순서의 변화: 채집은 방에 없음 → **잠듦** → 노드 없음 → 고갈 → 너무 멂 → 쿨다운이 된다. 건설은 방에 없음 → **잠듦** → 종류 → 상한 → 너무 멂 → 구역 밖 → 겹침/맵 밖 → 자원 부족이 된다.

호환되지 않는 변경은 없다(메시지, 필드, 열거값 추가만). `ready_tick`의 뜻이 바뀌는 것은 [stage4.md](stage4.md)의 프로토콜 표와 [unreal-client.md](unreal-client.md)에 기록한다.

## 7. 저장 (농장)

**마이그레이션 `db/migrations/004_create_farm_clock.sql`** (새 표. 기존 규칙대로 `CREATE TABLE IF NOT EXISTS`)

| 열 | 형 | 비고 |
| --- | --- | --- |
| `farm_id` | BIGINT UNSIGNED PK, FK → `farms.id` | |
| `day` | INT UNSIGNED NOT NULL DEFAULT 1 | |
| `day_elapsed_ticks` | BIGINT UNSIGNED NOT NULL DEFAULT 0 | 저장할 때의 `elapsed` |
| `updated_at_unix_ms` | BIGINT NOT NULL | |

행이 없으면 1일차 경과 0으로 본다(이 조각 전에 만든 농장).

**저장소 인터페이스:** `FarmData`에 `std::optional<StoredClock> clock`, `FarmSave`에 `std::optional<StoredClock> clock`을 더한다(`StoredClock{day, day_elapsed_ticks}`). `SaveFarm`의 트랜잭션에 시계 upsert를 포함한다(절대값 덮어쓰기, 멱등). MySQL 구현의 테이블 이름 옵션에 `farm_clock`을 더하고, `account_mysql_test`는 `farm_clock_test`를 쓴다.

**저장 시점** (`FarmPersistence`)

| 사건 | 시계 저장 |
| --- | --- |
| 밤 정산 | **바로**(완공된 구조물과 함께, `elapsed = 0`) |
| 농장이 닫힘(마지막 사람이 나감) | **바로**(닫힌 시각) |
| 서버 종료 | 종료 저장에 포함 |
| 주기 저장(10초) | **다른 변경이 있어 어차피 저장할 때만** 함께 싣는다. 시계만 바뀐 농장을 10초마다 쓰지는 않는다(시계는 매 틱 바뀌므로) |

- 서버가 비정상 종료하면 시계는 마지막 저장 시각으로 돌아간다(최대 "마지막 저장 이후의 시간"). 한계로 적는다.
- **건설 중인 집:** 저장 형식(`state`, `remaining_ticks`)은 그대로 둔다. 새로 저장하는 건설 중인 집의 `remaining_ticks`는 0이다. 복원할 때는 `remaining_ticks` 값과 상관없이 **건설 중**으로 되살리고 다음 밤 정산에 완공된다(옛 데이터도 같은 규칙).
- **복원한 elapsed가 하루 길이 이상이면**(설정이 바뀌었거나 값이 잘못됨) `elapsed = day_length`로 둔다. 그러면 다음 틱에 강제 정산된다. 로그에 남긴다.
- 게스트 방은 시계를 저장하지 않는다(지금처럼 방이 닫히면 사라진다).
- 노드 상태는 지금처럼 저장하지 않는다. 하루 중간에 농장을 닫았다 열면 그날 깎은 노드가 되살아난다(game-plan.md에 적은 한계).

## 8. 서버 구조 (구현 안내)

원칙은 그대로다. `Room`, `World`, `ResourceField`, `StructureSet`은 소켓, 시계, DB를 모르고 **틱 번호로만** 시간을 안다. 밤 정산은 `Room`의 순수 함수로 만들어 소켓 없이 시험한다.

| 파일 | 바꿀 것 |
| --- | --- |
| `src/play/play_config.h` | 4절의 값 더하기, `respawn_ticks`와 `build_ticks` 없애기, `DayLengthTicks()` 도우미 |
| `src/play/room.h/.cpp` | `day_`, `day_start_tick_`, `Member::sleeping`. `Sleep(client_id, bool)` → 결과, `ShouldEndDay(tick)` → 없음/모두 잠듦/강제, `RunNight(tick, forced)` → `NightReport{completed, respawned, day}`, `RestoreClock(day, elapsed)`, `ExportClock()`. `Gather`와 `Build`에 잠듦 판정 더하기 |
| `src/play/resources.h/.cpp` | `Hit`이 재생 시각을 예약하지 않는다. `RespawnAllDepleted(tick, blockers)` 더하기(실패한 노드만 재시도 예약). 기존 `Advance`는 재시도 전용이 된다 |
| `src/play/structures.h/.cpp` | `Add`는 완공 시각 없이 건설 중으로 만든다. `CompleteAll(tick)` 더하기. `Restore`는 건설 중이면 그대로 건설 중. 틱 기준 `Advance` 없애기 |
| `src/play/world.h/.cpp` | 고정 직사각형 장애물(시작 집) 목록, 잠든 사람의 입력 무시(또는 `Room`에서 막기), 아침 위치로 옮기는 함수(칸 번호 → 시작 위치, 막히면 가장 가까운 빈 자리) |
| `src/play/game_service.h/.cpp` | `HandleSleep`, 틱 순서(이동 → 정산 판정 → `RunNight` → 재시도 재생 → 보낼 것 확보 → 전송), 5.5의 알림 순서, `SleepUpdate`, 농장 시계 복원과 저장 호출, 통계(아래) |
| `src/play/farm_persistence.h/.cpp` | 시계 값 보관, 7절의 저장 시점 |
| `src/farm/*` | `StoredClock`, 인메모리와 MySQL 구현, `farm_clock` 테이블 이름 옵션 |
| `src/server/session.cpp` | ID 116을 `GameService`로 넘기기. 클라이언트가 117~119를 보내면 알 수 없는 ID처럼 연결을 닫는다(기존 규칙) |
| `src/server/main.cpp` | `--bench-day-ticks N` |
| `src/client/game_client.cpp` | `/sleep`, `/wake`, `/clock`(날과 시각, 잠든 사람), `DayStarted`와 `SleepUpdate` 출력 |
| `src/tools/game_load_client.cpp` | 116~119를 받아도 죽지 않는다. `DayStarted`를 받으면 **그다음 스냅샷 한 번**은 틱당 이동 거리 검사를 건너뛴다(순간이동). 받은 밤 정산 수를 결과에 출력한다 |

**통계(`GameStats`와 통계 줄):** `days_started`(밤 정산 수), `forced_nights`(그중 강제), `night_us`(밤 정산에 쓴 시간 합계, 틱 시간 분해의 새 항목), `max_night_micros`(방 하나의 정산 최대 시간). 통계 줄 형식이 바뀌면 `stats_format`과 그 시험, results/stage4의 집계 스크립트가 새 항목을 무시하는지 확인한다.

**기존 시험에 생기는 변화(예상):** 틱 기준 재생 시험(`play_resources_test`의 "600틱 뒤 재생" 등), 틱 기준 완공 시험(`play_structures_test`의 "99틱 건설 중, 100틱 완공"), 실제 타이머로 완공을 기다리는 통합 시험, 농장의 "건설 중 닫았다 열면 남은 시간 뒤 완공" 시험은 **규칙이 바뀌었으므로 새 규칙으로 고쳐 쓴다**. 단 같은 성질(정확히 그 순간에 바뀜, 그 전에는 안 바뀜, 방 사이 격리)을 밤 정산 기준으로 다시 시험해야 한다. 고친 시험은 이름과 이유를 이 문서 맨 아래 "구현 결과"에 적는다. 시작 집 때문에 결과가 바뀌는 이동/건설 시험은 시작 집이 있는 상태로 고치는 것을 먼저 하고, 시작 집과 정말 무관한 시험에만 `farmhouse_width_cm = 0`을 쓴다.

## 9. 완료 기준 (구현 전에 확정)

**서버**

1. **시계:** 새 방은 1일차 경과 0(06:00)이다. 경과 10틱마다 게임 1분이 지난다. 깨어 있는 사람이 있으면 **경과 11,999틱에는 정산하지 않고 12,000틱에 강제 정산**한다. `bench_day_ticks`를 주면 그 길이를 쓴다.
2. **잠 판정:** 시작 집과 완공된 집 앞에서는 잘 수 있다. 거리 경계를 포함한다(150.0 허용, 150.1 거부). 건설 중인 집 앞은 `GAME_SLEEP_TOO_FAR`다. 이미 잠든 사람이 잔다고 하면 성공이고 변화가 없다. 깨어나기가 된다. 거부되면 상태가 바뀌지 않는다.
3. **잠든 동안:** 이동 입력이 무시되고(위치와 `last_input_seq`가 그대로), 채집과 건설은 `GAME_SLEEPING`이며 인벤토리, 노드, 구조물, 쿨다운이 바뀌지 않는다. 판정 순서(6절)도 시험한다.
4. **정산 조건:** 모두 잠들면 다음 틱에 정산한다. 한 명이라도 깨어 있으면 하지 않는다. 깨어 있던 마지막 사람이 나가면 다음 틱에 정산한다. 새로 들어온 사람(깨어 있음)이 정산을 막는다. 02:00 강제 정산은 위치와 상관없이 일어나고 `forced = true`다. 따라잡기 틱이 경계를 넘어도 정산은 한 번이다.
5. **정산 내용과 순서:** 건설 중인 모든 집이 완공되고, 고갈된 모든 노드가 되살아나고(재생 자리 규칙 유지, 못 찾은 노드는 20틱마다 재시도), 모두 깨어나 자기 칸의 시작 위치에 방향 (0, 0)으로 서고, `day`가 1 늘고, 새 날이 그 틱에 시작한다. **같은 상태에서 정산하면 항상 같은 결과다**(노드 자리 포함, 결정성).
6. **옛 시간 규칙이 없어졌다:** 고갈된 노드는 600틱이 지나도, 밤 정산 전에는 고갈 상태다. 지은 집은 100틱이 지나도 건설 중이다(충돌은 처음부터 있다).
7. **알림:** 정산한 틱에 방 전원이 `DayStarted`를 **그 틱의 다른 메시지보다 먼저** 받고, 이어서 아침 위치의 스냅샷, 되살아난 노드, 완공된 집을 받는다. `SleepUpdate`는 목록이 바뀐 틱에만 간다. 늦게 들어온 사람은 `RoomState`로 시계, 시작 집, 잠든 사람을 받는다. 다른 방에는 영향이 없다(격리).
8. **시작 집:** 걸어서 들어갈 수 없다(부풀린 방향 입력, 모서리 포함). 겹치게 지으면 `GAME_BUILD_BLOCKED`다. 노드가 그 위에 생기지 않는다. 시작 칸 4개가 막히지 않는다. 너비 0이면 없다.
9. **저장(인메모리 저장소):** 밤 정산 때 구조물과 시계가 바로 저장된다. 농장이 닫힐 때 시계가 저장된다. 다시 열면 같은 날, 같은 경과에서 이어진다(예: 경과 5,000에 닫으면 다시 연 뒤 7,000틱 뒤에 강제 정산). 닫혀 있는 동안 시간이 흐르지 않는다. 옛 데이터(시계 행 없음)는 1일차 경과 0이다. 저장된 건설 중인 집은 `remaining_ticks`와 상관없이 다음 밤에 완공된다. 잘못된 경과(하루 길이 이상)는 다음 틱에 강제 정산된다. 정산 저장이 실패해도 게임은 계속되고 다음 주기에 다시 저장된다. 게스트 방은 저장하지 않는다. 시작 집 자리에 저장된 집이 있어도 복원하다 죽지 않는다.
10. **실제 MySQL(`account_mysql_test`, 사용자가 실행):** `farm_clock` 왕복, 같은 값 두 번 저장(멱등), 트랜잭션 중간 실패 시 시계도 반영되지 않음, **서버를 껐다 켜도 날과 시각이 복원됨**.
11. **견고함:** 방에 없는 연결의 잠 요청, 같은 틱 안의 잠/깸 반복, 잠든 채 연결 끊김, 정산 알림을 보내는 중 연결 끊김, 정산 틱에 다른 사람의 나가기에서 죽지 않는다.
12. **밤 정산 비용 측정 (측정 전에 확정):**
    - 질문: 밤 정산이 그 틱을 밀어 4단계 합격 기준을 깨는가.
    - 조건: Release, `--io-threads 0`, 방 400개(4단계에서 대부분의 구성이 3/3 합격한 수), 생성기 설정은 [stage4.md](stage4.md)의 게임 부하 측정과 같다. **A** = 기본 하루(측정 35초 안에 정산이 없다), **B** = `--bench-day-ticks 200`(방마다 10초마다 강제 정산, 측정 구간에 방마다 약 3번). 생성기가 방을 거의 동시에 만들므로 B는 여러 방의 정산이 같은 틱 근처에 몰리는, **나쁜 쪽에 가까운 조건**이다.
    - 같은 세션에서 A와 B를 번갈아 3회씩, 매 실행 새 서버, 실행 전 PC 상태(배경 프로그램)를 기록한다(입출력 분리 측정과 같은 규칙).
    - 합격 기준: [stage4.md](stage4.md)의 표 그대로(스냅샷 도착 간격 p99 100 ms 이하, 입력 반영 지연 p99 100 ms 이하, 누락 1% 이하, 틱 건너뜀 0.1% 이하, 정합성 위반 0, 생성기 포화 아님). B도 이 기준을 만족해야 "밤 정산이 틱을 밀지 않는다"고 쓴다.
    - 함께 기록: `night_us` 합계, `max_night_micros`, 방 하나의 정산 평균 시간, 받은 `DayStarted` 수.
    - B가 불합격이면 그 결과를 그대로 적고 **이 조각에서는 최적화하지 않는다**(원인 분해는 다음 일로 남긴다). 측정을 하고 기록하면 이 기준은 충족이다(합격 여부와 무관). 결과는 `results/stage4/night-cost.md`와 원본에 둔다.
13. **기존 기능 유지:** 기존 `ctest`가 통과한다. 8절의 "기존 시험에 생기는 변화"로 고친 시험은 목록과 이유를 남긴다.
14. **변형 검증:** 고의로 버그를 심어 시험이 잡는지 확인한다. 최소 목록: 잠 거리 `>`를 `>=`로, 건설 중인 집도 잘 수 있게, "모두 잠듦"을 "누구든 잠듦"으로, 강제 정산 경계를 하나 어긋나게, 정산에서 완공 빠뜨림, 재생 빠뜨림, 아침 위치로 안 옮김, 잠을 안 깨움, `day` 안 늘림, `day_start_tick` 안 바꿈, 잠든 사람의 이동 입력 반영, 잠든 채 채집 허용, 잠든 채 건설 허용, 시작 집을 장애물에서 뺌, 시작 집 겹침 검사 뺌, 옛 600틱 재생을 남김, 옛 100틱 완공을 남김, `DayStarted`를 스냅샷 뒤에 보냄, `SleepUpdate`를 매 틱 보냄, 늦은 입장자의 `RoomState`에서 시계 뺌, 닫을 때 시계 저장 뺌, 복원 때 경과 무시, 정산 저장을 주기 저장으로 미룸. 살아남은 변형은 시험을 보강하거나 동등 변형이라는 근거를 적는다.
15. **문서:** 이 문서의 "구현 결과", [stage4.md](stage4.md) 프로토콜 표(116~119, 결과 코드 21~22, `ready_tick`의 뜻), [persistence.md](persistence.md) 스키마 표(`farm_clock`), [unreal-client.md](unreal-client.md) 기준 바이트, CLAUDE.md의 명령(`/sleep`, `/wake`, `/clock`, `--bench-day-ticks`)과 코드 구조.

**Unreal**

16. **자동 시험(`GameClientSpike`, 진짜 서버를 `--bench-day-ticks 200`으로):** 시작 집에서 멀면 잠이 거부되고 집 앞에서는 된다. 혼자 잠들면 다음 날이 오고, 위치가 시작 칸이 되고, 날 번호가 1 는다. 둘 중 하나만 자면 넘어가지 않는다. 강제 정산(`forced`)이 온다. 지은 집이 다음 날 완공된다. 깎은 나무가 다음 날 돌아온다. 잠든 채 채집하면 `Sleeping` 결과가 온다. 늦게 들어온 사람의 시계가 맞다. 순수 함수 모드(`dayrules`, 서버 불필요): 시각 계산과 표시 문구, 잠 가능 거리 판정(경계 포함), 음수 `day_start_tick`.
17. **PIE(사람이 확인):** 창 2개. 시계가 흐른다. 한쪽만 자면 "잠든 사람 1/2"가 보이고 넘어가지 않는다. 둘 다 자면 화면이 전환되고 2일차 06:00에 시작 집 앞에서 깨어난다. 전날 지은 집이 완공되어 있고, 전날 벤 나무가 돌아와 있다.

## 10. Unreal 설계 (요약)

| 대상 | 바꿀 것 |
| --- | --- |
| `FGameClient` | `Sleep(bool)`, 상태 `GetClock()`, `IsSleeping()`, `GetSleepingPlayers()`, `GetLandmarks()`, 이벤트 `OnSleepResult`, `OnSleepStateChanged`, `OnDayStarted`. 결과 코드 `SleepTooFar`, `Sleeping` |
| `FGameSnapshotInterpolator` | `DayStarted`를 받으면 초기화(순간이동을 보간하지 않는다) |
| `UYongNetSubsystem` | 블루프린트용 `Sleep`, `GetDay`, `GetGameMinute`(마지막 스냅샷 틱에서 계산), `IsSleeping`, `GetSleepingCount`, 이벤트 |
| `YongUi::` | `FormatClock(day, minute)`(예: `1일차 06:00`), `CanSleepNear(pos, structures, landmarks)`(대략 판정, 서버가 최종), `DescribeGameResult`에 새 코드 두 개 |
| 컨트롤러 | `F`: 집 앞이면 잠자기/깨어나기. 잠든 동안 이동 입력을 보내지 않는다 |
| `AYongLandmarkActor` | 고정 물체 하나의 표현. 블루프린트 자식 `BP_Farmhouse`가 메시를 정한다(처음에는 기본 도형이어도 된다). 충돌 없음(판정은 서버) |
| 구조물 액터 | 건설 중이면 진행률 대신 "내일 완공" 표시. `ready_tick`을 진행률에 쓰고 있었다면 고친다 |
| HUD | 선택 위젯 `ClockText`(날과 시각), `SleepStatusText`("잠든 사람 1/2"), `SleepHintText`("F: 잠자기"), 새 날 전환(페이드). 조명 회전은 선택 |

작업 순서는 지금까지와 같다: **S0 서버 → S1 Unreal C++(에디터 닫음) → S2 MCP 에셋(에디터 엶) → S3 사람의 PIE 확인.** MCP 작업: `BP_Farmhouse`, `BP_YongRoomGameMode`에 랜드마크 클래스 지정, `WBP_HUD`에 세 텍스트, 입력 매핑에 `F`.

## 11. 위험

- **두 시간 체계가 섞이는 기간:** 틱 기준 재생/완공을 없애는 것이 이 조각의 가장 큰 변경이다. 남은 틱 기준 코드(쿨다운, 재시도 재생, 저장 주기)는 그대로 틱이다. 무엇이 "하루"이고 무엇이 "틱"인지 `PlayConfig` 주석에 적는다.
- **정산 틱의 비용:** 한 틱에 노드 50개 재생 자리 찾기와 저장 요청이 몰린다. 방이 많고 정산이 같은 틱에 몰리면 그 틱이 길어질 수 있다. 완료 기준 12가 이것을 잰다.
- **게임이 쉬워진다:** 하루에 모을 수 있는 자원이 노드 체력 합(목재 150, 석재 160)으로 정해진다. 30초 재생 때보다 하루에 짓는 집이 줄어든다. 의도된 변화이고 값은 `PlayConfig`다.
- **혼자 하는 사람:** 혼자라면 언제든 자서 바로 다음 날로 갈 수 있다. 막지 않는다(스타듀밸리와 같다).
- **시작 집과 옛 데이터의 겹침**(5.6).

## 12. 확인 방법

**터미널 체험(Windows, Release):**

```powershell
build/windows-local/Release/game_server.exe --port 7777 --log-level info --bench-day-ticks 600              # 터미널 1: 하루 30초
build/windows-local/Release/game_client.exe --port 7777 --create --nick alice --snap 0                     # 터미널 2 (방 코드가 출력됨)
build/windows-local/Release/game_client.exe --port 7777 --join <방코드> --nick bob --snap 0               # 터미널 3
```

두 창에서 `/move 0 1`을 치고 0.5초쯤 뒤 `/stop`(시작 칸 0, 1은 시작 집에서 300 cm라서 북쪽으로 걸어가야 한다. 칸 2, 3은 바로 잘 수 있다), 그다음 `/sleep`. 실제 출력(요약):

```
[잠] 시작 집이나 완공된 집 앞(150 cm 이내)에서만 잘 수 있습니다 / 지금 깨어 있음     <- 걷기 전에 잤을 때
[잠] 성공 / 지금 잠들어 있음
[잠] 틱 44: 잠든 사람 1/2: alice                                                    <- 한쪽만 자면 넘어가지 않는다
[시계] 1일차 06:05 (틱 53, 하루 600틱) / 잠든 사람 1/2: alice / 나는 잠들어 있음
[채집] 노드 1: 잠든 동안에는 할 수 없습니다 (/wake) / 내 목재 0 석재 0
[새 날] 틱 64: 2일차 06:00 시작 (모두 잠듦). 모두 깨어나 시작 위치로 옮겨졌습니다     <- 둘 다 잔 다음 틱
[잠] 틱 64: 잠든 사람 0/2: 없음
[시계] 2일차 06:01 (틱 75, 하루 600틱) / 잠든 사람 0/2: 없음
```

아무도 자지 않으면 하루(600틱 = 30초)가 끝날 때 `[새 날] … (02:00 강제 정산)`이 나온다. 방에 들어갈 때 `시작 집 @(2500, 2850) 400x300 cm`와 `[시계]`가 함께 출력된다.

**부하 생성기가 밤 정산을 견디는지(짧은 확인):** 서버를 `--bench-day-ticks 100`으로 켜고 `game_load_client --rooms 20 --warmup 2 --duration 10`을 돌리면 결과에 `밤 정산 : 40번(방마다 셈), DayStarted 수신 160건(연결마다 셈)`이 나오고 정합성 위반 0, 합격이었다. `DayStarted` 다음 스냅샷의 거리 검사 건너뛰기를 빼고 같은 실행을 하면 위반 636건으로 불합격이었다(그 처리가 필요하다는 확인).

**밤 정산 비용 측정(완료 기준 12, 사용자가 실행):** Release로 빌드한 뒤 저장소 폴더에서

```powershell
powershell -ExecutionPolicy Bypass -File results\stage4\run-night-cost.ps1
```

조건 A(기본 하루)와 B(`--bench-day-ticks 200`)를 방 400개, `--io-threads 0`으로 번갈아 3회씩 돌린다(매 실행 새 서버, 실행 전 PC CPU 기록, 바쁘면 기다렸다가 그래도 바쁘면 `CONTAMINATED` 표시). 원본은 `results/stage4/raw/`(`game-night-*.txt`, `game-server-night-*.log.gz`, `game-load-night.csv`, `game-load-night-progress.txt`, `game-load-night-environment.txt`)에 남는다. 함께 기록할 값: 서버 통계 줄 끝의 `하루{정산= 강제= 정산시간=…us 최대정산=…us}`(측정 구간 시각으로 잘라 증가분을 본다)와 클라이언트 출력의 `밤 정산` 줄. 방 하나의 정산 평균 시간 = 정산시간 증가분 / 정산 증가분. 결과는 `results/stage4/night-cost.md`에 쓴다.

## 13. 구현 결과

### 진행 상태

| 단계 | 내용 | 상태 |
| --- | --- | --- |
| S0-1 | `PlayConfig` 값, `Room`/`ResourceField`/`StructureSet`/`World`의 순수 로직(시계, 잠, 정산 판정, `RunNight`, 시작 집 장애물), 단위 시험 | 완료(Windows) |
| S0-2 | proto(116~119, `DayClock`, `Landmark`, `RoomState` 7~9, 결과 코드 21~22), `GameService`(`HandleSleep`, 틱 순서, 알림 순서, 통계), `Session` 연결, 통합 시험 | 완료(Windows) |
| S0-3 | 시계 저장(마이그레이션 004, `StoredClock`, 저장소, `FarmPersistence` 저장 시점, 복원 규칙) | 완료(Windows, 인메모리 저장소로 시험. 실제 MySQL 시험은 사용자가 실행) |
| S0-4 | `game_client`(`/sleep`, `/wake`, `/clock`), `game_load_client`(새 메시지 견디기), 서버 `--bench-day-ticks` | 완료(Windows, 실제 실행으로 확인, 12절). 측정 스크립트 `results/stage4/run-night-cost.ps1`까지 준비, 측정은 사용자 |
| S0-5 | 완료 기준 14의 변형 검증 | 완료(37가지: 35가지 잡음, 1가지 시험 보강 뒤 잡음, 1가지 동등 변형. 아래 "변형 검증 결과") |
| S0-6 | 문서(stage4.md 프로토콜 표, persistence.md 스키마, unreal-client.md 기준 바이트, CLAUDE.md 명령과 코드 구조) | 남음(이 절, CLAUDE.md 현재 상태와 계정 MySQL 시험 명령만 갱신) |
| S1~S3 | Unreal | 남음 |

**Unreal S1 전까지의 한계:** 현재 Unreal 클라이언트는 117~119를 모르므로, 10분(기본 하루)이 넘게 접속해 있으면 `DayStarted`를 받는다(어떻게 처리하는지는 S1에서 확인한다). 운영 DB에는 `db_migrate`로 004를 적용해야 `--storage mysql` 서버가 농장 저장소 문장을 준비할 수 있다(없으면 농장 열기가 `GAME_STORAGE_UNAVAILABLE`).

### 구현 중 정한 세부 사항

- **고정 물체:** `src/play/landmark.h`(`Landmark`, `MakeLandmarks(config)`). 시작 집 너비나 높이가 0 이하이면 없다. `World::SetFixedObstacles`로 이동을 막고, `Room::Build`의 겹침 판정과 재생 자리(`Room::MakeSpotBlockers`, 공개 함수)에 들어간다. `bench_no_collision`이면 시작 집도 이동을 막지 않는다(측정 전용 설정의 뜻을 그대로 따름).
- **노드 처음 배치도 시작 집을 피한다.** 설계는 재생 자리만 말했지만 처음 배치도 같은 규칙으로 했다. 기본값에서는 시작 집이 건설 구역 안이라 결과가 같고, 구역을 끈 설정에서만 차이가 난다.
- **재생:** `ResourceField::Hit`은 재생을 예약하지 않는다(고갈된 노드의 `respawn_tick`은 `kNoRetry`). `RespawnAllDepleted(tick, blockers)`가 밤 정산의 재생이고, 자리를 못 찾은 노드만 `tick + respawn_retry_ticks`로 재시도를 예약한다. `Advance`는 그 재시도만 한다. 다음 밤 정산이 먼저 되살리면 남은 재시도 예약은 지운다.
- **완공:** `StructureSet::Add`는 `ready_tick = 0`으로 만들고, `CompleteAll(tick)`이 밤 정산의 완공이다(`ready_tick = tick`). 복원은 저장된 `remaining_ticks`와 상관없이 건설 중이면 건설 중(`ready_tick = 0`). 저장하는 `remaining_ticks`는 항상 0이다.
- **이동 입력:** `Room::ApplyInput`이 잠든 사람의 입력을 `InputResult::kSleeping`으로 통째로 버린다(입력 번호도 그대로). 통계는 `inputs_ignored`에 센다.
- **강제 정산:** 깨어 있는 사람을 잠든 상태로 바꾼 뒤 3)에서 모두 깨운다. 밖에서 보이는 차이는 없다.
- **`SleepUpdate` 조건:** 마지막으로 보낸 목록과 지금 목록이 다를 때만 보낸다(`Room::TakeSleepChange`). 그래서 같은 틱 안의 잠/깸 반복, 잠든 사람이 없던 강제 정산, 틱 사이에 잠들었다 바로 나간 경우에는 보내지 않는다. 정산 전에 잠든 사람이 있었으면 정산 틱에 빈 목록을 보낸다.
- **농장의 밤 정산 저장:** 완공된 집이 없어도 정산마다 구조물을 대기열에 넣고 바로 저장한다(S0-3에서 시계가 같은 저장에 들어간다).
- **통계:** 설계의 "틱 시간 분해의 새 항목"은 `틱시간{}` 묶음 안에 넣지 않고 별도 묶음으로 두었다. `results/stage4/analyze_*.py`가 `틱시간\{이동= 갱신= 구성= 전송= 합=\}` 모양을 정규식으로 그대로 읽기 때문이다. 줄 모양: `하루{정산=N 강제=N 정산시간=Nus 최대정산=Nus 잠=N 잠거부=N}`. 정산 시간은 `갱신`에서 빼고 따로 센다(합 = 이동 + 갱신 + 구성 + 전송 + 정산 + 기타). `잠`, `잠거부`는 설계에 없던 항목이고, 잠 요청 거부는 `requests_rejected`에 세지 않는다. `aggregate.py`의 정규식(`최대틱처리=…us[^}]*\}`)은 영향이 없다.

### 시계 저장에서 정한 세부 사항 (S0-3)

- **저장 형식:** `farm::StoredClock{day, day_elapsed_ticks}`. `FarmData::clock`과 `FarmSave::clock`은 `std::optional`이고, `FarmSave`에 시계가 없으면 시계 행을 건드리지 않는다. MySQL 테이블 이름 옵션 `FarmTables::clock`(기본 `farm_clock`, 시험은 `farm_clock_test`).
- **트랜잭션 안의 순서:** MySQL 구현은 시계를 **가장 먼저** upsert한다. 뒤의 구조물/인벤토리 문장이 실패하면 시계도 함께 취소되는지를 시험이 확인할 수 있게 하려는 순서다(시험: `ASaveThatFailsHalfwayChangesNothing`).
- **"주기 저장은 어차피 저장할 때만 싣는다"의 구현:** `FarmPersistence::SetClockSource`. `GameService`가 열린 농장의 지금 시계를 알려 주는 함수를 넘기고, `Flush`가 저장 묶음을 만들 때 그 값을 싣는다. 열린 농장이면 이 값이 대기열의 시계보다 우선한다(가장 새 값). 닫힌 농장은 대기열의 값(닫힌 시각)을 쓴다. 시계 출처는 대기열을 비어 있지 않게 만들지 않으므로, 시계만 흐른 농장은 주기마다 쓰지 않는다. `UpdateClock`(밤 정산, 농장 닫힘, 서버 종료)은 대기열에 넣어 저장을 일으킨다.
- **실패한 시계:** 대기열로 되돌린다. 그사이 더 새 시계가 들어왔으면 새 값이 이긴다(다른 값과 같은 규칙).
- **복원:** `FinishOpenFarm`이 새로 여는 농장에만 `Room::RestoreClock`을 부른다(이미 열려 있는 농장에 들어가는 경우는 방의 시계 그대로). 저장된 경과가 하루 길이 이상이면 경고 로그를 남긴다(같으면 자르지 않아도 다음 틱에 강제 정산된다).
- **열린 농장 찾기:** 시계 출처가 farm_id로 방을 찾도록 `GameService::open_farm_codes_`(farm_id → 방 코드)를 둔다. 농장이 닫힐 때 지운다.

### 도구에서 정한 세부 사항 (S0-4)

- **서버 `--bench-day-ticks N`:** 0~1,000,000,000(0이면 기본 하루). 시작 로그에 `하루=N틱(bench)`를 남겨 측정 기록에 설정이 남게 했다.
- **`game_client`:** `/sleep`, `/wake`, `/clock`. 시각은 마지막 스냅샷 틱과 방 상태/`DayStarted`의 `DayClock`으로 계산해 `HH:MM`(24시간을 넘긴 새벽은 시계 모양 01:59 등)으로 보인다. 방 상태를 받으면 시작 집과 시계를 출력한다. `SleepUpdate`는 "잠든 사람 n/전체: 닉네임"으로, `DayStarted`는 날과 강제 여부로 출력한다. 밤 정산처럼 노드가 6개 이상 한꺼번에 바뀐 `ResourceUpdate`는 한 줄로 줄인다(노드 위치도 갱신해 `/nodes`의 거리가 맞게 했다).
- **`game_load_client`:** 원래 모르는 메시지를 읽고 버리고 있었다. `DayStarted`를 받으면 그 연결의 다음 스냅샷 한 번은 틱당 이동 거리 검사를 건너뛴다(위치는 기억해 둔다). 측정 구간에 받은 수를 `밤 정산 : N번(방마다 셈), DayStarted 수신 M건(연결마다 셈)`으로 출력한다. **CSV에는 열을 더하지 않았다**: 기존 CSV 파일에 줄을 덧붙이는 방식이라 열이 바뀌면 이전 결과와 한 파일에 섞일 때 깨지기 때문이다. 측정 기록은 출력(`game-*.txt`)에서 읽는다.
- **측정 스크립트:** `results/stage4/run-night-cost.ps1`(입출력 분리 측정의 `run-load-io.ps1`과 같은 규칙, ASCII만).

### 고친 기존 시험과 이유

규칙이 바뀐 시험은 같은 성질(정확히 그 순간에 바뀜, 그 전에는 안 바뀜, 방 사이 격리)을 밤 정산 기준으로 다시 시험한다. "그 전에는 안 바뀜"은 옛 시각(600틱, 100틱)이 지나도 바뀌지 않는 것으로 확인한다.

| 파일 | 시험(옛 이름 → 새 이름) | 이유 |
| --- | --- | --- |
| play_resources_test | `RespawnsExactlyAfterTheConfiguredTicks` → `ADepletedNodeStaysDepletedThroughTheDayAndComesBackAtTheNight` | 재생이 밤 정산으로 |
| play_resources_test | `RoomGatherTest.DepletedNodeRespawnsAfterExactlyTheConfiguredTicks` → `ADepletedNodeStaysDepletedAllDayAndReturnsAfterTheNight` | 같음 |
| play_resources_test | `RespawnLeavesTheStructureSpot…`, `StaysDepletedWhileNoSpotIsFree…`, `ARetryThatFailsAgain…`, `RespawnedNodesNeverLandInsideTheBuildZone`, `RespawnIsReportedAsAChange` | 첫 재생 시도를 `Advance` 대신 `RespawnAllDepleted`(밤)로. 재시도 간격 시험은 그대로 |
| play_structures_test | `NumbersFromOneStartsBuildingAndCompletesAtTheExactTick` → `NumbersFromOneStartBuildingAndAllCompleteAtTheNight` | 완공이 밤 정산으로 |
| play_structures_test | `RoomBuildTest.TheBuildingCompletesAfterExactlyTheConfiguredTicks` → `TheBuildingStaysUnderConstructionAllDayAndCompletesAtTheNight` | 같음 |
| play_structures_test | `ChangesAreCollectedOnce…`, `CompletionIsReportedAsAChange`, `BuildingSpendsTheExactCost…` | `CompleteAll`, `ready_tick` 0 |
| play_structures_test | `StructuresMayTouchButNotOverlap`, `WhenTwoPlayersBuild…`, `TheInventoryIsLost…` | 좌표가 시작 집과 겹쳐 시작 집 밖으로 옮김 |
| play_collision_test | `NoInputSequenceEverGetsInsideAnObstacle` | 집 두 채를 시작 집 밖으로 옮기고 겹침 검사에 시작 집을 포함 |
| play_collision_test | `ARespawningNodeUnderThePlayerDoesNotTrapThem` | 밤 정산은 사람을 아침 위치로 먼저 옮기므로, 지금 위치 기준 재생 규칙을 `RespawnAllDepleted(tick, MakeSpotBlockers())`로 직접 시험 |
| game_service_test | `DepletedNodeComesBackAfterExactlyTheRespawnTicks…` → `ADepletedNodeStaysDepletedUntilTheNightAndThenTheRoomIsTold` | 재생이 밤 정산으로(모두 잠들어 넘김) |
| game_service_test | `ACatchUpTickStillDeliversEveryChangeAndRespawnsInOrder`, `ACatchUpTickStillDeliversTheCompletion` | 따라잡기 틱이 하루 끝을 넘어도 정산이 한 번인지로 |
| game_service_test | `BuildingRepliesAtOnce…ThenOnCompletion`, `ALateJoinerReceivesTheCurrentStructures…` | 완공을 밤 정산(잠, 강제)으로. 둘째 집 좌표를 시작 집 밖으로 |
| game_service_test | `TheServerStopsAPlayerAtABuiltWall…` 외 건설 좌표 (2400, 2800), (2500, 2800)을 쓰던 시험들 | 시작 집과 겹쳐 (2400, 2000) 등으로 옮김. 벽 시험은 남쪽으로 걷는다 |
| game_integration_test | `BuildingOverTcp…Completes` → `…CompletesAtTheNight`, `ADepletedNodeRespawnsOnTheRealTimer…` → `…AtTheNightOnTheRealTimer` | 실제 타이머 시험은 `bench_day_ticks`(20, 10)로 짧은 하루 |
| game_integration_test | `APlayerWalkingIntoABuiltHouse…`, `TheTickKeepsRunning…`, 건설 좌표 | 시작 집과 겹쳐 옮김 |
| farm_integration_test | `CompletionIsSavedImmediately` → `CompletionAtTheNightIsSavedImmediately`, `BuildingIsSavedImmediately…`(남은 틱 0) | 완공이 밤 정산으로 |
| farm_integration_test | `ConstructionProgressIsSavedAndTheClosedFarmDoesNotAdvanceTime` → `AHouseClosedUnderConstructionIsStillBuildingWhenReopenedAndCompletesAtTheNextNight` | 남은 틱 규칙이 없어졌다. "닫혀 있는 동안 시간이 흐르지 않는다"는 S0-3의 시계 저장 시험에서 다시 확인한다 |
| account_mysql_test | `FarmServerConfig`(`build_ticks` → `bench_day_ticks` 10, 인자로 바꿀 수 있게), 건설 좌표 | 같음. **실행하지 못했다(사용자가 실행)** |
| account_mysql_test | `ResetTestTables`(004도 적용, `farm_clock_test`), `ACreatedFarmCanBeFoundByItsOwnerOnly`(시계 없음), `ASaveThatFailsHalfwayChangesNothing`(시계도 취소) | S0-3 시계 저장 |

시험 설정 `FarmConfig`/`BuildServerConfig`/`GatherServerConfig`의 인자는 `bench_day_ticks`로 바뀌었고 기본값 0(시험 중에 밤이 오지 않음)이다. 밤이 오면 농장이 바로 저장되므로, 주기 저장을 확인하는 시험에 밤이 섞이지 않게 했다.

### 변형 검증 결과 (S0-5)

변형을 하나씩 심고 → Debug로 빌드 → 단위/통합 시험 11개(`play_day_test`, `play_resources_test`, `play_structures_test`, `play_collision_test`, `play_room_test`, `play_world_test`, `game_service_test`, `farm_persistence_test`, `farm_service_test`, `game_integration_test`, `farm_integration_test`)를 모두 돌리고 → 되돌렸다. 완료 기준 14의 목록 23가지(경계 둘, 옛 재생 둘로 나눠 25개)와, 빠진 곳을 보려고 더한 12가지다. 아래 "잡은 시험"은 실패한 시험의 일부다.

| 번호 | 변형 | 결과 | 잡은 시험(일부) |
| --- | --- | --- | --- |
| M01 | 잠 거리 경계(`<=`를 `<`로, 설계의 `>`를 `>=`로와 같음) | 잡음 | `SleepTest.TheFarmhouseRangeIncludesTheBoundary` 외 |
| M02 | 건설 중인 집 앞에서도 잘 수 있게 | 잡음 | `SleepTest.ACompletedHouseCanBeSleptAtButAHouseUnderConstructionCannot` |
| M03 | "모두 잠듦"을 "누구든 잠듦"으로 | 잡음 | `NightTriggerTest.*` 2, `DayServiceTest` 6, TCP 2 |
| M04a | 강제 정산 경계 하나 늦게 | 잡음 | `DayClockTest.WithSomeoneAwake…`, `TheDefaultDayIsForcedAtExactly12000…` 외 |
| M04b | 강제 정산 경계 하나 이르게 | 잡음 | 같음, 실제 타이머 TCP 1 |
| M05 | 정산에서 완공 빠뜨림 | 잡음 | `NightTest`, 건설 서비스/통합/농장 통합 |
| M06 | 정산에서 재생 빠뜨림 | 잡음 | `NightTest`, 채집 서비스/통합 |
| M07 | 아침 위치로 안 옮김 | 잡음 | `NightTest.TheNightCompletes…`, `TheDefaultDayIsForced…` |
| M08 | 잠을 안 깨움 | 잡음 | `NightTest`, 서비스, 농장, TCP |
| M09 | `day` 안 늘림 | 잡음 | `NightTest`, 서비스, 농장 |
| M10 | `day_start_tick` 안 바꿈 | 잡음 | `NightTest`, 따라잡기 틱 시험 |
| M11 | 잠든 사람의 이동 입력 반영 | 잡음 | `SleepTest.MoveInputsWhileAsleep…`, `DayServiceTest.MoveInputsFromASleeper…`, TCP |
| M12 | 잠든 채 채집 허용 | 잡음 | `RoomGatherTest.ASleepingPlayerCannotGather…`, 서비스, TCP |
| M13 | 잠든 채 건설 허용 | 잡음 | `RoomBuildTest.ASleepingPlayerCannotBuild…`, 서비스, TCP |
| M14 | 시작 집을 장애물에서 뺌 | 잡음 | `CollisionTest.NobodyCanWalkIntoTheFarmhouse…` 외 3 |
| M15 | 시작 집 겹침 검사 뺌 | 잡음 | `RoomBuildTest.NothingCanBeBuiltOverlappingTheFarmhouse…` 외 |
| M16a | 옛 600틱 재생을 남김(600틱마다 고갈 노드 재생) | 잡음 | `GatherServiceTest.ADepletedNodeStaysDepletedUntilTheNight…` |
| M16b | 옛 재생을 남김(고갈 노드를 낮의 재시도가 되살림) | 잡음 | `ResourceFieldTest.ADepletedNodeStaysDepletedThroughTheDay…` 외 |
| M17 | 옛 100틱 완공을 남김 | 잡음 | `BuildServiceTest.BuildingRepliesAtOnce…ThenOnCompletion` |
| M18 | `DayStarted`를 스냅샷 뒤에 보냄 | 잡음 | `DayServiceTest.WhenEveryoneSleeps…DayStartedSentFirst`, TCP |
| M19 | `SleepUpdate`를 매 틱 보냄 | 잡음 | `SleepTest.TheSleepChangeIsReported…`, `DayServiceTest.SleepUpdateGoesToEveryone…` |
| M20 | 늦은 입장자의 `RoomState`에서 시계 뺌 | 잡음 | `DayServiceTest.ALateJoinerReceivesTheClock…`, 농장 서비스/통합 |
| M21 | 닫을 때 시계 저장 뺌 | **살아남음(동등 변형)** | 아래 |
| M22 | 복원 때 경과 무시 | 잡음 | `FarmClock.ClosingSavesTheClock…`, 농장 통합 |
| M23 | 정산 저장을 주기 저장으로 미룸 | 잡음 | `FarmClock.TheNightSavesTheNewDayAtOnce…`, 농장 통합 |
| X01 | 재생을 깨우기 전에(5.4절 순서 3과 4를 바꿈) | **처음엔 살아남음 → 시험 보강 뒤 잡음** | 새 시험 `NightTest.NodesRespawnAroundTheMorningSpotsSoNobodyIsPushedAsideAtDawn` |
| X02 | `DayStarted.forced`를 늘 false로 | 잡음 | `TheDefaultDayIsForced…`, 농장, 실제 타이머 TCP |
| X03 | `RoomState`에서 잠든 사람 뺌 | 잡음 | `DayServiceTest.ALateJoiner…` |
| X04 | `RoomState`에서 시작 집 뺌 | 잡음 | 같음, TCP |
| X05 | 복원 경과를 하루 길이로 자르지 않음 | 잡음 | `DayClockTest.AnElapsedLongerThanTheDay…`, `FarmClock.AStoredElapsedLonger…` |
| X06 | 잠들 때 멈추지 않음 | 잡음 | `SleepTest.FallingAsleepStopsTheBody…` |
| X07 | `Session`이 `SleepRequest`를 게임으로 넘기지 않음 | 잡음 | TCP 4 |
| X08 | 클라이언트가 보낸 `DayStarted`(119)를 요청으로 받음 | 잡음 | `GameServerOnlyIdTest`(119) |
| X09 | 열린 농장의 지금 시계를 싣지 않음(시계 출처 제거) | 잡음 | `FarmClock.ThePeriodicSaveDoesNotWrite…`, `AFailedNightSave…` |
| X10 | 실패한 시계를 대기열로 되돌리지 않음 | 잡음 | `FarmPersistence.AFailedSaveKeepsTheClock…`, `FarmClock.AFailedNightSave…` |
| X11 | 노드 재생 자리가 시작 집을 피하지 않음 | 잡음 | `CollisionTest.RespawnedNodesNeverLandOnTheFarmhouse` |
| X12 | 잠든 사람이 나가 목록이 줄어도 알리지 않음 | 잡음 | `SleepTest.TheSleepChangeIsReported…`, TCP |

- **M21이 동등 변형인 근거:** 농장이 닫히는 것은 마지막 사람이 나갈 때뿐이고, 그때 `PersistLeaver`는 항상 그 사람의 인벤토리를 대기열에 넣고 바로 저장한다. 그 저장 묶음을 만들 때 방이 아직 지워지지 않았으므로(방은 `PersistLeaver` 뒤에 지운다) 시계 출처가 방의 지금 시계, 즉 닫힌 시각을 싣는다. 그래서 명시적인 `UpdateClock`을 빼도 저장되는 값이 같다. 두 장치가 겹쳐 있는 것이고, 시계 출처를 빼는 변형(X09)은 잡혔다. 명시적인 `UpdateClock`은 "닫힌 시각을 저장한다"는 규칙을 시계 출처의 호출 순서에 기대지 않게 하려고 남겨 둔다.
- **X01에서 보강한 시험:** 기본값에서는 노드가 건설 구역 밖에만 생기고 사람은 아침에 맵 중앙에 서므로 순서가 결과를 바꾸지 않는다(5.4절에도 적었다). 구역이 없는 작은 맵(1,000 x 1,000, 노드 자리 [200, 800])에서 사람이 밤마다 구석에 있게 하고 200번 정산해, 매번 아침에 자기 칸 (400, 400)에 정확히 서는지 본다. 재생이 먼저면 노드가 아침 자리에 나타나 사람이 빈 자리로 밀려난다.

### 검증 결과 (S0-3)

- `ctest` 전체(Debug): **S0-2 뒤 665개 → 684개(통과 647, MySQL 37 건너뜀, 실패 0)**, 두 번 실행해 같았다. 새로 더한 것:
  - `farm_persistence_test` 4개: 시계만으로 저장, 주기 저장은 다른 변경이 있을 때만 지금 시계를 실음, 열린 농장은 지금 시계/닫힌 농장은 넣어 둔 시계, 실패한 시계 보존과 새 값 우선.
  - `farm_service_test` 8개(시계를 직접 돌리는 결정적 시험): 밤 정산 저장이 주기 저장 없이 바로 됨, **경과 5,000에 닫고(그동안 서버의 다른 방은 3,000틱 흐름) 다시 열면 6,999틱에는 아직이고 7,000틱에 강제 정산**, 시계 행 없는 옛 농장은 1일차 경과 0, 하루 길이보다 긴 경과는 다음 틱에 강제 정산, 저장된 건설 중인 집은 남은 틱과 상관없이 다음 밤에 완공되어 저장, 밤 정산 저장 실패에도 게임이 계속되고 다음 주기에 저장, 시계만 흐른 농장은 주기 저장하지 않음, 서버 종료가 시계를 저장, 게스트 방은 저장하지 않음.
  - `farm_integration_test` 2개(실제 TCP와 타이머, 각각 `_io4` 포함): 밤 정산의 새 날 저장, 닫힌 시각에서 이어서 열림.
  - `account_mysql_test` 2개(**사용자가 실행**): 시계 왕복과 멱등, 시계를 싣지 않은 저장은 시계를 건드리지 않음 / 서버를 껐다 켜도 날과 시각(경과)이 복원됨. 기존 2개에 시계 확인을 더했다.
- 저장 관련 변형은 위 "변형 검증 결과"에 합쳤다(M21~M23, X05, X09, X10).

### 검증 결과 (S0-1, S0-2)

- `ctest` 전체(Debug): **이전 593개(통과 558, MySQL 35 건너뜀) → 이후 665개(통과 630, 건너뜀 35, 실패 0)**, 두 번 실행해 같았다. 새로 더한 것: `play_day_test` 28개(시계, 잠, 정산 조건, 정산 내용과 결정성, 재시도, 옛 데이터의 겹친 집), 시작 집 시험(충돌 5, 건설 4), `GameService`의 하루/잠 시험 15개, TCP 통합 4개(각각 `_io4` 반복 포함), 서버 전용 ID 117~119와 잘못된 `SleepRequest` 본문의 연결 종료.
- 변형 검증은 위 "변형 검증 결과"(S0-5)에 있다. S0-5에서 시험 1개(`NodesRespawnAroundTheMorningSpots…`)를 더해 `ctest`는 685개(통과 648, MySQL 37 건너뜀, 실패 0)가 되었다.
