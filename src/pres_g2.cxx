#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fftw3.h>
#include "grid.h"
#include "fields.h"
#include "pres_g2.h"
#include "defines.h"

cpres_g2::cpres_g2(cgrid *gridin, cfields *fieldsin, cmpi *mpiin)
{
  std::printf("Creating instance of object pres_g2\n");
  grid   = gridin;
  fields = fieldsin;
  mpi    = mpiin;

  allocated = false;
}

cpres_g2::~cpres_g2()
{
  if(allocated)
  {
    fftw_destroy_plan(iplanf);
    fftw_destroy_plan(iplanb);
    fftw_destroy_plan(jplanf);
    fftw_destroy_plan(jplanb);

    fftw_free(fftini);
    fftw_free(fftouti);
    fftw_free(fftinj);
    fftw_free(fftoutj);

    delete[] a;
    delete[] c;
    delete[] work2d;

    delete[] bmati;
    delete[] bmatj;
  }

  std::printf("Destroying instance of object pres_g2\n");
}

int cpres_g2::init()
{
  int imax, jmax, kmax;
  int itot, jtot, kgc;

  itot = grid->itot;
  jtot = grid->jtot;
  imax = grid->imax;
  jmax = grid->jmax;
  kmax = grid->kmax;
  kgc  = grid->kgc;

  bmati = new double[itot];
  bmatj = new double[jtot];
  
  // compute the modified wave numbers of the 2nd order scheme
  double dxidxi = 1./(grid->dx*grid->dx);
  double dyidyi = 1./(grid->dy*grid->dy);

  const double pi = std::acos(-1.);

  for(int j=0; j<jtot/2+1; j++)
    bmatj[j] = 2. * (std::cos(2.*pi*(double)j/(double)jtot)-1.) * dyidyi;

  for(int j=jtot/2+1; j<jtot; j++)
    bmatj[j] = bmatj[jtot-j];

  for(int i=0; i<itot/2+1; i++)
    bmati[i] = 2. * (std::cos(2.*pi*(double)i/(double)itot)-1.) * dxidxi;

  for(int i=itot/2+1; i<itot; i++)
    bmati[i] = bmati[itot-i];

  // allocate help variables for the matrix solver
  a = new double[kmax];
  c = new double[kmax];
  work2d = new double[imax*jmax];

  // create vectors that go into the tridiagonal matrix solver
  for(int k=0; k<kmax; k++)
  {
    a[k] = grid->dz[k+kgc] * grid->dzhi[k+kgc  ];
    c[k] = grid->dz[k+kgc] * grid->dzhi[k+kgc+1];
  }

  fftini  = fftw_alloc_real(itot);
  fftouti = fftw_alloc_real(itot);
  fftinj  = fftw_alloc_real(jtot);
  fftoutj = fftw_alloc_real(jtot);

  allocated = true;

  return 0;
}

int cpres_g2::load()
{ 
  int itot, jtot;

  itot = grid->itot;
  jtot = grid->jtot;

  char filename[256];
  std::sprintf(filename, "%s.%07d", "fftwplan", 0);

  if(mpi->mpiid == 0)
    std::printf("Loading \"%s\"\n", filename);

  int n = fftw_import_wisdom_from_filename(filename);
  if(n == 0)
  {
    if(mpi->mpiid == 0)
      std::printf("ERROR \"%s\" does not exist\n", filename);
    return 1;
  }

  iplanf = fftw_plan_r2r_1d(itot, fftini, fftouti, FFTW_R2HC, FFTW_EXHAUSTIVE);
  iplanb = fftw_plan_r2r_1d(itot, fftini, fftouti, FFTW_HC2R, FFTW_EXHAUSTIVE);
  jplanf = fftw_plan_r2r_1d(jtot, fftinj, fftoutj, FFTW_R2HC, FFTW_EXHAUSTIVE);
  jplanb = fftw_plan_r2r_1d(jtot, fftinj, fftoutj, FFTW_HC2R, FFTW_EXHAUSTIVE);

  fftw_forget_wisdom();

  return 0;
}

