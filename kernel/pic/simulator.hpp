#ifndef  _PIC_SIMULATOR_HPP_
#define  _PIC_SIMULATOR_HPP_

#include "particle/updater.hpp"
#include "field/sync.hpp"
#include "field/yee.hpp"

#include "particle/sorter.hpp"
#include "io/io.hpp"
#include "dye/dynamic_balance.hpp"
#include "ckpt/checkpoint.hpp"
#include "ckpt/autosave.hpp"

#include <memory>

#include "pic_impl.hpp"

#include "logger/logger.hpp"
#include "timer/timer.hpp"
#include "filesys/filesys.hpp"

#include "pic/vitals.hpp"

#ifdef PIC_DEBUG
#include "debug/debugger.hpp"
#endif

#include <unordered_map>

namespace pic {
  std::string this_run_dir;

  template < int DGrid,
             typename R,
             template < typename > class S,
             typename ShapeF,
             typename RJ >
  struct Simulator {
  private:
    const apt::Grid< R, DGrid >& _supergrid;
    std::optional<mpi::CartComm> _cart_opt;
    util::Rng<R> _rng;

    apt::Grid< R, DGrid > _grid;
    std::optional<dye::Ensemble<DGrid>> _ens_opt;

    particle::map<particle::Properties> _properties;
    field::Field<R, 3, DGrid> _E;
    field::Field<R, 3, DGrid> _B;
    field::Field<RJ, 3, DGrid> _J;
    particle::map<particle::array<R, S>> _particles;

    std::vector<std::unique_ptr<field::Action<R,DGrid,RJ>>> _field_actions;
    std::vector<std::unique_ptr<particle::Action<DGrid,R,S,RJ>>> _ptc_actions;

    std::vector<particle::Particle<R, S>> _ptc_buffer;

    void taylor( apt::ActionBase<DGrid>& a ) {
      // NOTE range is assumed to be noempty [,)
      auto f =
        [] ( int Ib_global, int Ie_global,
             const apt::Grid1D<R>& supergrid,
             const apt::Grid1D<R>& localgrid,
             bool is_periodic
             ) noexcept -> apt::pair<int>
        {
         // shift is the index of local lower in the global grid
         int shift = static_cast<int>( ( localgrid.lower() - supergrid.lower() ) / localgrid.delta() + 0.5 );
         Ib_global -= shift;
         Ie_global -= shift;
         // now Ib_ and Ie_global are with respect to the current local grid

         // Extend to infinity on true boundaries to
         // NOTE there may be actions done completely in the guard cells, such as assigining values
         int lb_local = 0;
         int ub_local = localgrid.dim();
         if ( !is_periodic ) {
           if ( std::abs( localgrid.lower() - supergrid.lower() ) < supergrid.delta() )
             lb_local = std::numeric_limits<int>::min();
           if ( std::abs( localgrid.upper() - supergrid.upper() ) < supergrid.delta() )
             ub_local = std::numeric_limits<int>::max();
         }

         if ( Ie_global <= lb_local || Ib_global >= ub_local ) {
           // range not applicable on current local patch
           return {0,0};
         } else {
           Ib_global = std::max<int>( lb_local, Ib_global );
           Ie_global = std::min<int>( Ie_global, ub_local );
           return { Ib_global, Ie_global };
         }
        };

      for ( int i = 0; i < DGrid; ++i ) {
        auto [b_new, e_new] = f( a[i].begin(), a[i].end(), _supergrid[i], _grid[i], pic::periodic[i] );
        a[i].begin() = b_new;
        a[i].end() = e_new;
      }
    }

    void update_parts( const dye::Ensemble<DGrid>& ens ) {
      for ( int i = 0; i < DGrid; ++i ) {
        _grid[i] = _supergrid[i].divide( ens.cart_topos[i].dim(), ens.cart_coords[i] );
      }

      { // set field actions
        // auto f_actions = field::set_up_field_actions<DGrid,R,RJ>();
        auto f_actions = set_up_field_actions();
        _field_actions.resize(f_actions.size());
        for ( int i = 0; i < f_actions.size(); ++i ) {
          _field_actions[i].reset(f_actions[i]->Clone());
          taylor(*_field_actions[i]);
        }
      }

      { // set particle actions
        // auto p_actions = particle::set_up_particle_actions<DGrid,R,S,ShapeF,RJ>();
        auto p_actions = set_up_particle_actions();
        _ptc_actions.resize(p_actions.size());
        for ( int i = 0; i < p_actions.size(); ++i ) {
          _ptc_actions[i].reset(p_actions[i]->Clone());
          taylor(*_ptc_actions[i]);
        }
      }

      { // init runtime data
        RTD::data().init( _properties, _grid );
      }

      if ( pic::msperf_qualified(_ens_opt) ) lgr::file.open(std::ios_base::app);
    }

