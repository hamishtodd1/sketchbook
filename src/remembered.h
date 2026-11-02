// Windows taken away for now, kept so they come back as they were.
//
// A box whose build fails has, for the moment, no picture, no values, no
// function -- and every window showing one of those goes, since there is
// nothing for it to show. When the box builds again they come back, and they
// used to come back new: docked, at the size every window starts at, framed
// afresh, as if nobody had ever moved or resized them. So a panel keeps what it
// took away, and a window coming back for the same box (and the same name)
// takes up where it was left: loose or docked, where it was, how big, at what
// scale.
//
// Bounded, like everything here that grows with use; and what a deleted box had
// is dropped with the box, so nothing kept points at it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace sb {

template <typename Item>
class Remembered {
 public:
  void Keep(Item item) {
    if constexpr (requires { item.selected; }) item.selected = false;
    items_.push_back(std::move(item));
    if (items_.size() > kMost) items_.erase(items_.begin());
  }

  // The most recently kept one that `matches` says is the one, taken back out;
  // false if there is none.
  template <typename Match>
  bool Take(Match matches, Item* out) {
    for (std::size_t i = items_.size(); i-- > 0;) {
      if (!matches(items_[i])) continue;
      *out = std::move(items_[i]);
      items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
    return false;
  }

  template <typename Match>
  void Drop(Match matches) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), matches), items_.end());
  }

  void Clear() { items_.clear(); }

 private:
  static constexpr std::size_t kMost = 16;
  std::vector<Item> items_;
};

}  // namespace sb
