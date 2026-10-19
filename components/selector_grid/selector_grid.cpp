#include "selector_grid.h"
#include "esphome/core/hal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

namespace esphome::selector_grid {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// At most this many keys on the glass at once, and this many options kept. A selector longer than that is shown up to
// its 64th option; the tile's data (2.6 KB) usually runs out first.
static constexpr int MAX_KEYS = 12;
static constexpr int MAX_ITEMS = 64;
static constexpr int MAX_HIDDEN = 16;
// An option's name as the tile gets it: a text of an attribute is cut at 48 bytes.
static constexpr int NAME_LEN = 49;
// How long a key stays lit after a tap: at least this long, and until the next tick after it.
static constexpr uint32_t FLASH_MS = 300;

static void copy(char *to, size_t size, const char *from) { std::snprintf(to, size, "%s", from ? from : ""); }

// Where each domain keeps its options and the one in use, and the action that selects one. The first whose prefix the
// entity id starts with is the entity's.
struct Source {
  const char *prefix;
  const char *list;     // the attribute with the options
  const char *current;  // the attribute with the option in use; nullptr: the entity's state
  const char *service;
  const char *field;
};
static constexpr Source SOURCES[] = {
    {"input_select.", "options", nullptr, "input_select.select_option", "option"},
    {"select.", "options", nullptr, "select.select_option", "option"},
    {"media_player.", "source_list", "source", "media_player.select_source", "source"},
    {"remote.", "activity_list", "current_activity", "remote.turn_on", "activity"},
};

struct Item {
  char name[NAME_LEN];
};

// The tile: the options as keys in a grid, the one in use lit. When they do not all fit, a strip along the bottom holds
// an arrow either side of the page number, and a tap on either half of it turns the page (round from the last to the
// first). A tap on a key selects its option.
class GridTile : public tessera::Tile {
 public:
  explicit GridTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    copy(entity_, sizeof entity_, c.entity);
    for (const Source &s : SOURCES)
      if (std::strncmp(entity_, s.prefix, std::strlen(s.prefix)) == 0) {
        source_ = &s;
        break;
      }
    for (JsonVariantConst v : c.options["hide"].as<JsonArrayConst>()) {
      const char *name = v | "";
      if (!name[0] || hidden_count_ >= MAX_HIDDEN) continue;
      copy(hidden_[hidden_count_++], NAME_LEN, name);
    }
    gap_ = ui::px(6);
    layout();
  }

  void on_state(JsonObjectConst data) override {
    const char *wait = data["wait"] | "";
    got_state_ = true;
    if (!source_ || std::strcmp(wait, "wrong_entity") == 0) {
      if (wrong_ && hint_) return;
      wrong_ = true;
      if (!items_.empty()) items_.clear();
      layout();
      return;
    }
    wrong_ = false;

    // The options, less the ones left out. Rebuilt only when they changed, so a change of the option in use (the usual
    // change) only repaints.
    fresh_.clear();
    for (JsonVariantConst v : data["attributes"][source_->list].as<JsonArrayConst>()) {
      const char *name = v | "";
      if (!name[0] || is_hidden(name)) continue;
      if ((int) fresh_.size() >= MAX_ITEMS) break;
      Item item;
      copy(item.name, sizeof item.name, name);
      fresh_.push_back(item);
    }
    bool changed = fresh_.size() != items_.size();
    for (size_t i = 0; !changed && i < fresh_.size(); ++i) changed = std::strcmp(fresh_[i].name, items_[i].name) != 0;

    const char *state = data["state"] | "";
    const char *current = source_->current ? (data["attributes"][source_->current] | "") : state;
    const bool moved = std::strcmp(current, current_) != 0;
    copy(current_, sizeof current_, current);
    const bool away = std::strcmp(state, "unavailable") == 0;
    const bool repaint = away != away_;
    away_ = away;

    if (changed) {
      items_ = fresh_;
      layout();  // shows the page of the option in use
      return;
    }
    // Another option in use, chosen here, on another screen or by an automation: show its page.
    if (moved && paged_) {
      const int at = index_of(current_);
      if (at >= 0 && at / per_page_ != page_) {
        page_ = at / per_page_;
        show_page();
        return;
      }
    }
    if (moved || repaint) paint();
  }

  void on_tick(uint32_t) override {
    if (flash_ >= 0 && esphome::millis() - flash_at_ >= FLASH_MS) {
      flash_ = -1;
      paint();
    }
  }