int cpres_g2::save()
{
  int itot, jtot;

  itot = grid->itot;
  jtot = grid->jtot;

  iplanf = fftw_plan_r2r_1d(itot, fftini, fftouti, FFTW_R2HC, FFTW_EXHAUSTIVE);
  iplanb = fftw_plan_r2r_1d(itot, fftini, fftouti, FFTW_HC2R, FFTW_EXHAUSTIVE);
  jplanf = fftw_plan_r2r_1d(jtot, fftinj, fftoutj, FFTW_R2HC, FFTW_EXHAUSTIVE);
  jplanb = fftw_plan_r2r_1d(jtot, fftinj, fftoutj, FFTW_HC2R, FFTW_EXHAUSTIVE);

  if(mpi->mpiid == 0)
  {
    char filename[256];
    std::sprintf(filename, "%s.%07d", "fftwplan", 0);

    std::printf("Saving \"%s\"\n", filename);

    int n = fftw_export_wisdom_to_filename(filename);
    if(n == 0)
    {
      std::printf("ERROR \"%s\" cannot be saved\n", filename);
      return 1;
    }
  }

  return 0;
}

int cpres_g2::pres_in(double * restrict p, 
                      double * restrict u , double * restrict v , double * restrict w , 
                      double * restrict ut, double * restrict vt, double * restrict wt, 
                      double * restrict dzi,
                      double dt)
{
  int    ijk,ii,jj,kk,ijkp,jjp,kkp;
  int    igc,jgc,kgc;
  double dxi,dyi;

  ii = 1;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  jjp = grid->imax;
  kkp = grid->imax*grid->jmax;

  dxi = 1./grid->dx;
  dyi = 1./grid->dy;

  igc = grid->igc;
  jgc = grid->jgc;
  kgc = grid->kgc;

  // set the cyclic boundary conditions for the tendencies
  grid->boundary_cyclic(ut);
  grid->boundary_cyclic(vt);
  grid->boundary_cyclic(wt);
  mpi->waitall();

  // write pressure as a 3d array without ghost cells
  for(int k=0; k<grid->kmax; k++)
    for(int j=0; j<grid->jmax; j++)
#pragma ivdep
      for(int i=0; i<grid->imax; i++)
      {
        ijkp = i + j*jjp + k*kkp;
        ijk  = i+igc + (j+jgc)*jj + (k+kgc)*kk;
        p[ijkp] = ( (ut[ijk+ii] + u[ijk+ii] / dt) - (ut[ijk] + u[ijk] / dt) ) * dxi
                + ( (vt[ijk+jj] + v[ijk+jj] / dt) - (vt[ijk] + v[ijk] / dt) ) * dyi
                + ( (wt[ijk+kk] + w[ijk+kk] / dt) - (wt[ijk] + w[ijk] / dt) ) * dzi[k+kgc];
      }

  return 0;
}

int cpres_g2::pres_solve(double * restrict p, double * restrict work3d, double * restrict b, double * restrict dz,
                         double * restrict fftini, double * restrict fftouti, 
                         double * restrict fftinj, double * restrict fftoutj)

