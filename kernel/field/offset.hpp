#ifndef  _FIELD_OFFSET_HPP_
#define  _FIELD_OFFSET_HPP_

namespace field {
  using offset_t = bool;
}

constexpr field::offset_t INSITU{false}; // right on the grid point
constexpr field::offset_t MIDWAY{true}; // in the middle of an edge connecting two grid points

#endif
