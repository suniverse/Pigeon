#include "particle/pusher.hpp"
#include "particle/particle.hpp"
#include "apt/numeric.hpp"
#include "core/coordinate.hpp"

namespace particle :: force {
  auto landau0 =
    [](const auto& p, const auto& E, const auto& B) {
      auto EB2 = apt::dot(E,B);
      EB2 = EB2 * EB2;
      auto B2_E2 = apt::abs_sq(B) - apt::abs_sq(E);
      // calculate E'^2
      auto Ep2 = 2 * EB2 / ( std::sqrt(B2_E2 * B2_E2 + 4 * EB2) + B2_E2 );
      auto beta_ExB = apt::cross(E,B) / ( apt::abs_sq(B) + Ep2);
      // find B' modulo gamma_ExB
      auto Bp = B - apt::cross( beta_ExB, E);
      // obtain the momentum with perpendicular components damped
      auto p_new = Bp * ( apt::dot( p, Bp ) / apt::abs_sq(Bp) );
      p_new += beta_ExB * std::sqrt( ( 1.0 + apt::abs_sq(p_new) ) / ( 1.0 - apt::abs_sq(beta_ExB) ) );
      return p_new - p;
    }

  auto lorentz = // lambda = dt / mass * e/m NOTE this is actually rescaling Lorentz force
    []( auto lambda, const auto& p, const auto& E, const auto& B ) noexcept {
      // TODO optimize use of intermediate variables
      lambda /= 2.0;

      auto u_halfstep = p + E * lambda + apt::cross(p, B) * ( lambda / std::sqrt( 1.0 + apt::abs_sq(p) ) );
      auto upr = u_halfstep + E * lambda;
      auto tau = B * lambda;
      // store some repeatedly used intermediate results
      auto tt = apt::abs_sq(tau);
      auto ut = apt::dot(upr, tau);

      auto sigma = 1.0 + apt::abs_sq(upr) - tt;
      auto inv_gamma2 =  2.0 / ( sigma + std::sqrt( sigma * sigma + 4.0 * ( tt + ut * ut ) ) ); // inv_gamma2 means ( 1 / gamma^(i+1) ) ^2
      auto s = 1.0 / ( 1.0 + inv_gamma2 * tt );
      auto p_vay = ( upr + tau * ( ut * inv_gamma2 ) + apt::cross(upr, tau) * std::sqrt(inv_gamma2) ) * s;
      return p_vay - p;
    };

}

// TODO move check of forces on_off to somewhere else
namespace particle {
  template < species sp,
             typename Tvt, std::size_t DPtc, std::size_t DField,
             typename Trl = apt::remove_cvref_t<Tvt>,
             typename Ptc = Particle<Tvt, DPtc>,
             typename T_field = Vec<Trl, DField>,
             typename T_dp = Vec<Trl,DPtc>
             >
  T_dp update_p< sp, Ptc, T_field, T_dp, Trl > ( Ptc& ptc, const Trl& dt,
                                                 const T_field& E, const T_field& B ) {
    T_dp dp;

    // Apply Lorentz force
    if ( _pane.lorentz_On  ) {
      dp += force::lorentz( dt / mass<sp>, ptc.p, E, B );
    }

    if ( _pane.gravity_On )
      dp += _pane.gravity( ptc.q ) * dt; // TODO gravity interface

    // FIXME add control in dashboard for rad_cooling
    // if ( is_radiative<sp> ) {
    //   rad_cooling(p, BVector, EVector, 2e-9, dt);
    // }

    // when B is strong enough, damp the perpendicular component of momentum
    // FIXME ions should also be affected right?
    // TODO should this go before p += dp
    if ( _pane.landau0_On && is_radiative<sp> ) {
      if ( apt::abs_sq(B) > _pane.B_landau0 * _pane.B_landau0 ) {
        dp += force::landau0( ptc.p, E, B );
      }
    }

    ptc.p += dp;

    return dp;
  }


  template < species sp,
             CoordSys CS, typename Tvt, std::size_t DPtc,
             typename Trl = apt::remove_cvref_t<Tvt>,
             typename Ptc = Particle<Tvt, DPtc>,
             typename T_dq = Vec<Trl,DPtc>
             >
  T_dq update_q< sp, CS, Ptc, T_dq, Trl > ( Ptc& ptc, const Trl& dt ) {
    auto gamma = std::sqrt( (mass<sp> > 0) + apt::abs_sq(ptc.p) );

    if constexpr ( CS == CoordSys::Cartesian ) {
      return coord<CS>::geodesic_move( ptc.q, ptc.p, dt / gamma );
    } else {
      auto dq = coord<CS>::geodesic_move( ptc.q, (ptc.p /= gamma), dt );
      ptc.p *= gamma;
      return dq;
    }

  }


}