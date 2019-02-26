#include "particle/migration.hpp"
#include "particle/c_particle.hpp"
#include "parallel/mpi++.hpp"
#include <memory>

namespace mpi {
  template < typename Ptc >
  MPI_Datatype datatype_for_cParticle() {
    using T = typename Ptc::vec_type::value_type;
    constexpr int DPtc = Ptc::Dim;
    using state_t = typename Ptc::state_type;

    std::vector< Ptc > ptcs(2);
    constexpr int numBlocks = 3;
    MPI_Datatype type[numBlocks] = { datatype<T>(), datatype<T>(), datatype<state_t>() };
    int blocklen[numBlocks] = { DPtc, DPtc, 1 };
    MPI_Aint disp[numBlocks];

    MPI_Get_address( &(ptcs[0].q()), disp );
    MPI_Get_address( &(ptcs[0].p()), disp+1 );
    MPI_Get_address( &(ptcs[0].state()), disp+2 );

    auto base = disp[0];
    for ( int i = 0; i < numBlocks; ++i )
      disp[i] = MPI_Aint_diff( disp[i], base );

    // first create a tmp type
    MPI_Datatype mdt_tmp;
    MPI_Type_create_struct( numBlocks, blocklen, disp, type, &mdt_tmp );
    // adjust in case of mysterious compiler padding
    MPI_Aint sizeofentry;
    MPI_Get_address( ptcs.data() + 1, &sizeofentry );
    sizeofentry = MPI_Aint_diff(sizeofentry, base);

    MPI_Datatype mdt_ptc;
    MPI_Type_create_resized(mdt_tmp, 0, sizeofentry, &mdt_ptc);
    return mdt_ptc;
  }

  namespace { MPI_Datatype MPI_CPARTICLE = MPI_DATATYPE_NULL; }

  template < typename T, int DPtc, typename state_t >
  constexpr MPI_Datatype datatype( const particle::cParticle<T,DPtc,state_t>* ) noexcept {
    return MPI_CPARTICLE;
  }

}


namespace particle :: impl {

  // NOTE Assume there is no empty particles.
  template < typename Buffer, typename F_LCR >
  auto lcr_sort( Buffer& buffer, const F_LCR& lcr ) noexcept {
    constexpr int L = 0;
    constexpr int C = 1;
    constexpr int R = 2;

    // ec points to end of ccc.., el points to end of lll..., and er points to the reverse end of ...rrr.
    // the upshot: ccc.. will be [0, ic), lll... will be [ic, ilr+1), rrr... will be [ilr+1, buffer.size()), edge case safe
    int ec = 0, el = 0, er = buffer.size() - 1;
    while ( el <= er ) {
      switch ( lcr( buffer[el] ) ) {
      case L : el++; break;
      case C :
        if ( ec != el ) std::swap( buffer[el], buffer[ec] );
        ec++; el++; break;
      case R :
        while ( el <= er && lcr( buffer[er] ) != R ) er--;
        if ( er != el ) std::swap( buffer[el], buffer[er] );
        er--;
        break;
      }
    }

    return std::array<const int, 3>{ ec, el, buffer.size() };
  }


