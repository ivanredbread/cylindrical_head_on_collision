#ifdef RICH_MPI
#include "source/mpi/MeshPointsMPI.hpp"
#endif
#include <iostream>
#include <fstream>
#include <cmath>
#include <vector>
#include <string>
#include "source/tessellation/geometry.hpp"
#include "source/newtonian/two_dimensional/hdsim2d.hpp"
#include "source/tessellation/tessellation.hpp"
#include "source/newtonian/common/hllc.hpp"
#include "source/newtonian/common/ideal_gas.hpp"
#include "source/tessellation/VoronoiMesh.hpp"
#include "source/newtonian/two_dimensional/spatial_distributions/uniform2d.hpp"
#include "source/newtonian/two_dimensional/point_motions/eulerian.hpp"
#include "source/newtonian/two_dimensional/point_motions/lagrangian.hpp"
#include "source/newtonian/two_dimensional/point_motions/round_cells.hpp"
#include "source/newtonian/two_dimensional/source_terms/zero_force.hpp"
#include "source/newtonian/two_dimensional/geometric_outer_boundaries/SquareBox.hpp"
#include "source/newtonian/test_2d/random_pert.hpp"
#include "source/newtonian/two_dimensional/diagnostics.hpp"
#include "source/misc/simple_io.hpp"
#include "source/misc/mesh_generator.hpp"
#include "source/newtonian/test_2d/main_loop_2d.hpp"
#include "source/newtonian/two_dimensional/hdf5_diagnostics.hpp"
#include "source/tessellation/shape_2d.hpp"
#include "source/newtonian/test_2d/piecewise.hpp"
#include "source/newtonian/two_dimensional/simple_flux_calculator.hpp"
#include "source/newtonian/two_dimensional/simple_cell_updater.hpp"
#include "source/newtonian/two_dimensional/simple_extensive_updater.hpp"
#include "source/newtonian/two_dimensional/stationary_box.hpp"
#include "source/tessellation/right_rectangle.hpp"
#include "source/newtonian/test_2d/clip_grid.hpp"
#include "source/newtonian/test_2d/multiple_diagnostics.hpp"
#include "source/misc/vector_initialiser.hpp"
#include "source/newtonian/test_2d/consecutive_snapshots.hpp"

using namespace std;
using namespace simulation2d;

namespace {

  vector<Vector2D> centered_hexagonal_grid(double r_min,
					   double r_max)
  {
    const vector<double> r_list = arange(0,r_max,r_min);
    vector<Vector2D> res;
    for(size_t i=0;i<r_list.size();++i){
      const size_t angle_num = max<size_t>(6*i,1);
      vector<double> angle_list(angle_num,0);
      for(size_t j=0;j<angle_num;++j)
	angle_list.at(j) = 2*M_PI*static_cast<double>(j)/static_cast<double>(angle_num);
      for(size_t j=0;j<angle_num;++j)
	res.push_back(r_list.at(i)*Vector2D(cos(angle_list.at(j)),
					    sin(angle_list.at(j))));
    }
    return res;
  }

  vector<Vector2D> centered_logarithmic_spiral(double r_min,
					       double r_max,
					       double alpha,
					       const Vector2D& center)
  {
    const double theta_max = log(r_max/r_min)/alpha;
    const vector<double> theta_list = 
      arange(0,theta_max,2*M_PI*alpha/(1-0.5*alpha));
  
    vector<double> r_list(theta_list.size(),0);
    for(size_t i=0;i<r_list.size();++i)
      r_list.at(i) = r_min*exp(alpha*theta_list.at(i));
  
    vector<Vector2D> res(r_list.size());
    for(size_t i=0;i<res.size();++i)
      res[i] = center+r_list[i]*Vector2D(cos(theta_list.at(i)),
					 sin(theta_list.at(i)));
    return res;
  }

  vector<Vector2D> complete_grid(double r_inner,
				 double r_outer,
				 double alpha)
  {
    const vector<Vector2D> inner = 
      centered_hexagonal_grid(r_inner*alpha*2*M_PI,
			      r_inner);
    const vector<Vector2D> outer =
      centered_logarithmic_spiral(r_inner,
				  r_outer,
				  alpha,
				  Vector2D(0,0));
    return join(inner, outer);
  }

#ifdef RICH_MPI

  vector<Vector2D> process_positions(const SquareBox& boundary)
  {
    const Vector2D lower_left = boundary.getBoundary().first;
    const Vector2D upper_right = boundary.getBoundary().second;
	int ws=0;
	MPI_Comm_size(MPI_COMM_WORLD,&ws);
    return RandSquare(ws,lower_left.x,upper_right.x,lower_left.y,upper_right.y);
  }

#endif

