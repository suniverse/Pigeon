#ifndef  _PIC_SIMULATOR_HPP_
#define  _PIC_SIMULATOR_HPP_

#include "manifold/grid.hpp"

#include "field/updater.hpp"
#include "particle/updater.hpp"
#include "field/sync.hpp"

#include "particle/migration.hpp"
#include "particle/sorter.hpp"
#include "io/io.hpp"
#include "dye/dynamic_balance.hpp"
#include "ckpt/checkpoint.hpp"
#include "ckpt/autosave.hpp"

#include <memory>

#include "gen.hpp"

#include "logger/logger.hpp"
#include "timer/timer.hpp"
#include "filesys/filesys.hpp"

#include "pic/stats.hpp"

#ifdef PIC_DEBUG
#include "debug/debugger.hpp"
#endif

namespace pic {
  std::string this_run_dir;

  constexpr real_t classic_electron_radius () noexcept {
    real_t res = wdt_pic * wdt_pic / ( 4.0 * std::acos(-1.0l) * dt * dt);
    apt::foreach<0,DGrid>( [&res](const auto& g) { res *= g.delta(); }, supergrid );
    return res;
  }


  template < int DGrid,
             typename Real,
             template < typename > class PtcSpecs,
             typename ShapeF,
             typename RealJ,
             typename Metric >
  struct Simulator {
  private:
    const mani::Grid< Real, DGrid >& _supergrid;
    const int _guard;
    std::optional<mpi::CartComm> _cart_opt;
    util::Rng<Real> _rng;

    mani::Grid< Real, DGrid > _grid;
    std::optional<dye::Ensemble<DGrid>> _ens_opt;
    apt::array< apt::pair<Real>, DGrid > _borders;

    field::Field<Real, 3, DGrid> _E;
    field::Field<Real, 3, DGrid> _B;
    field::Field<RealJ, 3, DGrid> _J;
    particle::map<particle::array<Real, PtcSpecs>> _particles;

    std::unique_ptr<field::Updater<Real,DGrid,RealJ>> _field_update;
    std::unique_ptr<particle::Updater<DGrid,Real,PtcSpecs,ShapeF,RealJ,Metric>> _ptc_update;

    std::vector<particle::Particle<Real, PtcSpecs>> _migrators;

    bc::Axissymmetric<DGrid> _fbc_axis;
    bc::Injector< DGrid, Real > _injector;

    // ScalarField<Scalar> pairCreationEvents; // record the number of pair creation events in each cell.
    // PairCreationTracker pairCreationTracker;

    constexpr auto all_but( field::offset_t all, int but_comp ) noexcept {
      apt::array<field::offset_t, DGrid> res;
      apt::foreach<0,DGrid>( [&all]( auto& ofs ) { ofs = all; }, res );
      if ( but_comp < DGrid ) res[but_comp] = !all;
      return res;
    }

    void update_parts( const dye::Ensemble<DGrid>& ens ) {
      for ( int i = 0; i < DGrid; ++i ) {
        _grid[i] = _supergrid[i].divide( ens.cart_dims[i], ens.cart_coords[i] );
        // TODO cart_dim = 1 and periodic
        _borders[i] = { _grid[i].lower(), _grid[i].upper() };
      }

      std::tie(_fbc_axis, _injector) = pic::set_up_boundary_conditions(_grid);

      if ( _cart_opt )
        _field_update.reset(new field::Updater<Real,DGrid,RealJ>
                            ( *_cart_opt, _grid,
                              ens.is_at_boundary(), _guard,
                              field::omega_spinup,
                              pic::classic_electron_radius() / pic::w_gyro_unitB )
                            );

      _ptc_update.reset(new particle::Updater<DGrid, Real, PtcSpecs, ShapeF, RealJ, Metric> (particle::properties));
      if ( pic::msperf_qualified(_ens_opt) ) lgr::file.open();
    }