  template < typename Ptc, typename F_LCR >
  void migrate_1dim ( std::vector<Ptc>& buffer,
                      const std::array<std::optional<mpi::InterComm>, 2>& intercomms,
                      const F_LCR& lcr, unsigned int shift ) {
    // sort order is center | left | right | empty. Returned are the delimiters between these catogories
    const auto begs = lcr_sort( buffer, lcr ); // begs = { begL, begR, begE_original };

    // NOTE buffer may be relocated in response to size growing. DO NOT store its pointers or references
    int begE_run = begs[2]; // running begin of empty particles in buffer
    for ( int lr = 0; lr < 2; ++lr ) {
      std::vector<mpi::Request> reqs;
      // sending
      if ( intercomms[lr] ) {
        const auto& send_comm = *intercomms[lr];
        int local_rank = send_comm.rank();
        int remote_dest = ( local_rank + shift ) % send_comm.remote_size();
        reqs.push_back( send_comm.Isend( remote_dest, 147, buffer.data() + begs[lr], begs[lr+1] - begs[lr] ) );
      }

      // receiving
      std::unique_ptr<Ptc[]> p_tmp(nullptr);
      int tot_num_recv = 0;
      if ( intercomms[1-lr] ) {
        const auto& recv_comm = *intercomms[1-lr];
        int local_rank = recv_comm.rank();
        int local_size = recv_comm.size();
        int remote_size = recv_comm.remote_size();
        std::vector<int> remote_srcs;
        std::vector<int> scan_recv_counts = {0}; // exclusive scan
        int src_rank = ( local_rank + local_size - (shift % local_size) ) % local_size;
        while ( src_rank < remote_size ) {
          remote_srcs.push_back(src_rank);
          scan_recv_counts.push_back( scan_recv_counts.back() + recv_comm.probe( src_rank, 147, buffer[0] ) );
          src_rank += local_size;
        }

        tot_num_recv = scan_recv_counts.back();
        // If recved more than space allows, store them in a temporary buffer then later merge with the primary buffer
        Ptc* p_recv = buffer.data() + begE_run;
        if ( tot_num_recv > buffer.capacity() - begE_run ) {
          p_tmp.reset( new Ptc [tot_num_recv] );
          p_recv = p_tmp.get();
        }

        for ( int i = 0; i < remote_srcs.size(); ++i ) {
          reqs.push_back( recv_comm.Irecv( remote_srcs[i], 147, p_recv + scan_recv_counts[i], scan_recv_counts[i+1] - scan_recv_counts[i] ) );
        }

      }

      mpi::waitall(reqs);

      // merge into buffer if needed.
      if ( intercomms[1-lr] && ( tot_num_recv > buffer.capacity() - begE_run ) ) {
        buffer.resize( begE_run + tot_num_recv );
        for ( int i = 0; i < tot_num_recv; ++i )
          buffer[begE_run + i] = p_tmp[i];
        p_tmp.reset(nullptr);
      }
      begE_run += tot_num_recv;
    }

    // erase sent particles by shifting
    for ( int i = 0; i < begE_run - begs[2]; ++i )
      buffer[begs[0] + i] = buffer[begs[2] + i];

    // NOTE It is essential to not have empty particles within buffer.size otherwise lcr_sort will fail.
    buffer.resize(begs[0] + begE_run - begs[2]);
  }
}

namespace particle {
  template < typename Vec, int DGrid, typename T >
  bool is_migrate( const apt::VecExpression<Vec>& q,
                   const std::array< std::array<T, 2>, DGrid>& borders ) noexcept {
    bool res = false;
    apt::foreach<0,DGrid>
      ( [&res]( const auto& x, const auto& bd ) noexcept {
          res |= ( x < std::get<0>(bd) || x > std::get<1>(bd) );
        }, q, borders );

    return res;
  }

  namespace impl {
    // NOTE must use std::size_t for DGrid, otherwise type mismatched will be triggered for std::array
    template < int I, typename T, int DPtc, typename state_t, std::size_t DGrid >
    inline void migrate( std::vector<cParticle<T,DPtc,state_t>>& buffer,
                         const std::array< std::array<std::optional<mpi::InterComm>, 2>, DGrid >& intercomms,
                         const std::array< std::array<T,2>, DGrid >& borders,
                         unsigned int shift ) {
      auto&& lcr =
        [&bd=std::get<I>(borders)]( const auto& ptc ) noexcept {
          return ( std::get<I>(ptc.q()) >= std::get<0>(bd) ) + ( std::get<I>(ptc.q()) > std::get<1>(bd) );
        };
      impl::migrate_1dim( buffer, std::get<I>(intercomms), std::move(lcr), shift );
      if constexpr ( I > 0 )
                     migrate<I-1>( buffer, intercomms, borders, shift );
    }
  }

  template < typename T, int DPtc, typename state_t, int DGrid >
  void migrate ( std::vector<cParticle<T,DPtc,state_t>>& buffer,
                 const std::array< std::array<std::optional<mpi::InterComm>,2>, DGrid >& intercomms,
                 const std::array< std::array<T,2>, DGrid>& borders,
                 unsigned int pairing_shift ) {
    if ( mpi::MPI_CPARTICLE == MPI_DATATYPE_NULL )
      mpi::MPI_CPARTICLE = mpi::datatype_for_cParticle<cParticle<T,DPtc,state_t>>();

    MPI_Type_commit(&mpi::MPI_CPARTICLE);
    impl::migrate<DGrid-1>( buffer, intercomms, borders, pairing_shift );
    MPI_Type_free( &mpi::MPI_CPARTICLE );
  }

}