  void on_theme() override {
    if (hint_) ui::set_color(hint_, theme::MUTED);
    if (page_label_) ui::set_color(page_label_, theme::MUTED);
    paint();
  }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    const int n = (int) items_.size();
    if (n == 0) return;
    // The arrows: along the bottom, the left half goes back a page and the right half on; at the sides, the arrow
    // either side.
    int turn = 0;
    if (paged_ && !side_ && p.y >= height_ - strip_h_) turn = p.x < width_ / 2 ? -1 : 1;
    if (paged_ && side_ && p.x >= 0) {
      if (p.x < arrow_w_ + gap_ / 2) turn = -1;
      else if (p.x >= width_ - arrow_w_ - gap_ / 2) turn = 1;
    }
    if (turn != 0) {
      page_ = (page_ + pages_ + turn) % pages_;
      flash_ = -1;
      show_page();
      return;
    }
    const int slot = slot_at(p);
    if (slot < 0) return;
    const int index = page_ * per_page_ + slot;
    if (index >= n) return;
    bool sent = false;
    if (source_ && entity_[0]) sent = tessera::action(source_->service, entity_, source_->field, items_[index].name);
    // Lit at once; Home Assistant's answer puts it right if the device chose otherwise.
    if (sent) copy(current_, sizeof current_, items_[index].name);
    flash_ = slot;
    flash_ok_ = sent;
    flash_at_ = esphome::millis();
    paint();
  }

 private:
  struct Key {
    lv_obj_t *block = nullptr, *label = nullptr;
  };

  bool is_hidden(const char *name) const {
    for (int i = 0; i < hidden_count_; ++i)
      if (strcasecmp(hidden_[i], name) == 0) return true;
    return false;
  }

  int index_of(const char *name) const {
    if (!name[0]) return -1;
    for (size_t i = 0; i < items_.size(); ++i)
      if (std::strcmp(items_[i].name, name) == 0) return (int) i;
    return -1;
  }

  // The key slot under the finger, with half the gap around each key, or -1. With one key and no known point, that key.
  int slot_at(tessera::Point p) const {
    if (p.x < 0 || p.y < 0) return per_page_ == 1 ? 0 : -1;
    const int col = (p.x - left_ + gap_ / 2) / (kw_ + gap_);
    const int row = (p.y - top_ + gap_ / 2) / (kh_ + gap_);
    if (p.x - left_ + gap_ / 2 < 0 || p.y - top_ + gap_ / 2 < 0 || col >= cols_ || row >= rows_) return -1;
    const int slot = row * cols_ + col;
    return slot < per_page_ ? slot : -1;
  }

  void clear() {
    for (Key &key : keys_) {
      if (key.block) lv_obj_del(key.block);
      key = Key{};
    }
    key_count_ = 0;
    for (lv_obj_t **obj : {&hint_, &prev_, &next_, &page_label_})
      if (*obj) {
        lv_obj_del(*obj);
        *obj = nullptr;
      }
    paged_ = false;
  }

  void show_hint(const char *key) {
    hint_ = ui::text(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(hint_, width_);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(hint_, plugin_->text(key));
    const int lines = ui::text_width(plugin_->text(key), Font::BODY) > width_ ? 2 : 1;
    lv_obj_set_pos(hint_, 0, (height_ - lines * ui::line_height(Font::BODY)) / 2);
  }

  // How many keys of a finger's size fit across and down the room given, at most twelve in all.
  void fit(int room_w, int room_h, int *cols, int *rows) const {
    const int kw_min = ui::mm(16), kh_min = ui::mm(9);
    *cols = std::max(1, (room_w + gap_) / (kw_min + gap_));
    *rows = std::max(1, (room_h + gap_) / (kh_min + gap_));
    while (*cols * *rows > MAX_KEYS) {
      if (*rows >= *cols) --*rows;
      else --*cols;
    }
  }

  void layout() {
    clear();
    const int n = (int) items_.size();
    if (!source_ || wrong_ || !entity_[0]) {
      show_hint("no_entity");
      return;
    }
    if (n == 0) {
      if (!got_state_) return;  // the entity's options are on their way
      show_hint("no_options");
      return;
    }

    int cols, rows;
    int room_w = width_, room_h = height_, room_x = 0;
    side_ = false;
    fit(room_w, room_h, &cols, &rows);
    if (n <= cols * rows) {
      // They all fit: the grid of them whose keys are closest to two and a half times as wide as high, a word's key.
      int best = 1;
      float best_off = 1e9f;
      for (int c = 1; c <= std::min(cols, n); ++c) {
        const int r = (n + c - 1) / c;
        if (r > rows) continue;
        const int kw = (room_w - (c - 1) * gap_) / c;
        const int kh = std::min((room_h - (r - 1) * gap_) / r, ui::mm(16));
        const float off = std::abs((float) kw / std::max(1, kh) - 2.5f);
        if (off < best_off) {
          best_off = off;
          best = c;
        }
      }
      cols_ = best;
      rows_ = (n + best - 1) / best;
      per_page_ = n;
      pages_ = 1;
    } else {
      // More than fit: a strip for the arrows along the bottom and full pages above it, or, on a tile too low for a
      // row of keys over the strip, an arrow at either side and the keys between them.
      paged_ = true;
      strip_h_ = std::max(ui::mm(8), ui::line_height(Font::ICON_SMALL) + ui::px(6));
      room_h = height_ - strip_h_ - gap_;
      if (room_h < ui::mm(9)) {
        side_ = true;
        arrow_w_ = std::max(ui::mm(5), ui::line_height(Font::ICON_SMALL) + ui::px(4));
        room_h = height_;
        room_x = arrow_w_ + gap_;
        room_w = std::max(1, width_ - 2 * room_x);
      }
      fit(room_w, room_h, &cols, &rows);
      cols_ = cols;
      rows_ = rows;
      per_page_ = cols * rows;
      pages_ = (n + per_page_ - 1) / per_page_;
    }

    kw_ = std::max(1, (room_w - (cols_ - 1) * gap_) / cols_);
    kh_ = std::max(1, std::min((room_h - (rows_ - 1) * gap_) / rows_, ui::mm(16)));
    left_ = room_x + (room_w - (cols_ * kw_ + (cols_ - 1) * gap_)) / 2;
    top_ = (room_h - (rows_ * kh_ + (rows_ - 1) * gap_)) / 2;
    radius_ = std::min(ui::px(ui::large() ? 12 : 8), std::min(kw_, kh_) / 2);

    // Words on a key: the larger face where two lines of it fit, else the smaller, one line where two do not.
    const int pad = ui::px(4);
    face_ = 2 * ui::line_height(Font::BODY_LARGE) + pad <= kh_ ? Font::BODY_LARGE : Font::BODY;
    // A key narrower than a word's key (a single cell with arrows) keeps one line, so a word is not broken in two.
    two_lines_ = 2 * ui::line_height(face_) + pad <= kh_ && kw_ >= ui::mm(16);
    const int slots = std::min(per_page_, MAX_KEYS);
    for (int i = 0; i < slots; ++i) {
      Key &key = keys_[i];
      key.block = ui::block(parent_, theme::BUTTON);
      lv_obj_set_size(key.block, kw_, kh_);
      lv_obj_set_pos(key.block, left_ + (i % cols_) * (kw_ + gap_), top_ + (i / cols_) * (kh_ + gap_));
      lv_obj_set_style_radius(key.block, radius_, 0);
      key.label = two_lines_ ? ui::text(key.block, face_, theme::INK) : ui::label(key.block, face_, theme::INK);
      lv_obj_set_width(key.label, kw_ - 2 * pad);
      lv_obj_set_style_text_align(key.label, LV_TEXT_ALIGN_CENTER, 0);
      if (!two_lines_) lv_label_set_long_mode(key.label, LV_LABEL_LONG_DOT);
    }
    key_count_ = slots;

    if (paged_) {
      const int y = side_ ? 0 : height_ - strip_h_;
      const int ah = side_ ? height_ : strip_h_;
      const int aw = side_ ? arrow_w_ : std::min(ui::mm(14), width_ / 3);
      const Font icon_face = Font::ICON_SMALL;
      const int ih = ui::line_height(icon_face);
      lv_obj_t **arrows[2] = {&prev_, &next_};
      const uint32_t glyphs[2] = {0xF0141, 0xF0142};  // chevron-left, chevron-right
      for (int a = 0; a < 2; ++a) {
        lv_obj_t *b = ui::block(parent_, theme::BUTTON);
        lv_obj_set_size(b, aw, ah);
        lv_obj_set_pos(b, a == 0 ? 0 : width_ - aw, y);
        lv_obj_set_style_radius(b, std::min(radius_, std::min(aw, ah) / 2), 0);
        lv_obj_t *icon = ui::label(b, icon_face, theme::INK);
        lv_obj_set_width(icon, aw);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(icon, 0, (ah - ih) / 2);
        ui::set_text(icon, ui::icon(glyphs[a]));
        *arrows[a] = b;
      }
      // The page number between the arrows of the strip; arrows at the sides leave no room for it.
      if (!side_) {
        page_label_ = ui::label(parent_, Font::BODY, theme::MUTED);
        lv_obj_set_width(page_label_, width_ - 2 * aw);
        lv_obj_set_style_text_align(page_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(page_label_, aw, y + (strip_h_ - ui::line_height(Font::BODY)) / 2);
      }
    }

    // The page of the option in use, else the first.
    const int at = index_of(current_);
    page_ = at >= 0 ? at / per_page_ : 0;
    if (page_ >= pages_) page_ = 0;
    show_page();
  }

  void show_page() {
    const int n = (int) items_.size();
    const int pad = ui::px(4);
    for (int i = 0; i < key_count_; ++i) {
      Key &key = keys_[i];
      const int index = page_ * per_page_ + i;
      if (index >= n) {
        lv_obj_add_flag(key.block, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_remove_flag(key.block, LV_OBJ_FLAG_HIDDEN);
      const std::string name = items_[index].name;
      ui::set_text(key.label, name);
      // Centred down the key: one line or two, as the words need.
      const int lines = two_lines_ && ui::text_width(name, face_) > kw_ - 2 * pad ? 2 : 1;
      lv_obj_set_pos(key.label, pad, (kh_ - lines * ui::line_height(face_)) / 2);
    }
    if (page_label_) {
      char text[16];
      std::snprintf(text, sizeof text, "%d/%d", page_ + 1, pages_);
      ui::set_text(page_label_, text);
    }
    paint();
  }

  void paint() {
    const int n = (int) items_.size();
    for (int i = 0; i < key_count_; ++i) {
      Key &key = keys_[i];
      const int index = page_ * per_page_ + i;
      if (index >= n) continue;
      if (i == flash_) {
        if (flash_ok_) ui::set_fill(key.block, Tone::NORMAL, theme::ACCENT_PRESSED);
        else ui::set_fill(key.block, Tone::ALERT, theme::BUTTON);
        ui::set_color(key.label, theme::ON_ACCENT);
        continue;
      }
      if (!away_ && std::strcmp(items_[index].name, current_) == 0) {
        ui::set_fill(key.block, Tone::NORMAL, theme::ACCENT);
        ui::set_color(key.label, theme::ON_ACCENT);
        continue;
      }
      ui::set_fill(key.block, Tone::NORMAL, theme::BUTTON);
      ui::set_color(key.label, away_ ? theme::SUBTLE : theme::INK);
    }
    for (lv_obj_t *arrow : {prev_, next_}) {
      if (!arrow) continue;
      ui::set_fill(arrow, Tone::NORMAL, theme::BUTTON);
      lv_obj_t *icon = lv_obj_get_child(arrow, 0);
      if (icon) ui::set_color(icon, theme::INK);
    }
  }

  const tessera::Plugin *plugin_;
  const Source *source_ = nullptr;
  lv_obj_t *parent_ = nullptr;
  int width_ = 1, height_ = 1;
  char entity_[128] = "";
  char hidden_[MAX_HIDDEN][NAME_LEN] = {};
  int hidden_count_ = 0;
  tessera::List<Item> items_, fresh_;
  char current_[NAME_LEN] = "";
  bool away_ = false, wrong_ = false, got_state_ = false;

  Key keys_[MAX_KEYS];
  int key_count_ = 0;
  lv_obj_t *hint_ = nullptr, *prev_ = nullptr, *next_ = nullptr, *page_label_ = nullptr;
  bool paged_ = false, side_ = false;
  int arrow_w_ = 0;
  int page_ = 0, pages_ = 1, per_page_ = 1, cols_ = 1, rows_ = 1;
  int kw_ = 1, kh_ = 1, gap_ = 0, left_ = 0, top_ = 0, radius_ = 0, strip_h_ = 0;
  Font face_ = Font::BODY;
  bool two_lines_ = false;

  int flash_ = -1;
  bool flash_ok_ = false;
  uint32_t flash_at_ = 0;
};

void SelectorGrid::setup() {
  add_tile("grid", [this]() { return new GridTile(this); });
}

}  // namespace esphome::selector_grid
