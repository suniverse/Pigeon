#include "gen.hpp"
#include "particle_updater.hpp"

#include "field/mesh_shape_interplay.hpp"
#include "field/current_deposition.hpp"

#include "kernel/coordinate.hpp"

#include "kernel/grid.hpp"
#include "kernel/shapef.hpp"

// TODO a hotfix on multiple definition
namespace particle {
  map<Properties> properties;
}

namespace particle {
  template < int DGrid,
             typename Real,
             template < typename > class PtcSpecs,
             typename ShapeF,
             typename Real_dJ, knl::coordsys CS >
  ParticleUpdater< DGrid, Real, PtcSpecs, ShapeF, Real_dJ, CS >
  ::ParticleUpdater( const knl::Grid< Real, DGrid >& localgrid, const util::Rng<Real>& rng )
    : _localgrid(localgrid), _rng(rng) {
    set_up<Real>(); // TODO
  }
}

namespace particle {
  template < int DGrid,
             typename Real,
             template < typename > class PtcSpecs,
             typename ShapeF,
             typename RealJ, knl::coordsys CS >
  void ParticleUpdater< DGrid, Real, PtcSpecs, ShapeF, RealJ, CS >
  ::update_species( species sp,
                    array<Real,PtcSpecs>& sp_ptcs,
                    field::Field<RealJ,3,DGrid>& J,
                    Real dt,
                    const field::Field<Real,3,DGrid>& E,
                    const field::Field<Real,3,DGrid>& B
                    ) {
    if ( sp_ptcs.size() == 0 ) return;

    const auto& prop = properties.at(sp);

    using Ptc = typename array<Real,PtcSpecs>::particle_type;

    auto update_p = force_gen<Real,PtcSpecs,vParticle>(sp);

    auto* scat = scat_gen<Real,PtcSpecs>(sp);

    constexpr auto shapef = ShapeF();
    auto charge_over_dt = static_cast<Real>(prop.charge_x) / dt;

    auto update_q =
      [is_massive=(prop.mass_x != 0)] ( auto& ptc, Real dt ) {
        auto gamma = std::sqrt( is_massive + apt::sqabs(ptc.p()) );

        if constexpr ( CS == knl::coordsys::Cartesian ) {
            // a small optimization for Cartesian
            knl::coord<CS>::geodesic_move( ptc.q(), ptc.p(), dt / gamma );
          } else {
          knl::coord<CS>::geodesic_move( ptc.q(), (ptc.p() /= gamma), dt );
          ptc.p() *= gamma;
        }
      };

    auto abs2std =
      [&grid=_localgrid]( const auto& qabs ) {
        apt::array<Real,PtcSpecs<Real>::Dim> q_std;
        apt::foreach<0,DGrid> // NOTE DGrid instead of DPtc
          ( [](auto& q, auto q_abs, const auto& g ) noexcept {
              q = ( q_abs - g.lower() ) / g.delta();
            }, q_std, qabs, grid );
        return q_std;
      };

    for ( auto ptc : sp_ptcs ) { // TODOL sematics, check ptc is proxy
      if( ptc.is(flag::empty) ) continue;

      {
        auto q0_std = abs2std( ptc.q() );
        auto E_itpl = field::interpolate( E, q0_std, shapef );
        auto B_itpl = field::interpolate( B, q0_std, shapef );

        apt::Vec<Real,PtcSpecs<Real>::Dim> dp = -ptc.p();
        update_p( ptc, dt, E_itpl, B_itpl );
        if ( scat ) {
          dp += ptc.p();
          (*scat)( std::back_inserter(sp_ptcs), ptc, std::move(dp), dt, B_itpl, _rng );
        }

        // NOTE q is updated, starting from here, particles may be in the guard cells.
        update_q( ptc, dt );
        // TODO pusher handle boundary condition. Is it needed?
        if ( prop.charge_x != 0 )
          deposit( J, charge_over_dt, shapef, q0_std, abs2std(ptc.q()) );
      }

    }
  }
}

namespace particle {
  template < int DGrid,
             typename Real,
             template < typename > class PtcSpecs,
             typename ShapeF,
             typename RealJ,
             knl::coordsys CS >
  void ParticleUpdater< DGrid, Real, PtcSpecs, ShapeF, RealJ, CS >
  ::operator() ( map<array<Real,PtcSpecs>>& particles,
                 field::Field<RealJ,3,DGrid>& J,
                 const field::Field<Real,3,DGrid>& E,
                 const field::Field<Real,3,DGrid>& B,
                 Real dt, int timestep ) {

    for ( auto&[ sp, ptcs ] : particles ) {
      const auto old_size = ptcs.size();
      update_species( sp, ptcs, J, dt, E, B );

      // Put particles where they belong after scattering
      for ( auto i = old_size; i < ptcs.size(); ++i ) {
        auto this_sp = ptcs[i].template get<species>();
        if ( this_sp != sp ) {
          particles[this_sp].push_back(std::move(ptcs[i]));
        }
      }
    }

    integrate(J);

    // NOTE rescale Jmesh back to real grid delta
    for ( int i = 0; i < DGrid; ++i ) {
      auto comp = J[i]; // TODOL semantics;
      for ( auto& elm : comp.data() ) elm *= _localgrid[i].delta();
    }

  }

}

#include "pic.hpp"
using namespace pic;
namespace particle {
  template class ParticleUpdater< DGrid, real_t, Specs, ShapeF, real_j_t, coordinate_system >;
}