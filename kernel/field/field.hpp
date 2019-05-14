#ifndef  _FIELD_HPP_
#define  _FIELD_HPP_

#include "field/offset.hpp"
#include "field/mesh.hpp"
#include "apt/type_traits.hpp"
#include <vector>

namespace field {
  template <typename T, int DGrid, bool Const = true >
  struct Component {
  private:
    using vector_type = apt::cond_t< Const, const std::vector<T>, std::vector<T> >;
    vector_type& _data;
    const Mesh<DGrid>& _mesh;
    const apt::array<offset_t, DGrid>& _offset;

  public:
    constexpr Component( vector_type& data, const Mesh<DGrid>& mesh, const apt::array<offset_t, DGrid>& offset ) noexcept
      : _data(data), _mesh(mesh), _offset(offset) {}

    inline T& operator() ( const apt::Index<DGrid>& i_bulk ) {
      return _data [_mesh.linearized_index_of_whole_mesh(i_bulk) ];
    }

    inline T operator() ( const apt::Index<DGrid>& i_bulk ) const {
      return _data [_mesh.linearized_index_of_whole_mesh(i_bulk) ];
    }

    inline T& operator[] ( int i ) { return _data[i]; }

    inline T operator[] ( int i ) const { return _data[i]; }

    inline const auto& offset() const noexcept { return _offset; }

    inline const auto& data() const noexcept { return _data; }
    inline auto& data() noexcept { return _data; }
  };
}

namespace field {

  template < typename T, int DField, int DGrid >
  struct Field {
  private:
    apt::array < std::vector<T>, DField > _comps;
    Mesh<DGrid> _mesh;
    apt::array< apt::array< offset_t, DGrid >, DField > _offset;

  public:
    using element_type = T;
    static constexpr int NDim = DField;

    Field() = default;

    Field( const Mesh<DGrid>& mesh ) : _mesh(mesh) {
      int size = 1;
      for ( int i = 0; i < DGrid; ++i ) size *= _mesh.extent()[i];
      apt::foreach<0,DField>
        ( [size] ( auto& comp ) {
            comp.reserve( size );
            comp.resize( size );
            for ( auto& elm : comp ) elm = static_cast<T>(0);
          }, _comps );
    }

    inline Field& set_offset( int component, const apt::array< offset_t, DGrid >& offset ) noexcept {
      _offset[component] = offset;
      return *this;
    }

    inline const auto& mesh() const noexcept { return _mesh;}

    inline const auto operator[] ( int i ) const noexcept {
      return Component<T,DGrid> ( _comps[i], _mesh, _offset[i] );
    }

    inline auto operator[] ( int i ) noexcept {
      return Component<T,DGrid,false> ( _comps[i], _mesh, _offset[i] );
    }

    inline void reset() {
      apt::foreach<0, DField>
        ( []( auto& comp ) {
            for ( auto& elm : comp ) elm = 0.0;
          }, _comps );
    }


  };
}

#endif