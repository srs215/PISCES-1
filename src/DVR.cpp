//TS
//
//  This was originally a DVR code I wrote, in C, for vibrational problems
//  and that would therefore work with arbitrary dimensions.
//
//  Here it is used for the excess electron, the number of dimensions is 3,
//  and many parts work, or make even sense, only in 3D.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>


#ifdef _OPENMP
#include <omp.h>
#endif

#include "timer.hpp"
#include "constants.h"
#include "cm_dvr.h"
#include "ho_dvr.h"
#include "sine_dvr.h"
#include "fulldiag.h"
#include "larnoldi.h"
#include "davdriver.h"
#include "writewfcuts.h"
#include "Small2Large.h"
#include "ReadCubeFile.h"
#include "lapackblas.h"


#include "Potential.h"
#include "DVR.h"
#include "KE_diag.h"

using namespace std;

// get random number in [0,1]
double Rand01(void);
double Rand01(void)
{
  return double(rand()) / RAND_MAX;
}

// get random number in [-1,1]
double Randm11(void);
double Randm11(void)
{
  if (rand() > RAND_MAX/2)
    return double(rand()) / RAND_MAX;
  else
    return -double(rand()) / RAND_MAX;
}


inline int sub2ind( int i, int j, int k, const int n[] );

void DVR::SetupDVR(const int *npts, int type, int Sampling, const double *para, int gridverbose)
{
  verbose = gridverbose;
  dvrtype = type;             // type 1: HO,   type 2: Sine
  sampling = Sampling;        // sampling of potential (single, double, or triple density)
  
  n_1dbas[0] = npts[0];     // number of grid points along x, y, and z
  n_1dbas[1] = npts[1];
  n_1dbas[2] = npts[2];
  
  gridpara[0] = para[0];      // this is the frequency omega (type 1)
  gridpara[1] = para[1];      // or the length of the grid (type 2)
  gridpara[2] = para[2];
  
  ComputeGridParameters();  // finds max1d, ngp, and strides for v_diag



  Vec_x_dvr.resize(max1db * no_dim);
  x_dvr = &Vec_x_dvr[0];
  Vec_v_diag.resize(ngp);
  v_diag = &Vec_v_diag[0];

  //TV: Set up FFT parameters if DVRtype =3
  if(dvrtype == 3){
     FFTSetup();
     cout << "Using FFT for Hamiltonian \n"; cout.flush();
  }

  for (int idim = 0; idim < no_dim; ++idim) {
    int npts = n_1dbas[idim];
    //TV: For FFT we need only diagonal elements of KE
    if(dvrtype == 3)
        e_kin[idim] = new double[npts];
    else
    e_kin[idim] = new double[npts*npts];
    if (dvrtype == 1)
      dvr_rep[idim] = new double[npts * npts];
    else
      dvr_rep[idim] = 0;
  }
  ComputeGridPointsAndKineticEnergy();
  
  if (verbose > 20)
    {cout << "SetupDVR done.\n"; cout.flush();} 
}


DVR::~DVR()
{

  for (int idim = 0; idim < no_dim; ++idim) {
    if (e_kin[idim] >= 0) delete[] e_kin[idim];
    if (dvr_rep[idim] >= 0) delete[] dvr_rep[idim];
  }

}



void DVR::SetVerbose(int v)
{
   verbose = v;
}

void DVR::FFTSetup()
{
  //TV: Define the arrays for fourier transformation  
  int nrofpts = n_1dbas[0]*n_1dbas[1]*n_1dbas[2];

      KE_diag = new double[nrofpts];        
      phi_x       = new Complex[nrofpts];     
      phi_k       = new Complex[nrofpts]; 
      KE_phi_k    = new Complex[nrofpts];
      KE_phi_x    = new Complex[nrofpts];
  
   //Right now we have better scaling with FFTW3 compared to MKL FFT 
  //fftw3 openmp part
    int nthreads = 1;    
    nthreads = omp_get_max_threads();
    fftw_init_threads();
    fftw_plan_with_nthreads(nthreads);
    printf("FFT Using %u OpenMP threads.\n",omp_get_max_threads() );
    

    /*intel MKL fftw3 wrapper part
      int nthreads = 1;
      fftw_init_threads();            
      nthreads = mkl_get_max_threads();      
      fftw3_mkl.number_of_user_threads =nthreads;
      printf("FFT Using %u MKL threads.\n",  );*/

    //Define the forward and backward FFT plans
      plan_forward   = fftw_plan_dft(3,n_1dbas ,(fftw_complex*)phi_x,(fftw_complex*)phi_k, FFTW_FORWARD,  FFTW_ESTIMATE);
      plan_backward  = fftw_plan_dft(3,n_1dbas,(fftw_complex*)KE_phi_k,(fftw_complex*)KE_phi_x, FFTW_BACKWARD, FFTW_ESTIMATE);

}

