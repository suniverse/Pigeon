#include "field/haugbolle_solver/updater_impl.hpp"
#include "pic.hpp"

namespace field {
  template struct HaugbolleUpdater<pic::real_t, pic::DGrid, pic::real_j_t>;
}