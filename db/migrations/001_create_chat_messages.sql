-- 채팅 메시지 저장 테이블.
--
-- 마이그레이션 파일 규칙 (docs/stage2.md)
--  - 파일 이름은 "번호_설명.sql". 번호 순서대로 적용하고, 적용한 번호는 schema_migrations에 기록한다.
--  - 한 번 적용한 파일은 고치지 않는다. 바꿀 일이 생기면 다음 번호의 새 파일을 만든다.
--  - 여러 번 실행해도 안전하도록 IF NOT EXISTS를 쓴다.
--
-- 문자 집합은 utf8mb4로 명시한다. 서버 기본값에 기대지 않아야 한글과 이모지가 깨지지 않는다.
-- nickname은 16문자 이하, text는 1,000바이트 이하(= 1,000문자 이하)라는 프로토콜 규칙에 맞춘 크기다.
-- id는 AUTO_INCREMENT라 저장 순서대로 증가하며, 이 번호가 곧 방의 전파 순서(message_id)다.
-- "최근 N개" 조회는 기본 키(id) 역순으로 읽으므로 별도 인덱스가 필요 없다.
CREATE TABLE IF NOT EXISTS chat_messages (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  nickname        VARCHAR(16)     NOT NULL,
  text            VARCHAR(1000)   NOT NULL,
  sent_at_unix_ms BIGINT          NOT NULL,
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
