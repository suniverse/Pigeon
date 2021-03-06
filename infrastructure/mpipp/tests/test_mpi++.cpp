#include "testfw/testfw.hpp"
#include "mpipp/mpi++.hpp"

using namespace mpi;

SCENARIO("World", "[parallel][mpi]") {
  if ( world.size() >= 2 ) {
    int myrank = world.rank();
    // std::cout << "world size = " << world.size() << " ," << myrank << std::endl;
    if ( 0 == myrank ) {
      int msg = 147;
      world.send( 1, 22, &msg );
    } else if ( 1 == myrank) {
      int msg;
      world.recv( 0, 22, &msg, 1 );
      REQUIRE( msg == 147 );
    }
  }
}

// SCENARIO("Group", "[parallel][mpi]") {
//   auto grp_world = world.group();
//   SECTION("Constructors") {
//     WHEN("Default") {
//       Group grp;
//       REQUIRE( MPI_UNDEFINED == grp.rank() );
//       REQUIRE( 0 == grp.size() );
//     }

//     WHEN("From MPI_WORLD_GROUP") {
//       AND_WHEN("no duplicates") {
//       }
//       AND_WHEN("there are duplicates") {
//       }
//     }
//   }

//   SECTION("Operations") {

//   }

// }

SCENARIO("Comm", "[parallel][mpi]") {
  auto comm_opt = world.split( true );
  REQUIRE( comm_opt );
  auto& comm = *comm_opt;
  if ( world.size() >= 2 ) {
    int myrank = comm.rank();
    if ( 0 == myrank ) {
      int msg = 147;
      comm.send( 1, 22, &msg );
    } else if ( 1 == myrank ) {
      int msg;
      comm.recv( 0, 22, &msg, 1 );
      REQUIRE( 147 == msg );
    }
  }
}

// TODOL restore after typelist is installed
// SCENARIO("Datatype", "[parallel][mpi]") {
// #define TestType(_T_, _MPIT_)                        \
//   REQUIRE( datatype((_T_*)0) == MPI_##_MPIT_ );      \
//   REQUIRE( datatype((const _T_*)0) == MPI_##_MPIT_); \
//   REQUIRE( datatype((_T_)0) == MPI_##_MPIT_);        \

//   TestType(int, INT);
//   TestType(char, CHAR);
//   TestType(short, SHORT);
//   TestType(long, LONG);

//   TestType(unsigned char, UNSIGNED_CHAR);
//   TestType(unsigned short, UNSIGNED_SHORT);
//   TestType(unsigned int, UNSIGNED);
//   TestType(unsigned long, UNSIGNED_LONG);

//   TestType(float, FLOAT);
//   TestType(double, DOUBLE);
//   TestType(bool, CXX_BOOL);
// #undef TestType
// }

SCENARIO("intra P2P communications", "[parallel][mpi]") {
  SECTION("Nonblocking") {
    if ( world.size() >= 2 ) {

      SECTION("one sided communication, test on wait") {
        if ( world.rank() == 0 ) {
          int send_msg = 351;
          Request req = world.Isend( 1, 147, &send_msg, 1 );
          mpi::wait(req);
        } else if ( world.rank() == 1 ) {
          int recv_msg;
          Request req = world.Irecv( 0, 147, &recv_msg, 1 );
          mpi::wait(req);
          REQUIRE( recv_msg == 351 );
        }
        world.barrier();
      }

      SECTION("double sided communication, test on waitall") {
        if ( world.rank() < 2 ) {
          std::vector<Request> reqs(2);
          int myrank = world.rank();
          int send_msg = myrank + 147;
          reqs[0] = world.Isend( 1 - myrank, 147, &send_msg, 1 );
          int recv_msg = -1;
          reqs[1] = world.Irecv( 1 - myrank, 147, &recv_msg, 1 );
          waitall(reqs);
          REQUIRE( send_msg == myrank + 147 );
          REQUIRE( recv_msg == 1 - myrank + 147 );
        }
        world.barrier();
      }

      SECTION("test wait on null requests") {
        if ( world.rank() == 0  ) {
          Request req;
          mpi::wait(req);

          std::vector<Request> reqs(2);
          mpi::waitall(reqs);
        }
      }
    }
  }
}

// SCENARIO("intra collective communications", "[parallel][mpi]") {}

SCENARIO("intercomm P2P communications", "[parallel][mpi]") {
  SECTION("each comm has one member") {
    if ( world.size() >= 2 && world.rank() < 2 ) {
      int myrank = world.rank();
      InterComm inter( self, 0, {world}, 1-myrank, 147 );
      if ( 0 == myrank ) {
        int msg = 147;
        inter.send( 0, 22, &msg, 1 );
      } else if ( 1 == myrank ) {
        int msg;
        inter.recv( 0, 22, &msg, 1 );
        REQUIRE(msg == 147);
      }
    }
    world.barrier();
  }

}

SCENARIO("cartesian communicator", "[parallel][mpi]") {
  WHEN("1x1 and periodic") {
    CartComm cart( self, {1,1}, {true, true} );
    for ( int i = 0; i < 2; ++i ) {
      auto[src_rank, dest_rank] = cart.shift(i,1);
      REQUIRE(src_rank);
      REQUIRE(dest_rank);
      REQUIRE(*src_rank == 0 );
      REQUIRE(*dest_rank == 0 );
    }
  }
}