  public:
    Simulator( const apt::Grid< R, DGrid >& supergrid, const std::optional<mpi::CartComm>& cart_opt, const particle::map<particle::Properties>& props )
      : _supergrid(supergrid), _cart_opt(cart_opt), _properties(props) {
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
        // FIXME : should include upper boundary ???
        auto range = apt::make_range({}, bulk_dims, myguard);
        _E = {range};
        _B = {range};
        _J = {range};

        for( int i = 0; i < 3; ++i ) {
          _E.set_offset( i, field::yee::ofs_gen<DGrid>( field::yee::Etype, i ) );
          _B.set_offset( i, field::yee::ofs_gen<DGrid>( field::yee::Btype, i ) );
          _J.set_offset( i, field::yee::ofs_gen<DGrid>( field::yee::Etype, i ) );
        }
        _E.reset();
        _B.reset();
        _J.reset();
      }
      // NOTE all species in the game should be created regardless of whether they appear on certain processes. This is to make the following work
      // 1. detailed balance. Absence of some species may lead to deadlock to transferring particles of that species.
      // 2. data export. PutMultivar requires every patch to output every species
      for ( auto sp : _properties )
        _particles.insert( sp, particle::array<R, S>() );

      if ( _ens_opt ) update_parts(*_ens_opt);
    }

    int load_initial_condition( std::optional<std::string> checkpoint_dir ) {
      int init_ts = pic::initial_timestep;
      if ( checkpoint_dir ) {
        init_ts = ckpt::load_checkpoint( *checkpoint_dir, _ens_opt, _cart_opt, _E, _B, _particles, _properties, RTD::data().N_scat );
        ++init_ts; // checkpoint is saved at the end of a timestep
        if ( _ens_opt ) update_parts(*_ens_opt);
      } else {
        // FIXME a temporary fix, which may crash under the edge case in which initially many particles are created.
        if (_cart_opt) {
          auto ic = set_up_initial_conditions();
          taylor(ic);
          ic(_grid, _E, _B, _J, _particles);
          field::copy_sync_guard_cells(_E, *_cart_opt);
          field::copy_sync_guard_cells(_B, *_cart_opt);
          field::copy_sync_guard_cells(_J, *_cart_opt);
        }
      }
      // broadcast to ensure uniformity
      mpi::world.broadcast( 0, &init_ts, 1 );
      // set up vitals
      if ( mpi::world.rank() == 0 ) {
        vital::t_phys_prev = ( init_ts - 1 ) * pic::dt; // NOTE the -1
        for ( auto sp : _particles ) {
          double num = 0.0;
          for ( const auto& ptc : _particles[sp] ) {
            if ( ptc.is(particle::flag::exist) ) num += ptc.frac();
          }
          vital::num_ptcs_prev.push_back(num);
          vital::num_scat_prev.push_back(RTD::data().N_scat[sp]);
        }
      }
      // initialize trace_counters to ensure unique trace serial numbers across runs
      Tracer::init(_properties, _particles);

      return init_ts;
    }

    inline void set_rng_seed( int seed ) { _rng.set_seed(seed); }