void DVR::DiagonalizeSetup(int nEV, int DiagFlag, int nMaxSub, int nMaxIter, int pTol)
{
   nStates = nEV;
   diagFlag = DiagFlag;
   maxSub = nMaxSub;
   maxIter = nMaxIter;
   ptol = pTol;
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//  call a subspace iteration method to compute a few eigenpairs of the DVR of the Hamiltonian
//  the input parameters are for the Lanczos or the Davidson
//  nStates   no of requested eigenvalues (only 1 for the Davidson so far)
//  diagFlag 1=Arnoldi, 2=Davidson, 3=Jacobi-Davidson (zeroth-order)
//  maxSub  maximal subspace size
//  maxIter how many macro iterations (up to maxSub x maxIter matrix-times-vector operations)
//  SVFlag   start vector Flag
//  ptol     convergence tolerance is 10^(-ptol)
//
//  output: no of converged eigenvalues, energies are in ev
//
int DVR::Diagonalize(int SVFlag, double *ev)
{
   counter kc; m_pkc = &kc;
   progress_timer tmr("DVR::Diagonalize", verbose);

   if (verbose > 0)
      cout << "Computing the energy and wavefunction using a DVR of the Hamiltonian\n";

   //
   // with the current logic you cannot use an old wavefunction as a start vector
   // in a calculation aiming at more than one state, and that is a bad idea anyway
   // most applications should have nwavefn = 1 anyway,
   // if more than one state is requested that is probably a single point, right?
   //

   if (nwavefn < nStates) {
     if (nwavefn > 0) {
         nconverged = 0;
      }
      wavefn.resize(ngp*nStates);
      nwavefn = nStates;
   }

   //  The start vectors are the first nStates columns of wavefn
   //
   //  SVFlag = 0   use last converged vectors (plus random vectors if needed)
   //         = 1   Particle-in-a-box startvector + (n-1) random vectors
   //         = 2   all random start vectors
   //
   int istart = 0;
   switch (SVFlag)
   {
   case 0:
      if (nconverged < nStates) {
	cout << "DVR::Diagonalize: SVFlag=0, but there are only "<< nconverged << " old wavefunctions available\n"
	      << "using random start vectors for the rest\n";
      }
      if (verbose > 0) cout << "Using "<< nconverged << " start vectors from a previous diagonalization.\n";
      istart = nconverged;
      break;
   case 1:
     if (verbose > 0) cout << "Initializing one PiaB-like start vector.\n";
     ParticleInAnDBoxWf(&wavefn[0]);
     istart = 1;
     break;
   case 2:
     break; // leave istart = 0
   default:
     cout << "DVR::Diagonalize: illegal value of start vector flag = " << SVFlag << "\n";
     exit(1);
   }

   if (istart < nStates) {
     if (verbose > 0) cout << "Initializing " << nStates-istart << " random start vectors.\n";   
     for (int i = istart; i < nStates; ++i)
       for (int k = 0; k < ngp; ++k)
	 wavefn[i*ngp+k] = Randm11();
   }

   //  if (istart < nStates) {
   //    if (verbose > 0) cout << "vkv is Initializing " << nStates-istart << " start vectors with PiaB stupidly.\n";
   //    for (int i = istart; i < nStates; ++i)
   //        ParticleInAnDBoxWf(&wavefn[i*ngp]) ;
   //  }


   int CorrFlag = 0;  // correction scheme for "Davidson"
   switch(diagFlag)
     {
     case 2: CorrFlag = 1; break; // standard Davidson correction
     case 3: CorrFlag = 2; break; // Jacobi-Davidson correction
     case 4: CorrFlag = 0; break; // no correction: effective Lanczos-Arnoldi
     default: break;
       // do nothing
     }  

   switch (diagFlag)
   {
   case 0:
      if (verbose > 0)
	{printf("\nFull Diagonalization (only for debugging)\n"); cout.flush();}
      if (ngp > 1000)
	{cout << "Ndim = " << ngp << "\nIf you want more than 1000, recompile."; exit(42);}
      {double *hmat = new double[ngp*ngp];
      build_h(hmat);
      fulldiag(hmat);
      delete[] hmat;}
      break;
   case 1:
      if (verbose > 0)
         printf("\nLanczos Arnoldi:\n");
      nconverged = larnoldi(ngp, nStates, maxSub, maxIter, ptol, ev);
      break;
   case 2:
   case 3:
   case 4:
      if (verbose > 0)
         printf("\nDavidson:\n");
      nconverged = larnoldi(ngp, nStates, maxSub, maxIter, ptol, ev);
      nconverged = davdriver(ngp, nStates, maxSub, maxIter, ptol, CorrFlag, ev);
      break;
   default:
      printf("Diagonalisation method = %i?\n", diagFlag);
      exit(1);
   }


   return nconverged;

}






//////////////////////////////////////////////////////
//
//  Compute grid parameters:  no of grid points, max of 1D basis set functions, and strides
//
void DVR::ComputeGridParameters()
{

   //
   //  first dimension has stride 1
   //
   ngp = 1;
   max1db = 1;
   for (int idim = 0; idim < no_dim; ++idim) {
      int npi = n_1dbas[idim];
      ngp *= npi;
      if (max1db < npi)
         max1db = npi;
      incv[idim] = 1;
      for (int k = 0; k < idim; k++)
         incv[idim] *= n_1dbas[k];
   }

   if (verbose > 0) {
      printf("\nDefinition of the Grid:\n  No of grid points for each dimension:");
      for (int k = 0; k < no_dim; k++)
         printf(" %i", n_1dbas[k]);
      printf("\n");
      printf("  Total no of grid points : %i\n", ngp);
      if (verbose > 2) {
         printf("  Strides for each dimension:");
         for (int k = 0; k < no_dim; k++)
            printf(" %i", incv[k]);
         printf("\n");
      }
   }
}


////////////////////////////////////////////////////////////////
//
//  compute the grid points and the 1D kinetic energy matrices
//
void DVR::ComputeGridPointsAndKineticEnergy()
{
   // kinetic energy 1d-matrices
   for (int idim = 0; idim < no_dim; ++idim) {
      int npts = n_1dbas[idim];
      double mass = 1.0;
      int ldt = 0;
      if (tformat == 1)
         ldt = npts;
      switch (dvrtype)
      {
      case 1:
         ho_dvr(npts, gridpara[idim], x_dvr+(idim*max1db), e_kin[idim], ldt, dvr_rep[idim], 1, 0);
         break;
      case 20:
         cm_ham(npts, mass, -0.5*gridpara[idim], 0.5*gridpara[idim], e_kin[idim], x_dvr+(idim*max1db), ldt, 0);
         break;
      case 3:	
          Tdiag(npts, mass, -0.5*gridpara[idim], 0.5*gridpara[idim], x_dvr+(idim*max1db),e_kin[idim] , ldt, 0);
          StepSize[idim] = x_dvr[idim*max1db+1] - x_dvr[idim*max1db];          
         break;
      default:
         sine_dvr(npts, mass, -0.5*gridpara[idim], 0.5*gridpara[idim], x_dvr+(idim*max1db), e_kin[idim], ldt, 0);
	 StepSize[idim] = x_dvr[idim*max1db+1] - x_dvr[idim*max1db];
         break;
      }
   }

   //TV: the final Diagonal KE matrix is constructed. This is used in the FFT
   // This part can be improved. Assumes a 3 dimensional case
   if(dvrtype == 3){  
   for( xa1 = 0; xa1 < n_1dbas[0]; xa1++){
        for( ya2 = 0; ya2 < n_1dbas[1]; ya2++){
           for( za3 = 0; za3 < n_1dbas[2]; za3++){	     
	     KE_diag[xa1*n_1dbas[1]*n_1dbas[2]+ya2*n_1dbas[2]+za3] =   *(e_kin[0]+xa1)+  *(e_kin[1]+ya2) +   *(e_kin[2]+za3);
           }
        }
	
    }
  } 

   if (verbose > 0) {
      for (int k = 0; k < no_dim; k++) {
         printf("  Q%i : %3i grid points from %10.6f to %10.6f",
            k, n_1dbas[k], x_dvr[max1db*k], x_dvr[max1db*k+n_1dbas[k]-1]);
	 if (dvrtype == 1)
	   printf("\n");
	 else
	   printf("  StepSize = %10.6f\n", StepSize[k]);
         if (verbose > 5) {
            for (int l = 0; l < n_1dbas[k]; ++l)
               printf("      %4i  %10.6f\n", l+1, x_dvr[max1db*k + l]);
         }
      }
   }

}





/////////////////////////////////////////////////////////////////////////////////
//
//  compute vdiag, i.e., the potential defined in V at the DVR grid points
//
void DVR::ComputePotential(class Potential &V)
{
  progress_timer t("ComputePotential", verbose);
   // stepping over the grid
   // this is designed for arbitrary dimension
   // so instead of three loops (x, y, z) there is a loop over all grid points
   // and there are counters for the points in each dimension


  // this vector holds all DVR points 
  // TS: it is not really needed, I would think, and,
  // if this is done, it should be done in ComputeGridPointsAndKineticEnergy
  static dVec qtest; qtest.resize(no_dim*ngp);

  //  istart with all indicies 0 (1st step of the grid point loop will increase ii[0])
  int ii[MAXDIM] = {-1,0,0};
  for (int igp = 0; igp < ngp; igp++)
    {
      for (int k = 0; k < no_dim; k++) {
	ii[k] ++;
	if (ii[k] == n_1dbas[k])
	  ii[k] = 0;
	else
	  break;
      }
      // prepare next grid point in q
      for (int id = 0; id < no_dim; ++id)
	qtest[igp*no_dim + id] = x_dvr[id*max1db + ii[id]];
    }

  //
  // loop over the nD grid
  // depending on sampling several points in the neighbourhood are sampled



  if (sampling == 1) { 
#pragma omp parallel
   {
      Potential l_V = V;
#pragma omp for
      for (int igp = 0; igp < ngp; igp++)
      {
	v_diag[igp] = l_V.Evaluate(&qtest[igp*no_dim]);
      } 
      l_V.PrintMinMax();
   }
  }
  // 
  // 8x sampling
  // now this assumes equally space gridpoints in 3D grids
  else if (dvrtype == 0 && sampling == 2) {
    double dx = 0.25 * StepSize[0];
    double dy = 0.25 * StepSize[1];
    double dz = 0.25 * StepSize[2];
#pragma omp parallel
   {
      Potential l_V = V;
#pragma omp for
      for (int igp = 0; igp < ngp; igp++)
      {
	// this is replaced: v_diag[igp] = l_V.Evaluate(&qtest[igp*no_dim]);
	double q[MAXDIM];
	double v8 = 0;
	double x0 = qtest[igp*no_dim];
	double y0 = qtest[igp*no_dim+1];
	double z0 = qtest[igp*no_dim+2];
	double wsum = 1.0/8.0;
	q[0] = x0 + dx;   q[1] = y0 + dy;   q[2] = z0 + dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 - dx;   q[1] = y0 + dy;   q[2] = z0 + dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 + dx;   q[1] = y0 - dy;   q[2] = z0 + dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 + dx;   q[1] = y0 + dy;   q[2] = z0 - dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 - dx;   q[1] = y0 - dy;   q[2] = z0 + dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 - dx;   q[1] = y0 + dy;   q[2] = z0 - dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 + dx;   q[1] = y0 - dy;   q[2] = z0 - dz;   v8 += l_V.Evaluate(q);
	q[0] = x0 - dx;   q[1] = y0 - dy;   q[2] = z0 - dz;   v8 += l_V.Evaluate(q);
	v_diag[igp] = wsum * v8;
      } 
   }
  }
  //
  // 27x sampling
  // equally spaced grids in 3D again
  else if (dvrtype == 0 && sampling == 3) {
    double dx = StepSize[0] / 3.0;
    double dy = StepSize[1] / 3.0;
    double dz = StepSize[2] / 3.0;
#pragma omp parallel
    {
      Potential l_V = V;
#pragma omp for
      for (int igp = 0; igp < ngp; igp++)
	{
	  // this is replaced: v_diag[igp] = l_V.Evaluate(&qtest[igp*no_dim]);
	  double q[MAXDIM];
	  double v27 = 0;
	  double x0 = qtest[igp*no_dim];
	  double y0 = qtest[igp*no_dim+1];
	  double z0 = qtest[igp*no_dim+2];
	  // 27 = 1 center, 6 faces, 12 edges, and 8 corners
	  q[0] = x0;        q[1] = y0;        q[2] = z0;        v27 += l_V.Evaluate(q);
	  // faces
	  q[0] = x0 + dx;   q[1] = y0;        q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0;        q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0 + dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0 - dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0;        q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0;        q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  // edges
	  q[0] = x0 + dx;   q[1] = y0 + dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 + dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0 + dx;   q[1] = y0 - dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 - dy;   q[2] = z0;        v27 += l_V.Evaluate(q);
	  
	  q[0] = x0 + dx;   q[1] = y0;        q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0;        q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 + dx;   q[1] = y0;        q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0;        q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  
	  q[0] = x0;        q[1] = y0 + dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0 - dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0 + dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0;        q[1] = y0 - dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  
	  // corners
	  q[0] = x0 + dx;   q[1] = y0 + dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 + dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 + dx;   q[1] = y0 - dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 + dx;   q[1] = y0 + dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 - dy;   q[2] = z0 + dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 + dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 + dx;   q[1] = y0 - dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  q[0] = x0 - dx;   q[1] = y0 - dy;   q[2] = z0 - dz;   v27 += l_V.Evaluate(q);
	  v_diag[igp] = v27 / 27.0;
	} 
    }
  }
  //
  // unspecific 6x sampling with length 0.2 Bohr
  // works for all grids
  else if (sampling == 4) {
      double dx = 0.2;
#pragma omp parallel
      {
	Potential l_V = V;
#pragma omp for
	for (int igp = 0; igp < ngp; igp++)
	  {
	    // this is replaced: v_diag[igp] = l_V.Evaluate(&qtest[igp*no_dim]);
	    double q[MAXDIM];
	    double v6 = 0;
	    double x0 = qtest[igp*no_dim];
	    double y0 = qtest[igp*no_dim+1];
	    double z0 = qtest[igp*no_dim+2];
	    double wsum = 1.0/6.0;
	    q[0] = x0 + dx;   q[1] = y0;        q[2] = z0;        v6 += l_V.Evaluate(q);
	    q[0] = x0 - dx;   q[1] = y0;        q[2] = z0;        v6 += l_V.Evaluate(q);
	    q[0] = x0;        q[1] = y0 + dx;   q[2] = z0;        v6 += l_V.Evaluate(q);
	    q[0] = x0;        q[1] = y0 - dx;   q[2] = z0;        v6 += l_V.Evaluate(q);
	    q[0] = x0;        q[1] = y0;        q[2] = z0 + dx;   v6 += l_V.Evaluate(q);
	    q[0] = x0;        q[1] = y0;        q[2] = z0 - dx;   v6 += l_V.Evaluate(q);
	    v_diag[igp] = wsum * v6;
	  } 
      }
    }
  //
  //  samplig = 5 or greater
  //  use Sq smoothing operator from Computer Physics Communications 167, 103 (2005) eq 18
  //  works for all grids but is intended for equally spaced grids
  else { 
#pragma omp parallel
    {
      Potential l_V = V;
#pragma omp for
      for (int igp = 0; igp < ngp; igp++)
	{
	  v_diag[igp] = l_V.Evaluate(&qtest[igp*no_dim]);
	} 
    }
    static dVec v_copy;
    v_copy.resize(ngp);
    std::copy(v_diag, &v_diag[ngp-1], &v_copy[0]);
    double qs = sampling;
    double wface = 1.0 / qs;
    double wedge = wface / qs;
    double wcrnr = wedge / qs;
    double wsum = qs*qs*qs / (qs*qs*qs + 6*qs*qs + 12*qs + 8);  //(see eq 18)
    //  loop over all non-edge grid points
    enum {X,Y,Z,NDIM};
    int igp; // grid index 
    for (int k=1; k<n_1dbas[Z]-1; ++k) {
      for (int j=1; j<n_1dbas[Y]-1; ++j) {
	for (int i=1; i<n_1dbas[X]-1; ++i) {
	  double vsmooth = 0;
	  // face
	  igp = sub2ind(i+1,j,  k,  n_1dbas); vsmooth += wface * v_copy[igp];
	  igp = sub2ind(i-1,j,  k,  n_1dbas); vsmooth += wface * v_copy[igp];
	  igp = sub2ind(i,  j+1,k,  n_1dbas); vsmooth += wface * v_copy[igp];
	  igp = sub2ind(i,  j-1,k,  n_1dbas); vsmooth += wface * v_copy[igp];
	  igp = sub2ind(i,  j,  k+1,n_1dbas); vsmooth += wface * v_copy[igp];
	  igp = sub2ind(i,  j,  k-1,n_1dbas); vsmooth += wface * v_copy[igp];
	  // edge
	  igp = sub2ind(i+1,j+1,k,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i-1,j+1,k,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i+1,j-1,k,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i-1,j-1,k,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i+1,j,k+1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i-1,j,k-1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i+1,j,k+1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i-1,j,k-1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i,j+1,k+1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i,j-1,k-1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i,j+1,k+1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  igp = sub2ind(i,j-1,k-1,n_1dbas); vsmooth += wedge * v_copy[igp];  
	  // corner
	  igp = sub2ind(i+1,j+1,k+1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i-1,j+1,k+1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i+1,j-1,k+1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i+1,j+1,k-1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i-1,j-1,k+1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i-1,j+1,k-1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i+1,j-1,k-1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  igp = sub2ind(i-1,j-1,k-1,n_1dbas); vsmooth += wcrnr * v_copy[igp];
	  // point itself
	  igp = sub2ind(i,j,k,n_1dbas); 
	  vsmooth += v_copy[igp];  
	  vsmooth *= wsum;
	  //printf("Smoothing:  %10.6f  %10.6f   %8.1e\n", v_diag[igp], vsmooth, fabs(v_diag[igp]-vsmooth)); 
	  v_diag[igp] = vsmooth;
	}
      }
    }   
  }
}


// small inline utility to convert from 3D subscript into Fortran style (column-major) indices
inline int sub2ind( int i, int j, int k, const int n[] )
{
   enum {X,Y,Z};
   return i + n[X]* (j + n[Y]*(k) );
}
// small inline utility to do the opposite
inline void ind2sub( int I, const int n[], int& i, int& j, int& k )
{
   enum {X,Y,Z};
   k = I%(n[X]*n[Y]);
   I -= k*n[X]*n[Y];
   j = I%n[X];
   I -= j*n[X];
   i = I;
}
#if _OPENMP
// These storage hacks allow for two optimizations:
//   (1) Do reduction without using omp critical 
//   (2) Avoid array re-allocation at each step for each thread
struct Storage {
   dVec tGrad; // temporary
   dVec Gradient; // unreduced return 
//   dVec tmu; // temporary
//   dVec mCm ;
   
 //  int nAtoms = nSites/4*3
   Potential V;
   Storage(int nSites)
      : tGrad(nSites*3)
      , Gradient(nSites*3)
//      , tmu(nSites/4*9)
//      , mCm(nSites*nSites*9*9/16)
   {}
};

//void DVR::ComputeGradient(class Potential &V, int nSites, double *Gradient, double *dEfield, double *PolGrad, class WaterCluster &WaterN)
void DVR::ComputeGradient(class Potential &V, int nSites, double *Gradient, double *dT_x , double *dT_y , double *dT_z, 
                          double *PolGrad, class WaterCluster &WaterN, double *dEfield ) 
{
  progress_timer t("ComputeGradient", verbose);
   static dVec qtest(no_dim * ngp);
   int nAtoms = nSites/4*3;
//   dVec mu(nAtoms*3);
//   dVec mu_cross_mu(nAtoms*3*nAtoms*3);

//   std::fill(mu.begin(), mu.end(), 0.);
//   std::fill(mu_cross_mu.begin(), mu_cross_mu.end(), 0.);

   // allocate thread local arrays 
   int nthread = omp_get_max_threads();
   static std::vector<Storage> storage(nthread, Storage(nSites));

   // 3D loop for qtest
   enum {X,Y,Z,NDIM};

#  pragma omp parallel
{
#  pragma omp for
   for (int k=0; k<n_1dbas[Z]; ++k) {
      for (int j=0; j<n_1dbas[Y]; ++j) {
         for (int i=0; i<n_1dbas[X]; ++i) {
            int ig = sub2ind(i,j,k,n_1dbas);
            qtest[ ig*NDIM + X ] = x_dvr[ X*max1db + i ];
            qtest[ ig*NDIM + Y ] = x_dvr[ Y*max1db + j ];
            qtest[ ig*NDIM + Z ] = x_dvr[ Z*max1db + k ];
         }
      }
   }
   int ithread = omp_get_thread_num(); 
   Storage& loc = storage[ithread];
   std::fill(loc.Gradient.begin(), loc.Gradient.end(), 0.);
   loc.V = V;
   dVec mCm(nAtoms*3*nAtoms*3);
   std::fill(mCm.begin(), mCm.end(), 0.);
//   std::fill(loc.tmu.begin(), loc.tmu.end(), 0.);
   dVec tmu(nAtoms*3*nAtoms*3);
   std::fill(tmu.begin(), tmu.end(), 0.);

#  pragma omp barrier
#  pragma omp for
   for (int igp = 0; igp < ngp; igp++)
   {
      std::fill( loc.tGrad.begin(), loc.tGrad.end(), 0.);

      loc.V.EvaluateGradient(&qtest[igp*no_dim], loc.tGrad, tmu, mCm, wavefn[igp] , WaterN);
      for (int j=0; j<nSites*3; ++j) 
         loc.Gradient[j] += wavefn[igp]*wavefn[igp]*loc.tGrad[j];

   }

   loc.V.FinalGradient(nAtoms, &loc.Gradient[0] , &tmu[0], &mCm[0] , &dT_x[0] , &dT_y[0], &dT_z[0],  &dEfield[0]);


} // omp parallel
   for (int ithread=0; ithread<nthread; ++ithread) {
      for (int j=0; j<nSites*3; ++j)
         Gradient[j] += storage[ithread].Gradient[j];
/*
      for (int j=0; j<nAtoms*3; ++j)
         mu[j] += storage[ithread].tmu[j];

      for (int i=0; i< nAtoms*nAtoms*9; ++i)
          mu_cross_mu[i] += storage[ithread].mCm[i];
*/
   }
   
  V.SubtractWWGradient (nSites, &PolGrad[0], &Gradient[0]) ; 

}
#else // not _OPENMP
void DVR::ComputeGradient(class Potential &V, int nSites, double *Gradient, double *dT_x , double *dT_y , double *dT_z, 
                          double *PolGrad, class WaterCluster &WaterN, double *dEfield ) 
{
  progress_timer t("ComputeGradient", verbose);
  static dVec qtest(no_dim * ngp);
  int nAtoms = nSites/4*3;

  // 3D loop for qtest
  enum {X,Y,Z,NDIM};
  for (int k=0; k<n_1dbas[Z]; ++k) {
     for (int j=0; j<n_1dbas[Y]; ++j) {
        for (int i=0; i<n_1dbas[X]; ++i) {
           int ig = sub2ind(i,j,k,n_1dbas);
           qtest[ ig*NDIM + X ] = x_dvr[ X*max1db + i ];
           qtest[ ig*NDIM + Y ] = x_dvr[ Y*max1db + j ];
           qtest[ ig*NDIM + Z ] = x_dvr[ Z*max1db + k ];
        }
     }
  }

  dVec mCm(nAtoms*3*nAtoms*3);
  dVec tmu(nAtoms*3*nAtoms*3);
  dVec tGrad(nSites*3);

  for (int igp = 0; igp < ngp; igp++)
  {
     std::fill( tGrad.begin(), tGrad.end(), 0.);

     V.EvaluateGradient(&qtest[igp*no_dim], tGrad, tmu, mCm, wavefn[igp] , WaterN);
     for (int j=0; j<nSites*3; ++j) 
        Gradient[j] += wavefn[igp]*wavefn[igp]*tGrad[j];

  }

  V.FinalGradient(nAtoms, &Gradient[0] , &tmu[0], &mCm[0] , &dT_x[0] , &dT_y[0], &dT_z[0],  &dEfield[0]);
  V.SubtractWWGradient (nSites, &PolGrad[0], &Gradient[0]) ; 

}
#endif // _OPENMP









//////////////////////////////////////////////////////////////////////////////////////////////
//
//  computes the particle-in-a-n dimensional-box wavefunction to be used as a start vector
//
// this is not really Psi for a sine grid, but close enough,
// and in this way it will also work for HO DVRs
//
//
//
void DVR::ParticleInAnDBoxWf(double *wf)
{

   int *ii = new int[no_dim];
   double *q  = new double[no_dim];
   double *x0 = new double[no_dim];
   double *L  = new double[no_dim];

   // get grid edge x0 and grid length L
   for (int id = 0; id < no_dim; id++) {
      x0[id] = x_dvr[id*max1db];
      L[id] = x_dvr[id*max1db+n_1dbas[id]-1] - x0[id];
      if (verbose > 1)
         printf("dimension %i  x0 = %10.6f   L = %10.6f\n",id, x0[id], L[id]);
   }

   //  istart with all indicies 0 (1st step of the grid point loop will increase ii[0])
   ii[0] = -1;
   for (int id = 1; id < no_dim; id++)
      ii[id] = 0;



   // loop over the nD grid
   for (int igp = 0; igp < ngp; igp++) {

      // increase ii[0], and, if overflow, ii[1], and, if overflow ...
      for (int k = 0; k < no_dim; k++) {
         ii[k] ++;
         if (ii[k] == n_1dbas[k])
            ii[k] = 0;
         else
            break;
      }

      // prepare next grid point in q
      for (int id = 0; id < no_dim; ++id)
         q[id] = x_dvr[id*max1db + ii[id]];

      // compute Psi(q1, q2, q3, ...)
      //
      // if this ever becomes a bottleneck, make a list of sine values first
      //
      wf[igp] = 1;
      for (int id = 0; id < no_dim; id++) {
         wf[igp] *= sin(PI/L[id]*(q[id]-x0[id]));
      }

      // print V at every grid point:
      if (verbose > 5) {
         for (int id = 0; id < no_dim; id++)
            printf("%12.6f ", q[id]);
         printf("   %12.8f\n", wf[igp]);
      }
   }


   delete[] L;
   delete[] x0;
   delete[] q;
   delete[] ii;

}


void DVR::ComputeDiagonal(double *diag)
{

   // initialize diagonal with V
   memcpy(diag, v_diag, ngp*sizeof(double));

   void addtdiag(int n1, int inc1, int nod, int *ndod, int *incr, double *t, double *diag, int ng);


   int k;


   // add the diagonal of each T(i) n times where n is the product of the no of grid points in the other dimensions
   //
   // this works similar to mtx, each element is "n" times needed, and there are start points and strides
   //
   for (int id = 0; id < no_dim; id++) {
      k = n_1dbas[id]; n_1dbas[id] = n_1dbas[0]; n_1dbas[0] = k;
      k = incv[id]; incv[id] = incv[0]; incv[0] = k;
      addtdiag(n_1dbas[0], incv[0], no_dim-1, &n_1dbas[1], &incv[1], e_kin[id], diag, ngp);
      k = n_1dbas[id]; n_1dbas[id] = n_1dbas[0]; n_1dbas[0] = k;
      k = incv[id]; incv[id] = incv[0]; incv[0] = k;
   }
}


////////////////////////////////////////////////////////////////////
//
//  This is used by DVR::ComputeDiagonal(double *diag)
//  look into mtx.cpp titxbl() documentation
//  only the diagonal elements of T(1) are added for the diagonal of H
//
void addtdiag(int n1, int inc1, int nod, int *ndod, int *incr, double *t, double *diag, int ng)
{
   int iod, irc, nrc, incrc, k, i1;
   int ir[20];  // this should be MAXDIM, but I do not want this to be a class member
   double *tt;

   nrc = 1;
   for (iod = 0; iod < nod; iod++)
      nrc *= ndod[iod];

   // counters for other dimensions
   ir[0] = -1;
   for (iod = 1; iod < nod; iod++)
      ir[iod] = 0;

   // loop over the product of the other dimension's number of grid points
   // so many time does each element of T(1) pop up in the matrix
   for (irc = 0; irc < nrc; irc++)
   {
      for (k = 0; k < nod; k++) {
         ir[k] ++;
         if (ir[k] == ndod[k])
            ir[k] = 0;
         else
            break;
      }
      incrc = 0;
      for (k = 0; k < nod; k++)
         incrc += ir[k] * incr[k];
      for (i1 = 0; i1 < n1; i1 ++) {
         int ig = incrc + i1*inc1;
         tt = &t[i1*(i1+1)/2];
         diag[ig] += tt[i1];
         //hop[ig + ng*ig] += tt[i1];
         //for (j1 = 0; j1 < i1; j1 ++) {
         //  int jg = incrc + j1*inc1;
         //  hop[ig + ng * jg] = tt[j1];
         //  hop[jg + ng * ig] = tt[j1];
         //}
      }
   }
}

/////////////////////////////////////////
//
//  writing a .cub file: the ground state is wavefunction 1
//
void DVR::WriteCubeFile(int iwf, const char *fname, int nAtoms, const int *Z, const double *position, int cubeflag)
{
   if (verbose > 2)
      cout << "Writing cube-file " << fname << " for wavefunction " << iwf << "\n";

   if (no_dim != 3) {
      cout << "Error in writewfcuts; this is a function for 3D grids only\n";
      exit(1);
   }

   if (iwf > nconverged) {
      cout << "WriteCubeFile: There are only " << nconverged << " states available at the moment.\n";
      exit(1);
   }

   int nx = n_1dbas[0];
   int ny = n_1dbas[1];
   int nz = n_1dbas[2];
   int ix, iy, iz;

   double *xgrid = x_dvr;
   double *ygrid = x_dvr+max1db;
   double *zgrid = x_dvr+2*max1db;

   //
   //  this is a DVR wavefunction, so the volume element is already in the
   //  value of the wavefunction at that grid point
   //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
   //
   //  good check: the sum over a DVR grid should always be 1.00000000
   //
   //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
   //
   //  all this makes only sense for Sine DVR (equidistant grids)
   //

   double dV = (zgrid[nz-1]-zgrid[0])*(ygrid[nz-1]-ygrid[0])*(xgrid[nz-1]-xgrid[0]) / double((nx-1)*(ny-1)*(nz-1));
   double oosqrdv = 1.0/sqrt(dV);

   if (verbose > 2)     
     cout << "Cube normalization factor is " << oosqrdv << "\n";

   
   double *wfp = &wavefn[0] +(iwf-1)*ngp;

   FILE *cube;
   cube = fopen(fname,"w+");


   //
   // 1=gOpenMol 2=Gaussian cube file format
   // all this does not work for HO DVR (this should be tested in dvr3d!)
   //
   double rho = 0, intr = 0;

   if (cubeflag == 1)
   {
      // gOpenMol
      fprintf(cube, "3 3\n%i %i %i\n", nz, ny, nx);
      const double B2A = Bohr2Angs;
      fprintf(cube, "%13.6e %13.6e    %13.6e %13.6e    %13.6e %13.6e\n",
         zgrid[0]*B2A, zgrid[nz-1]*B2A, ygrid[0]*B2A, ygrid[ny-1]*B2A, xgrid[0]*B2A, xgrid[nx-1]*B2A);
      for (iz = 0; iz < nz; ++iz) {
         for (iy = 0; iy < ny; ++iy) {
            for (ix = 0; ix < nx; ++ix) {
               rho = wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]] * oosqrdv;
               fprintf(cube, "%13.6e\n", rho);
               intr += rho*rho;
            }
         }
      }
   }
   else
   {
      // Gaussian-like cube file
      int ValuesPerLine = 6;
      double nought = 0.0;
      double dx = xgrid[1] - xgrid[0];
      double dy = ygrid[1] - ygrid[0];
      double dz = zgrid[1] - zgrid[0];
      // two comment lines suitable for cubeint
      fprintf(cube, " 5 0\n");
      fprintf(cube, " 0.01 0.001 0.0001 0.00001 0.000001\n");
      // header: no of atoms and definition of the grid
      // vkv thinks that gaussian cube files for orbitals requires natoms < 0
      //fprintf(cube, "%5i  %11.6f  %11.6f  %11.6f\n", nAtoms, xgrid[0], ygrid[0], zgrid[0]);
      fprintf(cube, "%5i  %11.6f  %11.6f  %11.6f\n", -nAtoms, xgrid[0], ygrid[0], zgrid[0]);
      fprintf(cube, "%5i  %11.6f  %11.6f  %11.6f\n", nx, dx, nought, nought);
      fprintf(cube, "%5i  %11.6f  %11.6f  %11.6f\n", ny, nought, dy, nought);
      fprintf(cube, "%5i  %11.6f  %11.6f  %11.6f\n", nz, nought, nought, dz);
      // atoms list: the 2nd number is ignored by most programs and usually 0.0
      // for cubeint it is set to the van der Waals radius
      for (int k = 0; k < nAtoms; ++k) {
         const double *r = position + 3*k;
         double RvdW = 0.0;
         switch (Z[k])
         {
         case 1: RvdW = 1.20; break;
         case 6: RvdW = 1.70; break;
         case 7: RvdW = 1.55; break;
         case 8: RvdW = 1.52; break;
         default: break; // do nothing;
         }
         fprintf(cube, "   %i %11.6f  %11.6f  %11.6f  %11.6f\n",
            Z[k], RvdW*Angs2Bohr, r[0]*Angs2Bohr, r[1]*Angs2Bohr, r[2]*Angs2Bohr);
      }
      //vkv
      fprintf(cube, "   1  %5i \n", iwf);
      // here comes the cube
      for (ix = 0; ix < nx; ++ix) {
         for (iy = 0; iy < ny; ++iy) {
            int iPerLine = 1;
            for (iz = 0; iz < nz; ++iz) {
               rho = wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]] * oosqrdv;
               fprintf(cube, "%13.6e ", rho);
               if (iPerLine % ValuesPerLine == 0)
                  fprintf(cube, "\n");
               iPerLine++;
               intr += rho*rho;
            }
            if (nz % ValuesPerLine != 0)
               fprintf(cube, "\n");
         }
      }
   }
   if (verbose > 2)
      printf("  Int d3r rho(r) = %11.9f\n", intr*dV);

   fclose(cube);

}





