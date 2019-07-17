#include "testfw/testfw.hpp"
#include "ckpt/checkpoint_impl.hpp"
#include <unordered_set>

using Real = double;
using State_t = long long;
constexpr int DGrid = 2;
template < typename T >
struct S {
  using value_type = T;
  static constexpr int Dim = 3;
  using state_type = apt::copy_cvref_t<T, State_t>;
};

namespace particle {
  map<Properties> properties =
    [](){
      map<Properties> res;
      res[species::electron] = {1,-1,"electron"};
      res[species::positron] = {1,1,"positron"};
      return res;
    }();
}

struct StateClass : public particle::StateExpression<StateClass, State_t> {
  State_t s{};
  constexpr State_t& state() noexcept { return s; }
  constexpr State_t state() const noexcept { return s; }
};

using namespace particle;

struct Ens {
  std::vector<map<load_t>> rank;
};

SCENARIO("Test save_checkpoint", "[ckpt]") {
  const apt::array<int,DGrid> partition { 1, 1 };
  int num_ens = 1;
  for ( auto x : partition ) num_ens *= x;

  std::vector<int> ens_nprocs = {1};
  REQUIRE( ens_nprocs.size() == num_ens );

  std::vector<Ens> ens( num_ens );
  for ( int i = 0; i < num_ens; ++i )
    ens[i].rank.resize( ens_nprocs[i] );

  const std::vector<species> sps_exist { species::electron, species::positron };
  apt::array<int, DGrid> bulk_dims { 128, 128 };
  const int guard = 1;

  ens[0].rank[0] = {{ species::electron, 709 }, { species::positron, 64}};
  std::string prefix = "PUBG";
  const int num_parts = 2;
  const int timestep = 1234;

  auto fq = []( int comp ) -> Real { return std::exp( comp ); };
  auto fp = []( int comp ) -> Real { return std::cos( comp ); };

  std::string save_dir;
  int num_procs = 0;
  {
    for ( int i = 0; i < num_ens; ++i ) {
      num_procs += ens_nprocs[i];
    }
    REQUIRE( num_procs <= mpi::world.size() );

    auto rwld_opt = aio::reduced_world(num_procs, mpi::world);
    if ( rwld_opt ) {
      auto cart_opt = aio::make_cart( partition, *rwld_opt );
      auto color = aio::color_from_nprocs( ens_nprocs, rwld_opt->rank() );
      auto intra = rwld_opt->split(color);
      auto ens_opt = dye::create_ensemble<DGrid>( cart_opt, intra );

      field::Field<Real, 3, DGrid> E( {bulk_dims, guard} );
      field::Field<Real, 3, DGrid> B( {bulk_dims, guard} );

      map<array<Real,S>> particles;

      if ( ens_opt ) {
        int label = ens_opt->label();
        int rank = ens_opt->intra.rank();
        if ( cart_opt ) {
          for ( int i = 0; i < 3; ++i ) {
            for ( auto& x : E[i].data() ) x = 1.23 + i;
            for ( auto& x : B[i].data() ) x = 147.147 + i;
          }
        }

        for ( auto[sp,load] : ens[label].rank[rank] ) {
          auto& ptcs = particles[sp];
          for ( int i = 0; i < load; ++i ) {
            ptcs.emplace_back({i * fq(0),i * fq(1),i * fq(2)},
                              {-i * fp(0),-i * fp(1),-i * fp(2)},
                              sp);
          }
        }
      }

      save_dir = ckpt::save_checkpoint(prefix, num_parts, ens_opt, timestep, E, B, particles);
    }

    // test
    if ( mpi::world.rank() == 0 ) {
      REQUIRE(fs::exists(save_dir));
      int nparts = 0;
      std::unordered_set<int> ens_uncovered;
      for ( auto f : fs::directory_iterator(save_dir) ) {
        ++nparts;
        auto sf = silo::open( f, silo::Mode::Read );
        REQUIRE( sf.var_exists("/timestep") );
        REQUIRE( sf.read1<int>("/timestep") == timestep );
        REQUIRE( sf.var_exists("/cartesian_partition") );
        {
          apt::array<int,DGrid> x;
          sf.read("/cartesian_partition", x.begin() );
          REQUIRE( x == partition );
        }

        REQUIRE( sf.var_exists("/species") );
        {
          auto x = sf.read1d<species>("/species");
          REQUIRE( x == sps_exist );
        }

        for ( auto edir : sf.toc_dir() ) {
          if ( edir.find("ensemble") == 0 ) {
            sf.cd(edir);
            REQUIRE( sf.var_exists("label") );
            int l = sf.read1<int>("label");
            ens_uncovered.insert(l);
            {
              REQUIRE( sf.var_exists("cartesian_coordinates") );
              // NOTE cartesian rank is the same as ensemble label. TODO cartesian coordinates is not tested
            }
            for ( auto rdir : sf.toc_dir() ) {
              if ( rdir.find("rank") == 0 ) {
                sf.cd(rdir);
                REQUIRE( sf.var_exists("r") );
                const int r = sf.read1<int>("r");
                if ( r == 0 ) {
                  sf.cd("..");
                  for( auto sp : sps_exist ) {
                    std::string load_var = properties[sp].name + "_load";
                    REQUIRE( sf.var_exists(load_var) );
                    load_t total = 0;
                    for ( const auto& r : ens[l].rank ) total += r.at(sp);
                    REQUIRE( total == sf.read1<load_t>(load_var) );
                  }
                  int field_size = 1;
                  for ( int i = 0; i < DGrid; ++i ) field_size *= ( 2 * guard + bulk_dims[i] );
                  for ( int i = 0; i < 3; ++i ) {
                    REQUIRE( sf.var_exists("E"+std::to_string(i+1)) );
                    auto vars = sf.read1d<Real>("E"+std::to_string(i+1));
                    REQUIRE( vars.size() == field_size );
                    for ( auto x : vars ) REQUIRE( x == i + 1.23 );
                  }
                  for ( int i = 0; i < 3; ++i ) {
                    REQUIRE( sf.var_exists("B"+std::to_string(i+1)) );
                    auto vars = sf.read1d<Real>("B"+std::to_string(i+1));
                    REQUIRE( vars.size() == field_size );
                    for ( auto x : vars ) REQUIRE( x == i + 147.147 );
                  }

                  sf.cd(rdir);
                }
                for ( auto sp : sps_exist ) {
                  REQUIRE( sf.var_exists(properties[sp].name) );
                  sf.cd(properties[sp].name);
                  auto load = sf.read1<load_t>("load");
                  REQUIRE( load == ens[l].rank[r][sp] );
                  for ( int i = 0; i < S<Real>::Dim; ++i ) {
                    {
                      std::string var = "q" + std::to_string(i+1);
                      REQUIRE( sf.var_exists(var) );
                      auto qs = sf.read1d<Real>(var);
                      REQUIRE( qs.size() == load );
                      for ( int j = 0; j < qs.size(); ++j )
                        REQUIRE( qs[j] == j * fq(i) );
                    }
                    {
                      std::string var = "p" + std::to_string(i+1);
                      REQUIRE( sf.var_exists(var) );
                      auto ps = sf.read1d<Real>(var);
                      REQUIRE( ps.size() == load );
                      for ( int j = 0; j < ps.size(); ++j )
                        REQUIRE( ps[j] == - j * fp(i) );
                    }
                  }
                  {
                    REQUIRE( sf.var_exists("state") );
                    StateClass state;
                    state.set( sp, flag::exist );
                    auto ss = sf.read1d<State_t>("state");
                    REQUIRE( ss.size() == load );
                    for ( auto s : ss ) REQUIRE( s == state.s );
                  }
                  sf.cd("..");
                }
                sf.cd("..");
              }
            }
            sf.cd("..");
          }
        }
      }
      REQUIRE( nparts == std::min<int>(num_parts, num_procs) );
      {
        std::unordered_set<int> ens_set;
        for ( int i = 0; i < num_ens; ++i ) ens_set.insert(i);
        REQUIRE( ens_uncovered == ens_set );
      }

      fs::remove_all(prefix);
    }
  }

  // SCENARIO("Test load_checkpoint", "[ckpt]") {





  //   const apt::array<int,DGrid> partition { 1, 1 };
  //   const int num_procs = mpi::world.size();

  //   auto rwld_opt = aio::reduced_world(num_procs, mpi::world);
  //   if ( rwld_opt ) {
  //     auto cart_opt = aio::make_cart( partition, *rwld_opt );
  //     auto ens_opt = dye::create_ensemble<DGrid>( cart_opt );

  //     apt::array<int, DGrid> bulk_dims { 128, 128 };
  //     const int guard = 1;

  //     field::Field<Real, 3, DGrid> E( {bulk_dims, guard} );
  //     field::Field<Real, 3, DGrid> B( {bulk_dims, guard} );

  //     const int num_ptcs = 709;
  //     map<array<Real,S>> particles;
  //     for ( const auto& [sp, ignore] : properties )
  //       particles.emplace( sp, array<Real, S>() );


  //     {
  //     ens_opt = dye::create_ensemble<DGrid>( cart_opt );


  //     int ckpt::load_checkpoint(save_dir, )
  //       }
  //   }
  // }

}