{
  int i,j,k,jj,kk,ijk;
  int imax,jmax,kmax;
  int itot,jtot;
  int iblock,jblock,kblock;
  int igc,jgc,kgc;
  int iindex,jindex;

  imax   = grid->imax;
  jmax   = grid->jmax;
  kmax   = grid->kmax;
  itot   = grid->itot;
  jtot   = grid->jtot;
  iblock = grid->iblock;
  jblock = grid->jblock;
  kblock = grid->kblock;
  igc    = grid->igc;
  jgc    = grid->jgc;
  kgc    = grid->kgc;

  // transpose the pressure field
  grid->transposezx(work3d,p);
  mpi->waitall();

  jj = itot;
  kk = itot*jmax;

  // do the first fourier transform
  for(int k=0; k<kblock; k++)
    for(int j=0; j<jmax; j++)
    {
#pragma ivdep
      for(int i=0; i<itot; i++)
      { 
        ijk = i + j*jj + k*kk;
        fftini[i] = work3d[ijk];
      }

      fftw_execute(iplanf);

#pragma ivdep
      for(int i=0; i<itot; i++)
      {
        ijk = i + j*jj + k*kk;
        work3d[ijk] = fftouti[i];
      }
    }

  // transpose again
  grid->transposexy(p,work3d);
  mpi->waitall();

  jj = iblock;
  kk = iblock*jtot;

  // do the second fourier transform
  for(int k=0; k<kblock; k++)
    for(int i=0; i<iblock; i++)
    {
      for(int j=0; j<jtot; j++)
      { 
        ijk = i + j*jj + k*kk;
        fftinj[j] = p[ijk];
      }

      fftw_execute(jplanf);

      for(int j=0; j<jtot; j++)
      {
        ijk = i + j*jj + k*kk;
        // shift to use p in pressure solver
        work3d[ijk] = fftoutj[j];
      }
    }

  // transpose back to original orientation
  grid->transposeyz(p,work3d);
  mpi->waitall();

  jj = iblock;
  kk = iblock*jblock;

  // solve the tridiagonal system
  // create vectors that go into the tridiagonal matrix solver
  for(k=0; k<kmax; k++)
    for(j=0; j<jblock; j++)
#pragma ivdep
      for(i=0; i<iblock; i++)
      {
        // swap the mpicoords, because domain is turned 90 degrees to avoid two mpi transposes
        iindex = mpi->mpicoordy * iblock + i;
        jindex = mpi->mpicoordx * jblock + j;

        ijk  = i + j*jj + k*kk;
        b[ijk] = dz[k+kgc]*dz[k+kgc] * (bmati[iindex]+bmatj[jindex]) - (a[k]+c[k]);
        p[ijk] = dz[k+kgc]*dz[k+kgc] * p[ijk];
      }

  for(j=0; j<jblock; j++)
#pragma ivdep
    for(i=0; i<iblock; i++)
    {
      iindex = mpi->mpicoordy * iblock + i;
      jindex = mpi->mpicoordx * jblock + j;

      // substitute BC's
      ijk = i + j*jj;
      b[ijk] += a[0];

      // for wave number 0, which contains average, set pressure at top to zero
      ijk  = i + j*jj + (kmax-1)*kk;
      if(iindex == 0 && jindex == 0)
        b[ijk] -= c[kmax-1];
      // set dp/dz at top to zero
      else
        b[ijk] += c[kmax-1];
    }

  // call tdma solver
  tdma(a, b, c, p, work2d, work3d);
        
  // transpose back to y
  grid->transposezy(work3d, p);
  mpi->waitall();
  
  jj = iblock;
  kk = iblock*jtot;

  // transform the second transform back
  for(int k=0; k<kblock; k++)
    for(int i=0; i<iblock; i++)
    {
      for(int j=0; j<jtot; j++)
      { 
        ijk = i + j*jj + k*kk;
        fftinj[j] = work3d[ijk];
      }

      fftw_execute(jplanb);

      for(int j=0; j<jtot; j++)
      {
        ijk = i + j*jj + k*kk;
        p[ijk] = fftoutj[j] / jtot;
      }
    }

  // transpose back to x
  grid->transposeyx(work3d, p);
  mpi->waitall();
    
  jj = itot;
  kk = itot*jmax;

  // transform the first transform back
  for(int k=0; k<kblock; k++)
    for(int j=0; j<jmax; j++)
    {
#pragma ivdep
      for(int i=0; i<itot; i++)
      { 
        ijk = i + j*jj + k*kk;
        fftini[i] = work3d[ijk];
      }

      fftw_execute(iplanb);

#pragma ivdep
      for(int i=0; i<itot; i++)
      {
        ijk = i + j*jj + k*kk;
        // swap array here to avoid unncessary 3d loop
        p[ijk] = fftouti[i] / itot;
      }
    }

  // and transpose back...
  grid->transposexz(work3d, p);
  mpi->waitall();

  jj = imax;
  kk = imax*jmax;

  int ijkp,jjp,kkp;
  jjp = grid->icells;
  kkp = grid->icells*grid->jcells;

  // put the pressure back onto the original grid including ghost cells
  for(int k=0; k<grid->kmax; k++)
    for(int j=0; j<grid->jmax; j++)
#pragma ivdep
      for(int i=0; i<grid->imax; i++)
      {
        ijkp = i+igc + (j+jgc)*jjp + (k+kgc)*kkp;
        ijk  = i + j*jj + k*kk;
        p[ijkp] = work3d[ijk];
      }

  // set the boundary conditions
  // set a zero gradient boundary at the bottom
  for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
    for(int i=grid->istart; i<grid->iend; i++)
    {
      ijk = i + j*jjp + grid->kstart*kkp;
      p[ijk-kkp] = p[ijk];
    }

  // set the cyclic boundary conditions
  grid->boundary_cyclic(p);
  mpi->waitall();

  return 0;
}