    void migrate_particles( int timestep ) {
      // bulk range = [lb, ub)
      constexpr auto migrate_code =
        []( auto q, auto lb, auto ub ) noexcept {
          return ( q >= lb ) + ( q >= ub );
        };

      for ( auto sp : _particles ) {
        for ( auto ptc : _particles[sp] ) { // TODOL semantics
          if ( !ptc.is(particle::flag::exist) ) continue;
          particle::migrInt<DGrid> mig_dir{};
          for ( int i = 0; i < DGrid; ++i ) {
            mig_dir += migrate_code( ptc.q()[i], _borders[i][LFT], _borders[i][RGT] ) * apt::pow3(i);
          }

          if ( mig_dir != ( apt::pow3(DGrid) - 1 ) / 2 ) {
            mig_dir.imprint(ptc);
            _migrators.emplace_back(std::move(ptc));
          }
        }
      }

      particle::migrate( _migrators, _ens_opt->inter, timestep );
      for ( auto&& ptc : _migrators ) {
        auto sp = ptc.template get<particle::species>();
        ptc.template reset<particle::destination>();
        _particles[sp].push_back( std::move(ptc) );
      }
      _migrators.resize(0);
    }

    template < typename MR >
    inline bool is_do( const MR& mr, int timestep ) const noexcept {
      return mr.is_on && timestep >= mr.init_ts && (timestep % mr.interval == 0 );
    }

  public:
    Simulator( const mani::Grid< Real, DGrid >& supergrid, const std::optional<mpi::CartComm>& cart_opt, int guard )
      : _supergrid(supergrid), _guard(guard), _cart_opt(cart_opt) {
      _grid = supergrid;
      if ( pic::dlb_init_replica_deploy ) {
        std::optional<int> label{};
        if ( _cart_opt )
          label.emplace( _cart_opt->rank() );
        else {
          int num_ens = 1;
          for ( int i = 0; i < DGrid; ++i ) num_ens *= pic::dims[i];

          int my_rank = mpi::world.rank();
          int count = num_ens;
          for ( int i = 0; i < num_ens; ++i ) {
            int num_replicas = (*pic::dlb_init_replica_deploy)(i);
            if ( my_rank >= count && my_rank < count + num_replicas ) {
              label.emplace(i);
              break;
            } else
              count += num_replicas;
          }
        }

        auto intra_opt = mpi::world.split(label);
        _ens_opt = dye::create_ensemble<DGrid>(cart_opt, intra_opt);
      } else {
        _ens_opt = dye::create_ensemble<DGrid>(cart_opt);
      }

      { // initialize fields. Idles also do this for the sake of loading checkpoint, so pic::dims is used instead of _ens_opt or _cart_opt
        apt::Index<DGrid> bulk_dims;
        for ( int i = 0; i < DGrid; ++i ) {
          bulk_dims[i] = _supergrid[i].dim() / pic::dims[i];
        }
        _E = {{ bulk_dims, _guard }};
        _B = {{ bulk_dims, _guard }};
        // NOTE minimum number of guards of J on one side is ( supp + 1 ) / 2 + 1
        _J = {{ bulk_dims, ( ShapeF::support() + 3 ) / 2 }};

        for( int i = 0; i < 3; ++i ) {
          _E.set_offset( i, all_but( MIDWAY, i ) );
          _B.set_offset( i, all_but( INSITU, i ) );
          _J.set_offset( i, all_but( MIDWAY, i ) );
        }
        _E.reset();
        _B.reset();
        _J.reset();
      }
      // NOTE all species in the game should be created regardless of whether they appear on certain processes. This is to make the following work
      // 1. detailed balance. Absence of some species may lead to deadlock to transferring particles of that species.
      // 2. data export. PutMultivar requires every patch to output every species
      for ( auto sp : particle::properties )
        _particles.insert( sp, particle::array<Real, PtcSpecs>() );

      if ( _ens_opt ) update_parts(*_ens_opt);
    }

    int load_initial_condition( std::optional<std::string> checkpoint_dir ) {
      int init_ts = 0;
      if ( checkpoint_dir ) {
        init_ts = ckpt::load_checkpoint( *checkpoint_dir, _ens_opt, _cart_opt, _E, _B, _particles, particle::N_scat );
        ++init_ts; // checkpoint is saved at the end of a timestep
        if ( _ens_opt ) update_parts(*_ens_opt);
      } else {
        // TODOL a temporary fix, which may crash under the edge case in which initially many particles are created
        if (_cart_opt) {
          auto ic = pic::set_up_initial_conditions(_grid);
          ic(_grid, _E, _B, _J, _particles);
          init_ts = ic.initial_timestep();
          field::copy_sync_guard_cells(_E, *_cart_opt);
          field::copy_sync_guard_cells(_B, *_cart_opt);
          field::copy_sync_guard_cells(_J, *_cart_opt);
        }
      }
      // broadcast to ensure uniformity
      mpi::world.broadcast( 0, &init_ts, 1 );
      return init_ts;
    }