  vector<ComputationalCell> calc_init_cond(const Tessellation& tess)
  {
    const double radius = 0.04;
    const double sep = 1.01*radius;
    const double min_density = 1e-9;
    vector<ComputationalCell> res(static_cast<size_t>(tess.GetPointNo()));
    for(size_t i=0;i<res.size();++i){
      res.at(i).density = min_density;
      res.at(i).pressure = 1e-7;
      res.at(i).velocity = Vector2D(0,0);
      const Vector2D& r = tess.GetMeshPoint(static_cast<int>(i));
      if(abs(r-Vector2D(0,sep))<radius){
	const double x = radius - abs(r-Vector2D(0,sep));
	res.at(i).density = max(min_density,10*pow(x/radius,1.5));
	res.at(i).velocity = Vector2D(0,-1);
      }
      if(abs(r-Vector2D(0,-sep))<radius){
	const double x = radius - abs(r-Vector2D(0,-sep));
	res.at(i).density = max(min_density,10*pow(x/radius,1.5));
	res.at(i).velocity = Vector2D(0,1);
      }
    }
    return res;
  }

  class SimData
  {
  public:

    SimData(void):
      pg_(),
      width_(2),
      outer_(-width_,width_,width_,-width_),
#ifdef RICH_MPI
	  vproc_(process_positions(outer_),outer_),
		init_points_(SquareMeshM(50,50,vproc_,outer_.getBoundary().first,outer_.getBoundary().second)),
		tess_(vproc_,init_points_,outer_),
#else
      init_points_(clip_grid
		   (RightRectangle(Vector2D(-width_,-width_), Vector2D(width_, width_)),
		    complete_grid(0.1,
				  2*width_,
				  0.005))),
		tess_(init_points_, outer_),
#endif
      eos_(5./3.),
      bpm_(),
      point_motion_(bpm_,eos_),
      sb_(),
      rs_(),
      force_(),
      tsf_(0.3),
      fc_(rs_),
      eu_(),
      cu_(),
      sim_(
#ifdef RICH_MPI
		  vproc_,
#endif
		  tess_,
	   outer_,
	   pg_,
	   calc_init_cond(tess_),
	   eos_,
	   point_motion_,
	   sb_,
	   force_,
	   tsf_,
	   fc_,
	   eu_,
	   cu_) {}

    hdsim& getSim(void)
    {
      return sim_;
    }

  private:
    const SlabSymmetry pg_;
    const double width_;
    const SquareBox outer_;
#ifdef RICH_MPI
	VoronoiMesh vproc_;
#endif
    const vector<Vector2D> init_points_;
    VoronoiMesh tess_;
    const IdealGas eos_;
#ifdef RICH_MPI
    //Eulerian point_motion_;
    //	Lagrangian point_motion_;
#else
    //Eulerian point_motion_;
    Lagrangian bpm_;
    RoundCells point_motion_;
    //Lagrangian point_motion_;
#endif
    const StationaryBox sb_;
    const Hllc rs_;
    ZeroForce force_;
    const SimpleCFL tsf_;
    const SimpleFluxCalculator fc_;
    const SimpleExtensiveUpdater eu_;
    const SimpleCellUpdater cu_;
    hdsim sim_;
  };

  class WriteCycle: public DiagnosticFunction
  {
  public:

    WriteCycle(const string& fname):
      fname_(fname) {}

    void operator()(const hdsim& sim)
    {
      write_number(sim.getCycle(),fname_);
    }

  private:
    const string fname_;
  };
}

int main(void)
{
#ifdef RICH_MPI
	MPI_Init(NULL,NULL);
	int rank=0;
	MPI_Comm_rank(MPI_COMM_WORLD,&rank);
#endif
  SimData sim_data;
  hdsim& sim = sim_data.getSim();

  const double tf = 5e-1;
  SafeTimeTermination term_cond(tf,1e6);
  MultipleDiagnostics diag
  (VectorInitialiser<DiagnosticFunction*>()
   (new ConsecutiveSnapshots(new ConstantTimeInterval(tf/200),
			     new Rubric("output/snapshot_",".h5")))
   (new WriteTime("time.txt"))
   (new WriteCycle("cycle.txt"))());
  write_snapshot_to_hdf5(sim, "output/initial.h5");
  main_loop(sim,
	    term_cond,
	    &hdsim::TimeAdvance,
	    &diag);
	    

#ifdef RICH_MPI
  write_snapshot_to_hdf5(sim, "process_"+int2str(rank)+"_final"+".h5");
  MPI_Finalize();
#else
  write_snapshot_to_hdf5(sim, "output/final.h5");
#endif


  return 0;
}

