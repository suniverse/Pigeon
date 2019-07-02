#include "field/sync.hpp"
#include "field/field.hpp"
#include "mpipp/mpi++.hpp"
#include "apt/block.hpp"

// TODOL can we use the most continous part of field for another temporary buffer? More generally, can we change the memory layout? Because guard cells in other directions are not continous anyway. Those guard cells can be used as temporary buffers.
namespace field {

  namespace impl {
    template < typename T, int DField, int DGrid, template < typename > class Policy >
    void sync( Field<T, DField, DGrid>& field, const mpi::CartComm& comm, Policy<T> ) {
      // Looping order ( from outer to inner ): DGrid, LeftRightness, DField
      const auto& mesh = field.mesh();
      auto I_b = mesh.origin();
      auto extent = mesh.extent();

      std::vector<T> send_buf, recv_buf;

      for ( int i_grid = 0; i_grid < DGrid; ++ i_grid ) {
        int ext_orig = extent[i_grid];
        extent[i_grid] = Policy<T>::extent(mesh.guard());
        int ext_size = 1;
        apt::foreach<0,DGrid>
          ( [&ext_size] (auto e) { ext_size *= e;}, extent );

        int I_b_orig = I_b[i_grid];
        for ( int lr_send = 0; lr_send < 2; ++lr_send ) { // 0 is to left, 1 is to right

          auto [ src, dest ] = comm.shift( i_grid, lr_send ? 1 : -1 );
          // Edge case: cart dim = 1 with periodic boundaries
          if ( src && (*src == comm.rank()) ) { // NOTE check on dest is not needed
            // use I_b for I_b_send
            auto I_b_recv = I_b;
            I_b[i_grid] = Policy<T>::sendIb( mesh.bulk_dim(i_grid), mesh.guard(), lr_send );
            I_b_recv[i_grid] = Policy<T>::recvIb( mesh.bulk_dim(i_grid), mesh.guard(), lr_send );
            apt::foreach<0,DField>
              ( [&]( auto comp ) { // TODOL semantics
                  for ( const auto& I : apt::Block(extent) ) {
                    T buf{}; // This is needed to accommodate merge_guard
                    Policy<T>::to_send_buf( buf, comp(I_b + I) );
                    Policy<T>::from_recv_buf( comp(I_b_recv + I), buf, lr_send );
                  }
                }, field );
          } else {
            std::vector<mpi::Request> reqs(2);
            // copy all components into one buffer
            if ( dest ) {
              send_buf.resize( ext_size * DField );
              auto itr_s = send_buf.begin();
              I_b[i_grid] = Policy<T>::sendIb( mesh.bulk_dim(i_grid), mesh.guard(), lr_send );
              apt::foreach<0,DField>
                ( [&]( auto comp ) { // TODOL semantics
                    for ( const auto& I : apt::Block(extent) )
                      Policy<T>::to_send_buf( *(itr_s++), comp(I_b + I) );
                  }, field );
              reqs[0] = comm.Isend( *dest, 924, send_buf.data(), send_buf.size() );
            }
            if ( src ) {
              recv_buf.resize( ext_size * DField );
              reqs[1] = comm.Irecv( *src, 924, recv_buf.data(), recv_buf.size() );
            }
            mpi::waitall(reqs);
            if ( src ) {
              auto itr_r = recv_buf.cbegin();
              I_b[i_grid] = Policy<T>::recvIb( mesh.bulk_dim(i_grid), mesh.guard(), lr_send );
              apt::foreach<0,DField>
                ( [&]( auto comp ) { // TODOL semantics
                    for ( const auto& I : apt::Block(extent) )
                      Policy<T>::from_recv_buf( comp(I_b + I), *(itr_r++), lr_send );
                  }, field );
            }
          }

        }
        // restroe
        I_b[i_grid] = I_b_orig;
        extent[i_grid] = ext_orig;
      }
    }
  }


}

namespace field {
  template < typename T >
  struct copy_policy {
    static constexpr bool is_copy_sync = true;

    static constexpr void to_send_buf( T& v_buf, const T& v_data ) noexcept {
      v_buf = v_data;
    }

    static constexpr void from_recv_buf( T& v_data, const T& v_buf, bool ) noexcept {
      v_data = v_buf;
    }

    static constexpr int sendIb( int bulk_dim, int guard, bool is_send_to_right ) noexcept {
      return is_send_to_right ? ( bulk_dim - guard ) : 0;
    }

    static constexpr int recvIb( int bulk_dim, int guard, bool is_send_to_right ) noexcept {
      return is_send_to_right ? - guard : bulk_dim;
    }

    static constexpr int extent( int guard ) noexcept { return guard; }
  };

  template < typename T, int DField, int DGrid >
  void copy_sync_guard_cells( Field<T, DField, DGrid>& field, const mpi::CartComm& comm ) {
    impl::sync( field, comm, copy_policy<T>{} );
  }

  template < typename T >
  struct merge_policy {
    static constexpr bool is_copy_sync = false;

    static constexpr void to_send_buf( T& v_buf, T& v_data ) noexcept {
      v_buf = v_data;
    }

    static constexpr void from_recv_buf( T& v_data, const T& v_buf, bool is_send_to_right ) noexcept {
      if ( is_send_to_right )
        v_data = v_buf;
      else
        v_data += v_buf;
    }

    static constexpr int sendIb( int bulk_dim, int guard, bool is_send_to_right ) noexcept {
      return is_send_to_right ? bulk_dim - guard : - guard;
    }

    static constexpr int recvIb( int bulk_dim, int guard, bool is_send_to_right ) noexcept {
      return is_send_to_right ? -guard : ( bulk_dim - guard );
    }

    static constexpr int extent( int guard ) noexcept { return 2 * guard; }
  };

  template < typename T, int DField, int DGrid >
  void merge_sync_guard_cells( Field<T, DField, DGrid>& field, const mpi::CartComm& comm ) {
    impl::sync( field, comm, merge_policy<T>{} );
  }
}