int cpres_g2::pres_out(double * restrict ut, double * restrict vt, double * restrict wt, 
                       double * restrict p , double * restrict dzhi)
{
  int    ijk,ii,jj,kk;
  double dxi,dyi;

  ii = 1;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  dxi = 1./grid->dx;
  dyi = 1./grid->dy;

  for(int k=grid->kstart; k<grid->kend; k++)
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        ut[ijk] -= (p[ijk] - p[ijk-ii]) * dxi;
        vt[ijk] -= (p[ijk] - p[ijk-jj]) * dyi;
        wt[ijk] -= (p[ijk] - p[ijk-kk]) * dzhi[k];
      }

  return 0;
}

// tridiagonal matrix solver, taken from Numerical Recipes, Press
int cpres_g2::tdma(double * restrict a, double * restrict b, double * restrict c, 
                double * restrict p, double * restrict work2d, double * restrict work3d)
                
{
  int i,j,k,jj,kk,ijk,ij;
  int iblock,jblock,kmax;

  iblock = grid->iblock;
  jblock = grid->jblock;
  kmax = grid->kmax;

  jj = iblock;
  kk = iblock*jblock;

  for(j=0;j<jblock;j++)
#pragma ivdep
    for(i=0;i<iblock;i++)
    {
      ij = i + j*jj;
      work2d[ij] = b[ij];
    }

  for(j=0;j<jblock;j++)
#pragma ivdep
    for(i=0;i<iblock;i++)
    {
      ij = i + j*jj;
      p[ij] /= work2d[ij];
    }

  for(k=1; k<kmax; k++)
  {
    for(j=0;j<jblock;j++)
#pragma ivdep
      for(i=0;i<iblock;i++)
      {
        ij  = i + j*jj;
        ijk = i + j*jj + k*kk;
        work3d[ijk] = c[k-1] / work2d[ij];
      }
    for(j=0;j<jblock;j++)
#pragma ivdep
      for(i=0;i<iblock;i++)
      {
        ij  = i + j*jj;
        ijk = i + j*jj + k*kk;
        work2d[ij] = b[ijk] - a[k]*work3d[ijk];
      }
    for(j=0;j<jblock;j++)
#pragma ivdep
      for(i=0;i<iblock;i++)
      {
        ij  = i + j*jj;
        ijk = i + j*jj + k*kk;
        p[ijk] -= a[k]*p[ijk-kk];
        p[ijk] /= work2d[ij];
      }
  }

  for(k=kmax-2; k>=0; k--)
    for(j=0;j<jblock;j++)
#pragma ivdep
      for(i=0;i<iblock;i++)
      {
        ijk = i + j*jj + k*kk;
        p[ijk] -= work3d[ijk+kk]*p[ijk+kk];
      }

  return 0;
}

double cpres_g2::calcdivergence(double * restrict u, double * restrict v, double * restrict w, double * restrict dzi)
{
  int    ijk,ii,jj,kk;
  double dxi,dyi;

  ii = 1;
  jj = grid->icells;
  kk = grid->icells*grid->jcells;

  dxi = 1./grid->dx;
  dyi = 1./grid->dy;

  double div, divmax;
  div    = 0;
  divmax = 0;

  for(int k=grid->kstart; k<grid->kend; k++)
    for(int j=grid->jstart; j<grid->jend; j++)
#pragma ivdep
      for(int i=grid->istart; i<grid->iend; i++)
      {
        ijk = i + j*jj + k*kk;
        div = (u[ijk+ii]-u[ijk])*dxi + (v[ijk+jj]-v[ijk])*dyi + (w[ijk+kk]-w[ijk])*dzi[k];

        divmax = std::max(divmax, std::abs(div));
      }

  grid->getmax(&divmax);

  return divmax;
}