void DVR::GetWaveFnCube(int iwf, double *cube)
{

  if (iwf > nconverged) {
    cout << "GetWaveFnCube: There are only " << nconverged << " states available at the moment.\n";
    exit(1);
  }

  double *wfp = &wavefn[0] +(iwf-1)*ngp;

  int nx = n_1dbas[0];
  int ny = n_1dbas[1];
  int nz = n_1dbas[2];
  int ix, iy, iz;
  double *xgrid = x_dvr;
  double *ygrid = x_dvr+max1db;
  double *zgrid = x_dvr+2*max1db;
  //
  //  this is a DVR wavefunction, so the volume element is already in the
  //  value of the wavefunction at that grid point
  //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
  //
  //  good check: the sum over a DVR grid should always be 1.00000000
  //
  //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
  //
  //  all this makes only sense for Sine DVR (equidistant grids)
  //
  double dV = (zgrid[nz-1]-zgrid[0])*(ygrid[nz-1]-ygrid[0])*(xgrid[nz-1]-xgrid[0]) / double((nx-1)*(ny-1)*(nz-1));
  double oosqrdv = 1.0/sqrt(dV);

  double *cubeptr = cube;
  for (ix = 0; ix < nx; ++ix) {
    for (iy = 0; iy < ny; ++iy) {
      for (iz = 0; iz < nz; ++iz) {
	*cubeptr = wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]] * oosqrdv;
	cubeptr ++;
      }
    }
  }
  
}