    inline void set_rng_seed( int seed ) { _rng.set_seed(seed); }

    // NOTE we choose to do particle update before field update so that in saving snapshots we don't need to save _J
    void evolve( int timestep, Real dt ) {
      if ( timestep % pic::cout_ts_interval == 0 && mpi::world.rank() == 0 )
        std::cout << "==== Timestep " << timestep << " ====" << std::endl;

#ifdef PIC_DEBUG
      debug::timestep = timestep;
      debug::world_rank = mpi::world.rank();
      if ( _ens_opt ) debug::ens_label = _ens_opt -> label();
#endif

      std::optional<tmr::Timestamp> stamp_all;
      std::optional<tmr::Timestamp> stamp;
      if ( is_do(pic::msperf_mr, timestep) && pic::msperf_qualified(_ens_opt) ) {
        stamp_all.emplace();
        stamp.emplace();
        if ( msperf_max_entries &&
             ( timestep - msperf_mr.init_ts > *msperf_max_entries * msperf_mr.interval )  ) lgr::file.clear();
        lgr::file << "==== Timestep " << timestep << " ====" << std::endl;
        lgr::file.indent_append("\t");
      }

      if ( _ens_opt ) {
        const auto& ens = *_ens_opt;

        if ( stamp ) {
          lgr::file % "BroadcastEB" << "==>>" << std::endl;
          stamp.emplace();
        }
        // TODOL reduce number of communications?
        for ( int i = 0; i < 3; ++i )
          ens.intra.broadcast( ens.chief, _E[i].data().data(), _E[i].data().size() );
        for ( int i = 0; i < 3; ++i )
          ens.intra.broadcast( ens.chief, _B[i].data().data(), _B[i].data().size() );
        if ( stamp ) {
          lgr::file % "\tLapse " << stamp->lapse().in_units_of("ms") << std::endl;
        }

        _J.reset();

        if ( is_do(pic::sort_particles_mr, timestep) ) {
          if (stamp) {
            lgr::file % "SortParticles" << "==>>" << std::endl;
            stamp.emplace();
          }
          for ( auto sp : _particles ) particle::sort( _particles[sp] );
          if (stamp) {
            lgr::file % "\tLapse " << stamp->lapse().in_units_of("ms") << std::endl;
          }
        }

        if ( stamp && _particles.size() != 0 ) {
          lgr::file % "ParticleCounts:" << std::endl;
          for ( auto sp : _particles )
            lgr::file % "\t" << particle::properties[sp].name << " = " << _particles[sp].size() << std::endl;
        }

        // ----- before this line particles are all within borders --- //
        if (stamp) {
          lgr::file % "ParticleUpdate" << "==>>" << std::endl;
          stamp.emplace();
        }
        (*_ptc_update) ( _particles, _J, _E, _B, _grid, dt, timestep, _rng );
        _fbc_axis.setJ(_J);
        if (stamp) {
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }

        // lgr::file % "particle BC" << std::endl;

        if (stamp) {
          lgr::file % "MigrateParticles" << "==>>" << std::endl;
          stamp.emplace();
        }
        migrate_particles( timestep );
        if (stamp) {
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }

        // ----- After this line, particles are all within borders.
        // NOTE injector is implemented as a local operation
        _injector( timestep, dt, _rng, pic::wdt_pic, ens, _grid, _E, _B, _J, _particles );

        // ----- Before this line _J is local on each cpu --- //
        if ( stamp ) {
          lgr::file % "ReduceJmesh" << "==>>" << std::endl;
          stamp.emplace();
        }
        // TODOL reduce number of communications?
        for ( int i = 0; i < 3; ++i )
          ens.reduce_to_chief( mpi::by::SUM, _J[i].data().data(), _J[i].data().size() );
        if ( stamp ) {
          lgr::file % "\tLapse " << stamp->lapse().in_units_of("ms") << std::endl;
        }

        if ( _cart_opt ) {
          if (stamp) {
            lgr::file % "FieldUpdate" << "==>>" << std::endl;
            stamp.emplace();
          }
          field::merge_sync_guard_cells( _J, *_cart_opt );
          (*_field_update)(_E, _B, _J, dt, timestep);
          _fbc_axis.setEB(_E,_B);
          if (stamp) {
            lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
          }

        }
      }


      // TODO NOTE injection and annihilation are more like free sources and sinks of particles users control, in other words they fit the notion of particle boundary conditions. Do them outside of Updater. In contrast, pair creation is a physical process we model, so it is done inside the updater.
      // inject_particles(); // TODO only coordinate space current is needed in implementing current regulated injection // TODO compatible with ensemble??
      // TODOL annihilation will affect deposition // NOTE one can deposit in the end
      // annihilate_mark_pairs( );

      if ( is_do(pic::export_data_mr, timestep) && _ens_opt ) {
        if (stamp) {
          lgr::file % "ExportData" << "==>>" << std::endl;
          stamp.emplace();
        }
        io::export_data<pic::real_export_t, pic::Metric, pic::ShapeF >( this_run_dir, timestep, dt, pic::pmpio_num_files, pic::downsample_ratio, _cart_opt, 4.0 * std::acos(-1.0l) * classic_electron_radius() / pic::w_gyro_unitB ,  *_ens_opt, _grid, _E, _B, _J, _particles  );
        if (stamp) {
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }
      }

      if ( is_do(pic::dlb_mr, timestep) ) {
        if (stamp) {
          lgr::file % "DynamicLoadBalance" << "==>>" << std::endl;
          stamp.emplace();
        }
        if ( _ens_opt ) { // first reduce N_scat to avoid data loss
          const auto& ens = *_ens_opt;
          ens.reduce_to_chief( mpi::by::SUM, particle::N_scat.data().data(), particle::N_scat.data().size() );
          if ( !ens.is_chief() ) {
            for ( auto& x : particle::N_scat.data() ) x = 0;
          }
        }
        // TODO has a few hyper parameters
        // TODO touch create is not multinode safe even buffer is used
        std::optional<int> old_label;
        if ( _ens_opt ) old_label.emplace(_ens_opt->label());

        dynamic_load_balance( _particles, _ens_opt, _cart_opt, pic::dlb_target_load );

        std::optional<int> new_label;
        if ( _ens_opt ) new_label.emplace(_ens_opt->label());
        if (stamp) lgr::file % "  update parts of simulator..." << std::endl;
        if ( old_label != new_label ) update_parts(*_ens_opt);
        if (stamp) {
          lgr::file % "  Done." << std::endl;
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }
      }

      if (_ens_opt && is_do(pic::stats_mr, timestep) ) {
        if (stamp) {
          lgr::file % "Statistics" << std::endl;
          stamp.emplace();
        }
        particle::statistics( pic::this_run_dir + "/logs/statistics.txt", timestep, *_ens_opt, _cart_opt, _particles, particle::N_scat );
        if (stamp) {
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }
      }

      static ckpt::Autosave autosave; // significant only on mpi::world.rank() == 0
      if ( is_do(pic::checkpoint_mr, timestep)
           || ( pic::checkpoint_autosave_hourly &&
                autosave.is_save({*pic::checkpoint_autosave_hourly * 3600, "s"}) ) ) {
        if (stamp) {
          lgr::file % "SaveCheckpoint" << "==>>" << std::endl;
          stamp.emplace();
        }
        if ( _ens_opt ) { // first reduce N_scat to avoid data loss
          const auto& ens = *_ens_opt;
          ens.reduce_to_chief( mpi::by::SUM, particle::N_scat.data().data(), particle::N_scat.data().size() );
          if ( !ens.is_chief() ) {
            for ( auto& x : particle::N_scat.data() ) x = 0;
          }
        }
        auto dir = ckpt::save_checkpoint( this_run_dir, num_checkpoint_parts, _ens_opt, timestep, _E, _B, _particles, particle::N_scat );
        if ( mpi::world.rank() == 0 ) {
          auto obsolete_ckpt = autosave.add_checkpoint(dir, pic::max_num_ckpts);
          if ( obsolete_ckpt ) fs::remove_all(*obsolete_ckpt);
        }

        if (stamp) {
          lgr::file % "\tLapse = " << stamp->lapse().in_units_of("ms") << std::endl;
        }
      }

      // TODOL
      // if (false)
      //   save_tracing();
      if ( stamp_all ) {
        lgr::file.indent_reset();
        lgr::file << "  Total Lapse = " << stamp_all->lapse().in_units_of("ms") << std::endl;
      }
    }
  };
}

#endif
