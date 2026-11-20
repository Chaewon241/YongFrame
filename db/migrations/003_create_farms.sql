-- 농장(영속하는 방)과 구조물, (농장, 계정)별 인벤토리 테이블 (docs/persistence.md).
--
-- 규칙은 001, 002와 같다: 번호 순서로 적용하고, 적용한 파일은 고치지 않고, 여러 번 실행해도 안전하도록 IF NOT EXISTS를 쓴다.
-- 002(accounts)가 먼저 적용되어 있어야 한다.
--
-- farms: 계정당 농장 1개(owner UNIQUE), 방 코드는 농장이 있는 동안 바뀌지 않고 겹치지 않는다(code UNIQUE).
-- farm_structures: 집 하나. structure_id는 농장 안 번호(1부터 이어진다). remaining_ticks는 건설 중일 때 완공까지 남은 틱
--   (농장이 닫혀 있는 동안은 시간이 흐르지 않는다). built_by_account_id는 지은 사람(모르면 0이라서 외래 키가 없다).
-- farm_players: 농장에서 각 계정이 가진 목재/석재. 행이 없으면 서버는 0으로 본다.
-- 모든 값은 서버가 "지금의 값"(절대값)으로 덮어쓴다(INSERT ... ON DUPLICATE KEY UPDATE).
CREATE TABLE IF NOT EXISTS farms (
  id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  code               CHAR(6)         NOT NULL,
  owner_account_id   BIGINT UNSIGNED NOT NULL,
  created_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_farms_code (code),
  UNIQUE KEY uq_farms_owner (owner_account_id),
  CONSTRAINT fk_farms_owner FOREIGN KEY (owner_account_id) REFERENCES accounts (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS farm_structures (
  farm_id             BIGINT UNSIGNED NOT NULL,
  structure_id        INT UNSIGNED    NOT NULL,
  type                INT UNSIGNED    NOT NULL,
  x                   FLOAT           NOT NULL,
  y                   FLOAT           NOT NULL,
  state               INT UNSIGNED    NOT NULL,
  remaining_ticks     BIGINT UNSIGNED NOT NULL DEFAULT 0,
  built_by_account_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (farm_id, structure_id),
  CONSTRAINT fk_farm_structures_farm FOREIGN KEY (farm_id) REFERENCES farms (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS farm_players (
  farm_id            BIGINT UNSIGNED NOT NULL,
  account_id         BIGINT UNSIGNED NOT NULL,
  wood               INT UNSIGNED    NOT NULL DEFAULT 0,
  stone              INT UNSIGNED    NOT NULL DEFAULT 0,
  updated_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (farm_id, account_id),
  CONSTRAINT fk_farm_players_farm FOREIGN KEY (farm_id) REFERENCES farms (id),
  CONSTRAINT fk_farm_players_account FOREIGN KEY (account_id) REFERENCES accounts (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