    // NOTE we choose to do particle update before field update so that in saving snapshots we don't need to save _J
    void evolve( int timestep, R dt ) {
      if ( timestep % pic::cout_ts_interval == 0 && mpi::world.rank() == 0 )
        std::cout << "==== Timestep " << timestep << " ====" << std::endl;

#ifdef PIC_DEBUG
      debug::timestep = timestep;
      debug::world_rank = mpi::world.rank();
      if ( _ens_opt ) debug::ens_label = _ens_opt -> label();
#endif

      std::optional<tmr::Timestamp> stamp_all;
      std::optional<tmr::Timestamp> stamp;
      if ( pic::msperf_mr.is_do(timestep) && pic::msperf_qualified(_ens_opt) ) {
        stamp_all.emplace();
        stamp.emplace();
        if ( msperf_max_entries &&
             ( timestep - msperf_mr.init_ts > *msperf_max_entries * msperf_mr.interval )  ) lgr::file.clear();
        lgr::file << "==== Timestep " << timestep << " ====" << std::endl;
        lgr::file.indent_append("\t");
      }

      auto TIMING = [&stamp](std::string message, auto f) {
                      if ( stamp ) {
                        lgr::file % message << "==>>" << std::endl;
                        stamp.emplace();
                      }
                      f();
                      if ( stamp ) {
                        lgr::file % "\tLapse " << stamp->lapse().in_units_of("ms") << std::endl;
                      }
                    };
#define START [&]()

      if ( _ens_opt ) {
        const auto& ens = *_ens_opt;

        TIMING("BroadcastEB", START {
            // TODOL reduce number of communications?
            for ( int i = 0; i < 3; ++i )
              ens.intra.broadcast( ens.chief, _E[i].data().data(), _E[i].data().size() );
            for ( int i = 0; i < 3; ++i )
              ens.intra.broadcast( ens.chief, _B[i].data().data(), _B[i].data().size() );
          });

        _J.reset();

        if ( pic::sort_particles_mr.is_do(timestep) ) {
          TIMING("SortParticles", START {
              for ( auto sp : _particles ) particle::sort( _particles[sp] );
            });
        }

        if ( stamp && _particles.size() != 0 ) {
          lgr::file % "ParticleCounts:" << std::endl;
          for ( auto sp : _particles )
            lgr::file % "\t" << _properties[sp].name << " = " << _particles[sp].size() << std::endl;
        }

        // ----- before this line particles are all within borders --- //
        TIMING("ParticleActions", START {
            for ( int i = 0; i < _ptc_actions.size(); ++i ) {
              if ( !_ptc_actions[i] ) continue;
              if ( stamp ) {
                lgr::file % "--" << _ptc_actions[i]->name() << std::endl;
              }
              auto& act = *_ptc_actions[i];
              act( _particles, _J, &_ptc_buffer, _properties, _E, _B, _grid, &ens, dt, timestep, _rng );
            }
          });

        // ----- Before this line _J is local on each cpu --- //
        TIMING("ReduceJmesh", START {
            // TODOL reduce number of communications?
            for ( int i = 0; i < 3; ++i )
              ens.reduce_to_chief( mpi::by::SUM, _J[i].data().data(), _J[i].data().size() );
          });

        if ( _cart_opt ) {
          TIMING("FieldUpdate", START {
              field::merge_sync_guard_cells( _J, *_cart_opt );

              std::unordered_map<int,field::Field<R, 3, DGrid>> E_old_patch;
              std::unordered_map<int,field::Field<R, 3, DGrid>> B_old_patch;
              int i_max = -1; // action index with the largest range
              {
                int full_size_max = 0;
                for ( int i = 0; i < _field_actions.size(); ++i ) {
                  if ( !_field_actions[i] or !_field_actions[i] -> require_original_EB() ) continue;
                  const auto& act = *_field_actions[i];
                  int full_size = 1;
                  for ( int j = 0; j < act.size(); ++j )
                    full_size *= act[j].full_size();

                  int i_to_be_cached = 0;
                  if ( full_size > full_size_max ) {
                    i_to_be_cached = i_max;
                    i_max = i;
                    full_size_max = full_size;
                  } else
                    i_to_be_cached = i;
                  if ( -1 == i_to_be_cached  ) continue;

                  const auto& a = *_field_actions[i_to_be_cached];
                  E_old_patch.insert( {i_to_be_cached, {a}} );
                  B_old_patch.insert( {i_to_be_cached, {a}} );
                  for ( int C = 0; C < 3; ++ C ) {
                    for ( const auto& I : apt::Block(apt::range::far_begin(a), apt::range::far_end(a)) ) {
                      E_old_patch.at(i_to_be_cached)[C](I) = _E[C](I);
                      B_old_patch.at(i_to_be_cached)[C](I) = _B[C](I);
                    }
                  }
                }
              }

              // FIXME: i_max must go first in this category
              if ( -1 != i_max ) {
                if ( stamp ) {
                  lgr::file % "--" << _field_actions[i_max]->name() << std::endl;
                }
                (*_field_actions[i_max])(_E, _B, _J, _grid, *_cart_opt, timestep, dt);
              }

              for ( int i = 0; i < _field_actions.size(); ++i ) {
                if ( !_field_actions[i] or i_max == i ) continue; // FIXME NOTE the i_max
                if ( stamp ) {
                  lgr::file % "--" << _field_actions[i]->name() << std::endl;
                }
                const auto& act = *_field_actions[i];

                if ( E_old_patch.find(i) != E_old_patch.end() ) {
                  auto& E_old = E_old_patch.at(i);
                  auto& B_old = B_old_patch.at(i);
                  act(E_old, B_old, _J, _grid, *_cart_opt, timestep, dt);
                  for ( const auto& I : apt::Block(apt::range::begin(act), apt::range::end(act)) ) {
                    for ( int C = 0; C < 3; ++C ) {
                      _E[C](I) = E_old[C](I);
                      _B[C](I) = B_old[C](I);
                    }
                  }

                } else
                  act(_E, _B, _J, _grid, *_cart_opt, timestep, dt);
              }
            });


          // NOTE sub_range same as domain_range FIXME rethink domain and sub range
          copy_sync_guard_cells(_E, *_cart_opt );
          copy_sync_guard_cells(_B, *_cart_opt );
        }
      }

      if ( pic::export_data_mr.is_do(timestep) && _ens_opt ) {
        TIMING("ExportData", START {
            export_prior_hook( _particles, _properties, _E, _B, _J, _grid, *_ens_opt, dt, timestep );

            auto fexps = set_up_field_export();
            auto pexps = set_up_particle_export();

            io::set_is_collinear_mesh(is_collinear_mesh); // TODO

            io::export_data<pic::real_export_t>( this_run_dir, timestep, dt, pic::pmpio_num_files, pic::downsample_ratio, _cart_opt, *_ens_opt, _grid, _E, _B, _J, _particles, _properties, fexps, pexps );
            for ( auto ptr : fexps ) delete ptr;
            for ( auto ptr : pexps ) delete ptr;

            export_post_hook();
          });
      }

      auto& Ns = RTD::data().N_scat;
      if ( pic::dlb_mr.is_do(timestep) ) {
        TIMING("DynamicLoadBalance", START {
            if ( _ens_opt ) { // first reduce N_scat to avoid data loss
              const auto& ens = *_ens_opt;
              ens.reduce_to_chief( mpi::by::SUM, Ns.data().data(), Ns.data().size() );
              if ( !ens.is_chief() ) {
                for ( auto& x : Ns.data() ) x = 0;
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
            if (stamp) lgr::file % "  Done." << std::endl;
          });
      }

      if (_ens_opt && pic::vitals_mr.is_do(timestep) ) {
        TIMING("Statistics", START {
            pic::check_vitals( pic::this_run_dir + "/vitals.txt", timestep * dt, *_ens_opt, _cart_opt, _properties, _particles, Ns );
          });
      }

      static ckpt::Autosave autosave; // significant only on mpi::world.rank() == 0
      if ( pic::checkpoint_mr.is_do(timestep)
           || ( pic::checkpoint_autosave_hourly &&
                autosave.is_save({*pic::checkpoint_autosave_hourly * 3600, "s"}) ) ) {
        TIMING("SaveCheckpoint", START {
            if ( _ens_opt ) { // first reduce N_scat to avoid data loss
              const auto& ens = *_ens_opt;
              ens.reduce_to_chief( mpi::by::SUM, Ns.data().data(), Ns.data().size() );
              if ( !ens.is_chief() ) {
                for ( auto& x : Ns.data() ) x = 0;
              }
            }
            auto dir = ckpt::save_checkpoint( this_run_dir, num_checkpoint_parts, _ens_opt, timestep, _E, _B, _particles, _properties, Ns );
            if ( mpi::world.rank() == 0 ) {
              auto obsolete_ckpt = autosave.add_checkpoint(dir, pic::max_num_ckpts);
              if ( obsolete_ckpt ) fs::remove_all(*obsolete_ckpt);
            }
          });
      }

      if ( pic::tracing_mr.is_do(timestep) ) {
        TIMING("SaveTracing", START {
            auto dir = ckpt::save_tracing( this_run_dir, num_tracing_parts, _ens_opt, timestep, _particles );
          });
      }

      if ( stamp_all ) {
        lgr::file.indent_reset();
        lgr::file << "  Total Lapse = " << stamp_all->lapse().in_units_of("ms") << std::endl;
      }
    }
  };
}

#endif
