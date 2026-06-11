#pragma once

namespace exodus {

// https://www.cppstories.com/2019/02/2lines3featuresoverload.html
template <typename... Ts>
struct overload : Ts... {
  using Ts::operator()...;
};

template <typename... Ts>
overload(Ts...) -> overload<Ts...>;

} // namespace exodus