void DVR::WriteOneDCuts(void)
{
   if (no_dim != 3) {
      cout << "Error in WriteCuts; this is a function for 3D grids only\n";
      exit(1);
   }

   int nx = n_1dbas[0];
   int ny = n_1dbas[1];
   int nz = n_1dbas[2];
   int ix, iy, iz;
   FILE *flout;

   double *xg = x_dvr;
   double *yg = x_dvr+max1db;
   double *zg = x_dvr+2*max1db;

   iz = nz/2; iy = ny/2 ; cout << "Cut long X at the " << iz << "-th z-grid point and" << iy << "-th y-grid point\n";
   flout = fopen("POTENTIAL.X","w+");
   for (ix = 0; ix < nx; ++ix) {
      fprintf(flout, "%10.7f %15.7e\n", xg[ix], v_diag[ix*incv[0]+iy*incv[1]+iz*incv[2]]);
   }
   fprintf(flout,"\n");
   fclose(flout);



   // now the wavefunctions
   //
   //  this is a DVR wavefunction, so the volume element is already in the
   //  value of the wavefunction at that grid point
   //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
   //
   //  good check: the sum over a DVR grid should always be 1.00000000
   //
   //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
   //

   double dV = (zg[nz-1]-zg[0])*(yg[nz-1]-yg[0])*(xg[nz-1]-xg[0]) / double((nx-1)*(ny-1)*(nz-1));
   double oosqrdv = 1.0/sqrt(dV);

   for (int iwf = 0; iwf < nconverged; ++iwf)
   {
      char fname[15];
      double *wfp = &wavefn[0] + iwf*ngp;

      iz = nz/2  ; iy = ny/2  ; 
      sprintf(fname,"WaveFn%02i.X", iwf+1); flout = fopen(fname,"w+");
      for (ix = 0; ix < nx; ++ix) {
         fprintf(flout, "%10.7f %10.7f %10.7f  %15.7e\n", xg[ix], yg[iy], zg[iz], wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]]*oosqrdv);
      }
      fprintf(flout,"\n");
      fclose(flout);

   }
}
///////////////////////////////////////////
//
//  print 2D plot files of cuts along the the main coordinate planes
//  through the potential
//
//
void DVR::WriteCuts(void)
{
   if (no_dim != 3) {
      cout << "Error in WriteCuts; this is a function for 3D grids only\n";
      exit(1);
   }

   int nx = n_1dbas[0];
   int ny = n_1dbas[1];
   int nz = n_1dbas[2];
   int ix, iy, iz;
   FILE *flout;

   double *xg = x_dvr;
   double *yg = x_dvr+max1db;
   double *zg = x_dvr+2*max1db;

   iz = nz/2; cout << "Cut XY at the " << iz << "-th z-grid point\n";
   flout = fopen("POTENTIAL.XY","w+");
   for (iy = 0; iy < ny; ++iy) {
      for (ix = 0; ix < nx; ++ix) {
         fprintf(flout, "%10.7f %10.7f %15.7e\n", xg[ix], yg[iy], v_diag[ix*incv[0]+iy*incv[1]+iz*incv[2]]);
      }
      fprintf(flout,"\n");
   }
   fclose(flout);

   iy = ny/2; cout << "Cut XZ at the " << iy << "-th y-grid point\n";
   flout = fopen("POTENTIAL.XZ","w+");
   for (iz = 0; iz < nz; ++iz) {
      for (ix = 0; ix < nx; ++ix) {
         fprintf(flout, "%10.7f %10.7f %15.7e\n", xg[ix], zg[iz], v_diag[ix*incv[0]+iy*incv[1]+iz*incv[2]]);
      }
      fprintf(flout,"\n");
   }
   fclose(flout);

   ix = nx/2; cout << "Cut YZ at the " << ix << "-th x-grid point\n";
   flout = fopen("POTENTIAL.YZ","w+");
   for (iy = 0; iy < ny; ++iy) {
      for (iz = 0; iz < nz; ++iz) {
         fprintf(flout, "%10.7f %10.7f %15.7e\n", yg[iy], zg[iz], v_diag[ix*incv[0]+iy*incv[1]+iz*incv[2]]);
      }
      fprintf(flout,"\n");
   }
   fclose(flout);


   // now the wavefunctions
   //
   //  this is a DVR wavefunction, so the volume element is already in the
   //  value of the wavefunction at that grid point
   //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
   //
   //  good check: the sum over a DVR grid should always be 1.00000000
   //
   //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
   //

   double dV = (zg[nz-1]-zg[0])*(yg[nz-1]-yg[0])*(xg[nz-1]-xg[0]) / double((nx-1)*(ny-1)*(nz-1));
   double oosqrdv = 1.0/sqrt(dV);

   for (int iwf = 0; iwf < nconverged; ++iwf)
   {
      char fname[15];
      double *wfp = &wavefn[0] + iwf*ngp;

      iz = nz/2;
      sprintf(fname,"WaveFn%02i.XY", iwf+1); flout = fopen(fname,"w+");
      for (iy = 0; iy < ny; ++iy) {
         for (ix = 0; ix < nx; ++ix)
            fprintf(flout, "%10.7f %10.7f %15.7e\n", xg[ix], yg[iy], wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]]*oosqrdv);
         fprintf(flout,"\n");
      }
      fclose(flout);

      iy = ny/2;
      sprintf(fname,"WaveFn%02i.XZ", iwf+1); flout = fopen(fname,"w+");
      for (iz = 0; iz < nz; ++iz) {
         for (ix = 0; ix < nx; ++ix)
            fprintf(flout, "%10.7f %10.7f %15.7e\n", xg[ix], zg[iz], wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]]*oosqrdv);
         fprintf(flout,"\n");
      }
      fclose(flout);

      ix = nx/2;
      sprintf(fname,"WaveFn%02i.YZ", iwf+1); flout = fopen(fname,"w+");
      for (iy = 0; iy < ny; ++iy) {
         for (iz = 0; iz < nz; ++iz)
            fprintf(flout, "%10.7f %10.7f %15.7e\n", yg[iy], zg[iz], wfp[ix*incv[0]+iy*incv[1]+iz*incv[2]]*oosqrdv);
         fprintf(flout,"\n");
      }
      fclose(flout);
   }
}



