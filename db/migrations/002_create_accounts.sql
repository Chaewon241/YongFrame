-- 계정과 누적 기록 테이블 (docs/persistence.md).
--
-- 규칙은 001과 같다: 번호 순서로 적용하고, 적용한 파일은 고치지 않고, 여러 번 실행해도 안전하도록 IF NOT EXISTS를 쓴다.
--
-- login_name: 소문자 영문/숫자/밑줄 4~16자(서버가 소문자로 바꿔 넣는다). UNIQUE라서 같은 ID를 두 요청이 동시에
--   만들어도 하나만 통과한다. 문자 집합 utf8mb4와 대소문자 구분 없는 정렬(_ai_ci)이라 DB 쪽에서도 대소문자만
--   다른 ID는 같은 ID로 본다.
-- password_hash: 평문 비밀번호는 어디에도 저장하지 않는다. libsodium crypto_pwhash_str(Argon2id)가 만든 문자열
--   (솔트와 비용 매개변수 포함, 최대 128바이트)이다.
-- account_stats: 누적 기록. 행이 아직 없으면 서버는 0으로 본다.
CREATE TABLE IF NOT EXISTS accounts (
  id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  login_name         VARCHAR(16)     NOT NULL,
  password_hash      VARCHAR(255)    NOT NULL,
  created_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_accounts_login_name (login_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS account_stats (
  account_id         BIGINT UNSIGNED NOT NULL,
  wood_gathered      BIGINT UNSIGNED NOT NULL DEFAULT 0,
  stone_gathered     BIGINT UNSIGNED NOT NULL DEFAULT 0,
  houses_built       BIGINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (account_id),
  CONSTRAINT fk_account_stats_account FOREIGN KEY (account_id) REFERENCES accounts (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
