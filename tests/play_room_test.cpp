// Room 단위 테스트: 인원, 방장, 닉네임, 칸(slot) 배정만 검증한다. 소켓이 없다.

#include <gtest/gtest.h>

#include "play/room.h"

namespace game::play {
namespace {

Room MakeRoom() { return Room("123456", PlayConfig{}); }

TEST(RoomTest, StartsEmptyWithNoHost) {
  Room room = MakeRoom();
  EXPECT_TRUE(room.Empty());
  EXPECT_FALSE(room.Full());
  EXPECT_EQ(room.host_player_id(), 0u);
  EXPECT_EQ(room.code(), "123456");
}

TEST(RoomTest, TheFirstMemberBecomesTheHost) {
  Room room = MakeRoom();
  const std::uint32_t a = room.AddMember(100, "alice");
  const std::uint32_t b = room.AddMember(101, "bob");
  EXPECT_EQ(a, 1u);
  EXPECT_EQ(b, 2u);
  EXPECT_EQ(room.host_player_id(), a);
  ASSERT_EQ(room.members().size(), 2u);
  EXPECT_EQ(room.members()[0].nickname, "alice");  // 입장한 순서
  EXPECT_EQ(room.members()[1].nickname, "bob");
}

TEST(RoomTest, IsFullAtFourPlayers) {
  Room room = MakeRoom();
  for (std::uint64_t i = 0; i < 3; ++i) room.AddMember(i, "p" + std::to_string(i));
  EXPECT_FALSE(room.Full());
  room.AddMember(3, "p3");
  EXPECT_TRUE(room.Full());
}

TEST(RoomTest, FindsMembersByClientAndByPlayer) {
  Room room = MakeRoom();
  const std::uint32_t id = room.AddMember(77, "alice");
  ASSERT_NE(room.FindByClient(77), nullptr);
  EXPECT_EQ(room.FindByClient(77)->player_id, id);
  ASSERT_NE(room.FindByPlayer(id), nullptr);
  EXPECT_EQ(room.FindByPlayer(id)->client_id, 77u);
  EXPECT_EQ(room.FindByClient(1), nullptr);
  EXPECT_EQ(room.FindByPlayer(99), nullptr);
}

TEST(RoomTest, NicknamesAreComparedExactly) {
  Room room = MakeRoom();
  room.AddMember(1, "Alice");
  EXPECT_TRUE(room.HasNickname("Alice"));
  EXPECT_FALSE(room.HasNickname("alice"));  // 대소문자를 구분한다
  EXPECT_FALSE(room.HasNickname("Alic"));
}

TEST(RoomTest, MembersGetTheLowestFreeSlotAndReuseFreedOnes) {
  Room room = MakeRoom();
  room.AddMember(1, "a");
  room.AddMember(2, "b");
  room.AddMember(3, "c");
  EXPECT_EQ(room.FindByClient(1)->slot, 0u);
  EXPECT_EQ(room.FindByClient(2)->slot, 1u);
  EXPECT_EQ(room.FindByClient(3)->slot, 2u);
  room.RemoveMember(2);
  room.AddMember(4, "d");
  EXPECT_EQ(room.FindByClient(4)->slot, 1u);  // 비어 있던 칸을 새 사람이 쓴다
}

TEST(RoomTest, MembersJoinTheWorldAtTheirSlotPosition) {
  Room room = MakeRoom();
  const std::uint32_t a = room.AddMember(1, "a");
  const std::uint32_t b = room.AddMember(2, "b");
  ASSERT_NE(room.world().Find(a), nullptr);
  ASSERT_NE(room.world().Find(b), nullptr);
  EXPECT_FLOAT_EQ(room.world().Find(a)->x, 2400.0f);
  EXPECT_FLOAT_EQ(room.world().Find(b)->x, 2600.0f);
}

TEST(RoomTest, RemovingAMemberRemovesItFromTheWorld) {
  Room room = MakeRoom();
  const std::uint32_t a = room.AddMember(1, "a");
  room.AddMember(2, "b");
  const auto result = room.RemoveMember(1);
  EXPECT_TRUE(result.removed);
  EXPECT_EQ(result.member.player_id, a);
  EXPECT_EQ(room.world().Find(a), nullptr);
  EXPECT_EQ(room.members().size(), 1u);
}

TEST(RoomTest, RemovingSomeoneWhoIsNotInTheRoomChangesNothing) {
  Room room = MakeRoom();
  room.AddMember(1, "a");
  const auto result = room.RemoveMember(42);
  EXPECT_FALSE(result.removed);
  EXPECT_FALSE(result.host_changed);
  EXPECT_EQ(room.members().size(), 1u);
}

TEST(RoomTest, NonHostLeavingKeepsTheHost) {
  Room room = MakeRoom();
  const std::uint32_t host = room.AddMember(1, "a");
  room.AddMember(2, "b");
  const auto result = room.RemoveMember(2);
  EXPECT_TRUE(result.removed);
  EXPECT_FALSE(result.host_changed);
  EXPECT_EQ(room.host_player_id(), host);
}

// 방장이 나가면 먼저 들어온 순서로 다음 사람이 방장이 된다.
TEST(RoomTest, HostMigratesToTheEarliestRemainingMember) {
  Room room = MakeRoom();
  room.AddMember(1, "a");
  const std::uint32_t b = room.AddMember(2, "b");
  room.AddMember(3, "c");
  const auto result = room.RemoveMember(1);
  EXPECT_TRUE(result.removed);
  EXPECT_TRUE(result.host_changed);
  EXPECT_EQ(room.host_player_id(), b);

  const std::uint32_t c = room.FindByClient(3)->player_id;
  const auto second = room.RemoveMember(2);  // 새 방장도 나가면 그 다음 사람
  EXPECT_TRUE(second.host_changed);
  EXPECT_EQ(room.host_player_id(), c);
}

TEST(RoomTest, LastMemberLeavingEmptiesTheRoom) {
  Room room = MakeRoom();
  room.AddMember(1, "a");
  const auto result = room.RemoveMember(1);
  EXPECT_TRUE(result.removed);
  EXPECT_FALSE(result.host_changed);  // 남은 사람이 없어 방장이 바뀌지 않는다
  EXPECT_TRUE(room.Empty());
  EXPECT_EQ(room.host_player_id(), 0u);
}

TEST(RoomTest, PlayerIdsAreNeverReused) {
  Room room = MakeRoom();
  const std::uint32_t a = room.AddMember(1, "a");
  room.RemoveMember(1);
  const std::uint32_t b = room.AddMember(2, "b");
  EXPECT_NE(a, b);
  EXPECT_EQ(b, 2u);
}

}  // namespace
}  // namespace game::play
