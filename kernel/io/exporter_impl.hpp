#ifndef _IO_EXPORTER_IMPL_HPP_
#define _IO_EXPORTER_IMPL_HPP_

#include "io/exporter.hpp"

namespace io {
  template < typename Tcoarse, int DGrid, typename Tfine >
  apt::Grid<Tcoarse,DGrid> coarsen(const apt::Grid<Tfine,DGrid>& grid, int downsample_ratio) {
    apt::Grid<Tcoarse,DGrid> res;
    for ( int i = 0; i < DGrid; ++i ) {
      res[i] = apt::Grid1D<Tcoarse>( grid[i].lower(), grid[i].upper(), grid[i].dim() / downsample_ratio );
    }
    return res;
  }

  template < typename RealDS,
             int DGrid,
             typename Real,
             template < typename > class S,
             typename RealJ >
  void DataExporter<RealDS, DGrid, Real, S, RealJ>::
  execute( const DataSaver& saver,
           const apt::Grid<Real,DGrid>& grid,
           const field::Field<Real, 3, DGrid>& E,
           const field::Field<Real, 3, DGrid>& B,
           const field::Field<RealJ, 3, DGrid>& J,
           const particle::map<particle::array<Real,S>>& particles,
           const particle::map<particle::Properties>& properties,
           const std::vector<FexpT*>& fexps,
           const std::vector<PexpT*>& pexps
           ) const {
    const auto grid_ds = coarsen<RealDS>( grid, _ratio );

    field::Field<RealDS,3,DGrid> fds ( apt::make_range({}, apt::dims(grid_ds), _guard) ); // FIXME _guard??

    auto smart_save =
      [&saver]( const std::string name, const int field_dim, const auto& fds ) {
        for ( int comp = 0; comp < field_dim; ++comp ) {
          auto varname = name;
          auto pos = varname.find('#');
          if ( pos != std::string::npos )
            varname.replace( pos, 1, std::to_string(comp+1) );
          saver.save( varname, fds[comp] );
        }
      };

    if ( _cart_opt ) {
      for ( auto* fe : fexps ) {
        auto varname = fe->name();
        auto dim = fe->num_comps();
        if ( dim > 1 ) varname += "#";
        fds = fe->action(_ratio, _cart_opt, grid, grid_ds, _guard, E, B, J );

        field::copy_sync_guard_cells( fds, *_cart_opt );
        smart_save( varname, dim, fds );
      }
    }

    for ( auto sp : particles ) {
      fds.reset();
      const auto& prop = properties[sp];

      for ( auto* pe : pexps ) {
        auto varname = pe->name();
        auto dim = pe->num_comps();
        if ( dim > 1 ) varname += "#";
        varname += "_" + prop.name;
        fds = pe->action(_ratio, grid, grid_ds, _guard, prop, particles[sp]);

        // FIXME reduce number of communication
        for ( int i = 0; i < dim; ++i )
          _ens.reduce_to_chief( mpi::by::SUM, fds[i].data().data(), fds[i].data().size() );
        if ( _cart_opt ) {
          field::merge_sync_guard_cells( fds, *_cart_opt );
          smart_save( varname, dim, fds );
        }
      }
    }
  }
}

#endif