void DVR::EnergyPartitioning(class Potential &V)
{
  if (nconverged < 1) {
      cout << "ExpectationValues: No converged states available at the moment.\n";
      exit(1);
  }
   int nx = n_1dbas[0];
   int ny = n_1dbas[1];
   int nz = n_1dbas[2];
   int ix, iy, iz;

   double *xgrid = x_dvr;
   double *ygrid = x_dvr+max1db;
   double *zgrid = x_dvr+2*max1db;

   //
   //  this is a DVR wavefunction, so the volume element is already in the
   //  value of the wavefunction at that grid point
   //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
   //
   //  good check: the sum over a DVR grid should always be 1.00000000
   //
   //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
   //
   //  all this makes only sense for Sine DVR (equidistant grids)
   //

   double dV = (zgrid[nz-1]-zgrid[0])*(ygrid[nz-1]-ygrid[0])*(xgrid[nz-1]-xgrid[0]) / double((nx-1)*(ny-1)*(nz-1));
   double oosqrdv = 1.0/sqrt(dV);

   if (verbose > 1)     
     cout << "Cube normalization factor is " << oosqrdv << "\n";

   
   double *wfp = &wavefn[0];
   dVec wf; wf.resize(nconverged);
   dVec rho; rho.resize(nconverged);
   dVec vRep ; vRep.resize(nconverged);
   dVec vElec ; vElec.resize(nconverged);
   dVec vPol ; vPol.resize(nconverged);
   dVec vInd ; vInd.resize(nconverged);
   for (ix = 0; ix < nx; ++ix)
     for (iy = 0; iy < ny; ++iy)
       for (iz = 0; iz < nz; ++iz)
         {
           // get wavefunctions at this grid point
           int idx = ix*incv[0]+iy*incv[1]+iz*incv[2];
           for (int i_state = 0; i_state < nconverged; ++i_state) {
             wf[i_state] = wavefn[i_state*ngp + idx];
             rho[i_state] = wf[i_state] * wf[i_state];
           }
           // sums for expectation values
           // sums for transition dipoles
           double relectron[3]; relectron[0] = xgrid[ix] ; relectron[1] = ygrid[iy] ; relectron[2] = zgrid[iz] ;
           double energies[5] ;
           V.Evaluate(&relectron[0])  ; 
           V.ReportEnergies(5, energies)  ;
           //cout << energies[0] << endl;
           //cout << rho[0] << endl;
           for (int i = 0; i < nconverged; ++i) {
             //vKinteic[i] += vKinteic[i]+rho[i_siate]*;
             vElec[i] += rho[i]*energies[0] ;
             vInd[i]  += rho[i]*energies[1] ;
             vRep[i]  += rho[i]*energies[2] ;
             vPol[i]  += rho[i]*energies[3] ;
           }
         }

//   cout << AU2EV << endl; 
   cout << "\nEnergy expectation values (all in meV)\n";
   cout << "\nState     vElec     vInd      vRep      vPol  \n";
   for (int i = 0; i < nconverged; ++i) {
//   cout << vElec[i] ; 
     printf(" %3i   %10.5f  %10.5f  %10.5f  %10.5f\n ",
            i,  vElec[i]*AU2MEV ,vInd[i]*AU2MEV,  vRep[i]*AU2MEV, vPol[i]*AU2MEV);
   }
   cout << "\n";



}

