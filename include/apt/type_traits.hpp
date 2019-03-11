#ifndef  _APT_TYPE_TRAITS_HPP_
#define  _APT_TYPE_TRAITS_HPP_

namespace apt {
  template < class C >
  inline constexpr int ndim_v = C::NDim;

  template < class C >
  struct element {
    using type = typename C::element_type;
  };

  template < class C >
  using element_t = typename element<C>::type;
}


#include <array>
namespace apt {
  template < typename T, std::size_t N >
  inline constexpr int ndim_v<std::array<T,N>> = N;

  template < typename T, std::size_t N >
  struct element<std::array<T,N>> {
    using type = T;
  };
}

namespace apt {
  template < typename... T >
  using most_precise_t = std::enable_if_t< (... && std::is_arithmetic_v<T>),
                                           decltype( (... + (T)0) ) >;
}

#endif
