#include "server/stats_format.h"

#include <algorithm>
#include <vector>

namespace game::server {

namespace {

// "이름{...}" 묶음. 항목(이름=값)과 하위 묶음을 나온 순서와 상관없이 따로 모은다(항목 먼저, 하위 묶음은 그 아래 줄).
struct Group {
  std::string name;
  std::vector<std::string> items;
  std::vector<Group> children;
};

// s[pos]부터 '}' 또는 끝까지 읽어 group을 채운다. 중괄호가 맞지 않으면 false.
bool ParseGroup(std::string_view s, std::size_t& pos, Group& group, int depth) {
  while (pos < s.size()) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    if (pos >= s.size()) break;
    if (s[pos] == '}') {
      if (depth == 0) return false;  // 여는 괄호 없이 닫는 괄호
      ++pos;
      return true;
    }
    const std::size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '{' && s[pos] != '}') ++pos;
    const std::string_view token = s.substr(start, pos - start);
    if (pos < s.size() && s[pos] == '{') {
      if (token.empty()) return false;
      ++pos;
      Group child;
      child.name = std::string(token);
      if (!ParseGroup(s, pos, child, depth + 1)) return false;
      group.children.push_back(std::move(child));
    } else if (!token.empty()) {
      group.items.emplace_back(token);
    }
  }
  return depth == 0;  // 끝에 닿았는데 아직 묶음 안이면 닫히지 않은 것
}

void AppendLine(std::string& out, const std::string& line) {
  if (!out.empty()) out += '\n';
  out += line;
}

// label 뒤에 items를 items_per_line개씩 끊어 붙인다. 이어지는 줄은 첫 항목의 시작 위치에 맞춘다.
void AppendItems(std::string& out, const std::string& prefix, const std::vector<std::string>& items,
                 std::size_t items_per_line) {
  const std::string continuation(DisplayWidth(prefix), ' ');
  std::string line = prefix;
  std::size_t on_line = 0;
  for (const std::string& item : items) {
    if (items_per_line != 0 && on_line == items_per_line) {
      AppendLine(out, line);
      line = continuation;
      on_line = 0;
    }
    if (on_line != 0) line += "  ";
    line += item;
    ++on_line;
  }
  AppendLine(out, line);
}

std::string PadRight(const std::string& text, std::size_t width) {
  const std::size_t shown = DisplayWidth(text);
  return shown >= width ? text : text + std::string(width - shown, ' ');
}

}  // namespace

bool ParseStatsFormat(std::string_view text, StatsFormat& out) {
  if (text == "line") {
    out = StatsFormat::kLine;
  } else if (text == "block") {
    out = StatsFormat::kBlock;
  } else {
    return false;
  }
  return true;
}

std::size_t DisplayWidth(std::string_view utf8) {
  std::size_t width = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    std::size_t length = 1;
    char32_t code = lead;
    if (lead >= 0xF0) {
      length = 4;
      code = lead & 0x07;
    } else if (lead >= 0xE0) {
      length = 3;
      code = lead & 0x0F;
    } else if (lead >= 0xC0) {
      length = 2;
      code = lead & 0x1F;
    }
    for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
      code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    }
    // 한글, 한자, 전각 문자는 터미널에서 두 칸을 차지한다(대략적인 기준이지만 이 로그의 글자에는 충분하다).
    width += (code >= 0x1100) ? 2 : 1;
    i += length;
  }
  return width;
}

std::string FormatStatsBlock(std::string_view one_line, std::size_t items_per_line) {
  Group root;
  std::size_t pos = 0;
  if (!ParseGroup(one_line, pos, root, 0)) {
    return std::string(one_line);  // 예상과 다른 모양이면 그대로(정보를 잃지 않는다)
  }

  // 묶음 이름의 폭을 맞춘다(한글 두 글자 = 4칸이 기본).
  std::size_t label_width = DisplayWidth("서버");
  for (const Group& child : root.children) label_width = std::max(label_width, DisplayWidth(child.name));

  std::string out;
  if (!root.items.empty()) {
    AppendItems(out, "  " + PadRight("서버", label_width) + "  ", root.items, items_per_line);
  }
  const std::string nested_indent(2 + label_width + 2, ' ');
  for (const Group& child : root.children) {
    AppendItems(out, "  " + PadRight(child.name, label_width) + "  ", child.items, items_per_line);
    for (const Group& grandchild : child.children) {
      // 한 단계 더 안쪽 묶음(예: 게임 안의 틱시간)은 부모 항목 아래에 "이름: 항목..."으로 둔다.
      // 그보다 깊은 묶음은 지금 통계에 없어서 평평하게 이어 붙인다.
      std::vector<std::string> items = grandchild.items;
      for (const Group& deeper : grandchild.children) {
        for (const std::string& item : deeper.items) items.push_back(deeper.name + "." + item);
      }
      AppendItems(out, nested_indent + grandchild.name + ": ", items, items_per_line);
    }
  }
  return out;
}

}  // namespace game::server