/////////////////////////////////////////
//
//  expectation values
//
void DVR::ExpectationValues(int verbose)
{
   if (verbose > 1)
      cout << "\nExpectation values are printed for all converged wavefunctions\n";
   
   if (nconverged < 1) {
      cout << "ExpectationValues: No converged states available at the moment.\n";
      exit(1);
   }

   int nx = n_1dbas[0];
   int ny = n_1dbas[1];
   int nz = n_1dbas[2];
   int ix, iy, iz;

   double *xgrid = x_dvr;
   double *ygrid = x_dvr+max1db;
   double *zgrid = x_dvr+2*max1db;

   //
   //  this is a DVR wavefunction, so the volume element is already in the
   //  value of the wavefunction at that grid point
   //  in the grid-integrater we have dV=dxdydz  dx = (xmax-xmin)/(nx-1)
   //
   //  good check: the sum over a DVR grid should always be 1.00000000
   //
   //  multiplication with 1/sqrt(dV) should give the Bohr^(-3/2) unit of the wavefunction
   //
   //  all this makes only sense for Sine DVR (equidistant grids)
   //

   double dV = (zgrid[nz-1]-zgrid[0])*(ygrid[nz-1]-ygrid[0])*(xgrid[nz-1]-xgrid[0]) / double((nx-1)*(ny-1)*(nz-1));
   double oosqrdv = 1.0/sqrt(dV);

   if (verbose > 1)     
     cout << "Cube normalization factor is " << oosqrdv << "\n";

   
   double *wfp = &wavefn[0];
   dVec wf; wf.resize(nconverged);
   dVec rho; rho.resize(nconverged);
   dVec intr; intr.resize(nconverged);
   
   //
   // lots of expectation values for all converged states 
   //
   //   
   dVec xyzexpval; xyzexpval.resize(nconverged*3); // xyzexpval[3*i_state + k]  <n|x|n>, <n|y|n>, and <n|z|n>
   dVec rexpval; rexpval.resize(nconverged); // <n|r|n>
   dVec rsqexpval; rsqexpval.resize(nconverged); // <n|r^2|n>
   for (int i_state = 0; i_state < nconverged; ++i_state) {
     rexpval[i_state] = 0;
     rsqexpval[i_state] = 0;
     for (int k = 0; k < 3; ++k)
       xyzexpval[3*i_state + k] = 0;
   }

   // <n|x|0>, <n|y|0>, and <n|z|0> for all converged excited states
   dVec transdipole; transdipole.resize((nconverged-1)*3); // transdipole[3*i_exstate + k]


   for (ix = 0; ix < nx; ++ix) 
     for (iy = 0; iy < ny; ++iy) 
       for (iz = 0; iz < nz; ++iz) 
	 {
	   // get wavefunctions at this grid point
	   int idx = ix*incv[0]+iy*incv[1]+iz*incv[2];
	   for (int i_state = 0; i_state < nconverged; ++i_state) {
	     wf[i_state] = wavefn[i_state*ngp + idx];
	     rho[i_state] = wf[i_state] * wf[i_state];
	     intr[i_state] += rho[i_state];
	   }
	   // sums for expectation values
	   for (int i_state = 0; i_state < nconverged; ++i_state) {
	       xyzexpval[3*i_state + 0] += rho[i_state] * xgrid[ix]; 
	       xyzexpval[3*i_state + 1] += rho[i_state] * ygrid[iy]; 
	       xyzexpval[3*i_state + 2] += rho[i_state] * zgrid[iz];
	       rsqexpval[i_state] += rho[i_state] * (xgrid[ix]*xgrid[ix] + ygrid[iy]*ygrid[iy] + zgrid[iz]*zgrid[iz]);
	   }
	   // sums for transition dipoles
	   for (int i = 1; i < nconverged; ++i) { 
	     transdipole[3*(i-1) + 0] += wf[i] * xgrid[ix] * wf[0];
	     transdipole[3*(i-1) + 1] += wf[i] * ygrid[iy] * wf[0];
	     transdipole[3*(i-1) + 2] += wf[i] * zgrid[iz] * wf[0];
	   }
	 }


   //
   //  print some nice output
   //
   cout << "\nState     <|r|>     sqrt(<r^2>)   variance  (all in Angstrom)\n";
   for (int i = 0; i < nconverged; ++i) {
     if (fabs(intr[i]-1.0) > 1e-8)
       cout << "Warning: normaliziation integral of state " << i << " is not 1.0, but " << intr[i]
	    << "\nThis chould not happen.\n";
     cout << "normalization of state" << i << " is " << intr[i] << "\n" ; 
     for (int k = 0; k < 3; ++k)
       rexpval[i] += xyzexpval[3*i + k] * xyzexpval[3*i + k];
     rexpval[i] = sqrt(rexpval[i]);
     printf(" %3i   %10.5f  %10.5f  %10.5f\n", 
	    i, Bohr2Angs*rexpval[i], Bohr2Angs*sqrt(rsqexpval[i]), 
	    Bohr2Angs*sqrt(rsqexpval[i] - rexpval[i]*rexpval[i]) );
   }
   cout << "\n";


   if (nconverged > 1) {
     cout << "Transition dipoles d^2 and d=(<n|x|0>, <n|y|0>, <n|z|0>) (all in au)\n";
     for (int i = 1; i < nconverged; ++i) {
       double dsq = transdipole[3*(i-1)]*transdipole[3*(i-1)];
       dsq += transdipole[3*(i-1)+1]*transdipole[3*(i-1)+1];
       dsq += transdipole[3*(i-1)+2]*transdipole[3*(i-1)+2];
       printf(" %3i      %10.5f     (%10.5f,  %10.5f,  %10.5f)\n",
	      i, dsq, transdipole[3*(i-1)], transdipole[3*(i-1)+1], transdipole[3*(i-1)+2]);
     }
   cout << "\n";
   }
}
