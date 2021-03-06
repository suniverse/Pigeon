#include "io/io_impl.hpp"
#include "pic.hpp"

namespace io {
  using namespace pic;

  template
  void export_data<real_export_t, DGrid, real_t, particle::Specs, real_j_t>
  ( std::string prefix, int timestep, real_t dt, int num_files, int downsample_ratio,
    const std::optional<mpi::CartComm>& cart_opt,
    const dye::Ensemble<DGrid>& ens,
    const apt::Grid<real_t,DGrid>& grid, // local grid
    const field::Field<real_t, 3, DGrid>& Efield,
    const field::Field<real_t, 3, DGrid>& Bfield,
    const field::Field<real_j_t, 3, DGrid>& Jfield,// J is Jmesh on a replica
    const particle::map<particle::array<real_t,particle::Specs>>& particles,
    const particle::map<particle::Properties>& properties,
    const std::vector<FieldExportee<real_export_t, DGrid, real_t, real_j_t>*>& fexps,
    const std::vector<PtcExportee<real_export_t, DGrid, real_t, particle::Specs>*>& pexps
    );
}

