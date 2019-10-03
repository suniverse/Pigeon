#ifndef  _APT_ACTION_BASE_HPP_
#define  _APT_ACTION_BASE_HPP_

#include "apt/index.hpp"
#include "apt/pair.hpp"

namespace apt {
  template < int DGrid >
  struct ActionBase {
  private:
    apt::Index<DGrid> _Ib;
    apt::Index<DGrid> _Ie;
    apt::array<apt::pair<int>,DGrid> _guard;

  public:
    ActionBase& setIb(apt::Index<DGrid> Ib) { _Ib = Ib; return *this; }
    ActionBase& setIe(apt::Index<DGrid> Ie) { _Ie = Ie; return *this; }
    ActionBase& setGuard(apt::array<apt::pair<int>,DGrid> guard) { _guard = guard; return *this; }

    const auto& Ib() const noexcept { return _Ib; }
    const auto& Ie() const noexcept { return _Ie; }
    apt::Index<DGrid>  ext() const noexcept { return (_Ie - _Ib); }
    const auto& guard() const noexcept { return _guard; }

    virtual ~ActionBase() {};

    virtual ActionBase* Clone() const = 0; // covariant return types, see Modern C++ Design
  };
}

#endif