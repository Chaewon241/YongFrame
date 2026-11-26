-- 농장의 시계(날 번호와 그날의 경과 틱) 테이블 (docs/content-day.md 7절).
--
-- 규칙은 001~003과 같다: 번호 순서로 적용하고, 적용한 파일은 고치지 않고, 여러 번 실행해도 안전하도록 IF NOT EXISTS를 쓴다.
-- 003(farms)이 먼저 적용되어 있어야 한다.
--
-- 농장 하나에 행 하나. 행이 없으면 서버는 1일차 경과 0으로 본다(이 표가 생기기 전에 만든 농장).
-- day_elapsed_ticks는 저장할 때의 경과 틱이다. 농장이 닫혀 있는 동안 시간은 흐르지 않으므로, 다시 열면 이 값에서 이어진다.
-- 서버가 "지금의 값"(절대값)으로 덮어쓴다(INSERT ... ON DUPLICATE KEY UPDATE).
CREATE TABLE IF NOT EXISTS farm_clock (
  farm_id            BIGINT UNSIGNED NOT NULL,
  day                INT UNSIGNED    NOT NULL DEFAULT 1,
  day_elapsed_ticks  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (farm_id),
  CONSTRAINT fk_farm_clock_farm FOREIGN KEY (farm_id) REFERENCES farms (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
