// Copyright (c) 2017, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory (LLNL).
// LLNL-CODE-742473. All rights reserved.
//
// This file is part of HiOp. For details, see https://github.com/LLNL/hiop. HiOp 
// is released under the BSD 3-clause license (https://opensource.org/licenses/BSD-3-Clause). 
// Please also read “Additional BSD Notice” below.
//
// Redistribution and use in source and binary forms, with or without modification, 
// are permitted provided that the following conditions are met:
// i. Redistributions of source code must retain the above copyright notice, this list 
// of conditions and the disclaimer below.
// ii. Redistributions in binary form must reproduce the above copyright notice, 
// this list of conditions and the disclaimer (as noted below) in the documentation and/or 
// other materials provided with the distribution.
// iii. Neither the name of the LLNS/LLNL nor the names of its contributors may be used to 
// endorse or promote products derived from this software without specific prior written 
// permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
// SHALL LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR 
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
// AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, 
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Additional BSD Notice
// 1. This notice is required to be provided under our contract with the U.S. Department 
// of Energy (DOE). This work was produced at Lawrence Livermore National Laboratory under 
// Contract No. DE-AC52-07NA27344 with the DOE.
// 2. Neither the United States Government nor Lawrence Livermore National Security, LLC 
// nor any of their employees, makes any warranty, express or implied, or assumes any 
// liability or responsibility for the accuracy, completeness, or usefulness of any 
// information, apparatus, product, or process disclosed, or represents that its use would
// not infringe privately-owned rights.
// 3. Also, reference herein to any specific commercial products, process, or services by 
// trade name, trademark, manufacturer or otherwise does not necessarily constitute or 
// imply its endorsement, recommendation, or favoring by the United States Government or 
// Lawrence Livermore National Security, LLC. The views and opinions of authors expressed 
// herein do not necessarily state or reflect those of the United States Government or 
// Lawrence Livermore National Security, LLC, and shall not be used for advertising or 
// product endorsement purposes.

/**
 * @file hiopDualsUpdater.cpp
 *
 * @author Cosmin G. Petra <petra1@llnl.gov>,  LLNL
 * @author Nai-Yuan Chiang <chiang7@llnl.gov>,  LLNL
 *
 */
 
#include "hiopDualsUpdater.hpp"
#include "hiopLinAlgFactory.hpp"

#include "hiop_blasdefs.hpp"

#ifdef HIOP_SPARSE
#ifdef HIOP_USE_COINHSL
#include "hiopLinSolverIndefSparseMA57.hpp"
#endif
#ifdef HIOP_USE_STRUMPACK
#include "hiopLinSolverSparseSTRUMPACK.hpp"
#endif
#endif

namespace hiop
{

hiopDualsLsqUpdate::hiopDualsLsqUpdate(hiopNlpFormulation* nlp) 
  : hiopDualsUpdater(nlp), 
    mexme_(nullptr), mexmi_(nullptr), mixmi_(nullptr), mxm_(nullptr), 
    M_(nullptr),
    rhs_(nullptr), rhsc_(nullptr), rhsd_(nullptr),
    vec_n_(nullptr),vec_mi_(nullptr),
    linSys_(nullptr),
    lsq_dual_init_max_(1e3),
    augsys_update_(false) 
{
  hiopNlpSparse* nlpsp = dynamic_cast<hiopNlpSparse*>(nlp_);
  if(nullptr!=nlpsp)
  {
#ifndef HIOP_SPARSE
    assert(0 && "should not reach here!");
#endif // HIOP_SPARSE
    augsys_update_ = true;
    rhs_ = LinearAlgebraFactory::createVector(nlp_->n() + nlp_->m_ineq() + nlp_->m());
  } else {
    mexme_ = LinearAlgebraFactory::createMatrixDense(nlp_->m_eq(),   nlp_->m_eq());
    mexmi_ = LinearAlgebraFactory::createMatrixDense(nlp_->m_eq(),   nlp_->m_ineq());
    mixmi_ = LinearAlgebraFactory::createMatrixDense(nlp_->m_ineq(), nlp_->m_ineq());
    mxm_   = LinearAlgebraFactory::createMatrixDense(nlp_->m(), nlp_->m());

    M_      = LinearAlgebraFactory::createMatrixDense(nlp_->m(), nlp_->m());
    rhs_    = LinearAlgebraFactory::createVector(nlp_->m());
#ifdef HIOP_DEEPCHECKS
    M_copy_ = M_->alloc_clone(); 
    rhs_copy_ = rhs_->alloc_clone(); 
    mixme_ = LinearAlgebraFactory::createMatrixDense(nlp_->m_ineq(), nlp_->m_eq());
#endif
  }

  vec_n_ = nlp_->alloc_primal_vec();
  rhsc_ = nlp_->alloc_dual_eq_vec(); 
  rhsd_ = nlp_->alloc_dual_ineq_vec();
  vec_mi_ = rhsd_->alloc_clone(); 
  
  rhsc_->setToZero();
  rhsd_->setToZero();

  //user options
  recalc_lsq_duals_tol = 1e-6;
};

hiopDualsLsqUpdate::~hiopDualsLsqUpdate()
{
  if(linSys_) delete linSys_;
  if(mexme_) delete mexme_;
  if(mexmi_) delete mexmi_;
  if(mixmi_) delete mixmi_;
  if(mxm_) delete mxm_;
  if(M_) delete M_;
  if(rhs_) delete rhs_;
  if(rhsc_) delete rhsc_; 
  if(rhsd_) delete rhsd_;
  if(vec_n_) delete vec_n_;
  if(vec_mi_) delete vec_mi_;
#ifdef HIOP_DEEPCHECKS
  if(M_copy_) delete M_copy_;
  if(rhs_copy_) delete rhs_copy_;
  if(mixme_) delete mixme_;
#endif
}

bool hiopDualsLsqUpdate::
go(const hiopIterate& iter,  hiopIterate& iter_plus,
   const double& f, const hiopVector& c, const hiopVector& d,
   const hiopVector& grad_f, const hiopMatrix& jac_c, const hiopMatrix& jac_d,
   const hiopIterate& search_dir, const double& alpha_primal, const double& alpha_dual,
   const double& mu, const double& kappa_sigma, const double& infeas_nrm_trial)
{
  hiopNlpDenseConstraints* nlpd = dynamic_cast<hiopNlpDenseConstraints*>(nlp_);
  assert(nullptr!=nlpd);

  //first update the duals using steplength along the search directions. This is fine for 
  //signed duals z_l, z_u, v_l, and v_u. The rest of the duals, yc and yd, will be found as a 
  //solution to the above LSQ problem
  if(!iter_plus.takeStep_duals(iter, search_dir, alpha_primal, alpha_dual)) {
    nlp_->log->printf(hovError, "dual lsq update: error in standard update of the duals");
    return false;
  }
  if(!iter_plus.adjustDuals_primalLogHessian(mu,kappa_sigma)) {
    nlp_->log->printf(hovError, "dual lsq update: error in adjustDuals");
    return false;
  }


  //return if the constraint violation (primal infeasibility) is not below the tol for the LSQ update
  if(infeas_nrm_trial>recalc_lsq_duals_tol) {
    nlp_->log->printf(hovScalars,
                      "will not perform the dual lsq update since the primal infeasibility (%g) "
                      "is not under the tolerance recalc_lsq_duals_tol=%g.\n",
                      infeas_nrm_trial, recalc_lsq_duals_tol);
    return true;
  }

  return LSQUpdate(iter_plus, grad_f, jac_c, jac_d);
};

/** Given xk, zk_l, zk_u, vk_l, and vk_u (contained in 'iter'), this method solves an LSQ problem 
 * corresponding to dual infeasibility equation
 *    min_{y_c,y_d} ||  \nabla f(xk) + J^T_c(xk) y_c + J_d^T(xk) y_d - zk_l+zk_u  ||^2
 *                  || - y_d - vk_l + vk_u                                        ||_2,
 *  which is
 *   min_{y_c, y_d} || [ J_c^T  J_d^T ] [ y_c ]  -  [ -\nabla f(xk) + zk_l-zk_u ]  ||^2
 *                  || [  0       I   ] [ y_d ]     [ - vk_l + vk_u             ]  ||_2
 * ******************************
 * NLPs with dense constraints 
 * ******************************
 * For NLPs with dense constraints, the above LSQ problem is solved by solving the linear 
 *  system in y_c and y_d:
 *   [ J_c J_c^T    J_c J_d^T     ] [ y_c ]  =  [ J_c   0 ] [ -\nabla f(xk) + zk_l-zk_u ] 
 *   [ J_d J_c^T    J_d J_d^T + I ] [ y_d ]     [ J_d   I ] [ - vk_l + vk_u             ]
 * This linear system is small (of size m=m_E+m_I) (so it is replicated for all MPI ranks).
 * 
 * The matrix of the above system is stored in the member variable M_ of this class and the
 *  right-hand side in 'rhs_'.
 * 
 * **************
 * MDS NLPs
 * **************
 * For MDS NLPs, the linear system exploits the block structure of the Jacobians Jc and Jd. 
 * Namely, since Jc = [Jxdc  Jxsc] and Jd = [Jxdd  Jxsd], the following
 * dense linear system is to be solved for y_c and y_d
 *
 *    [ Jxdc Jxdc^T + Jxsc Jxsc^T   Jxdc Jxdd^T + Jxsc Jxsd^T     ] [ y_c ] = same rhs as
 *    [ Jxdd Jxdc^T + Jxsd Jxsc^T   Jxdd Jxdd^T + Jxsd Jxsd^T + I ] [ y_d ]     above
 * The above linear system is solved as a dense linear system. 
 *
 * ***********************
 * Sparse (general) NLPs
 * ***********************
 * For NLPs with sparse inputs, the corresponding LSQ problem is solved in augmeted system:
 * [    I    0     Jc^T  Jd^T  ] [ dx]      [ \nabla f(xk) - zk_l + zk_u  ]
 * [    0    I     0     -I    ] [ dd]      [        -vk_l + vk_u         ]
 * [    Jc   0     0     0     ] [dyc] =  - [             0               ]
 * [    Jd   -I    0     0     ] [dyd]      [             0               ]
 *
 * The matrix of the above system is stored in the member variable M_ of this class and the
 * right-hand side in 'rhs'.  * 
 */
bool hiopDualsLsqUpdate::
LSQUpdate(hiopIterate& iter, const hiopVector& grad_f, const hiopMatrix& jac_c, const hiopMatrix& jac_d)
{
  hiopMatrixDense* M = dynamic_cast<hiopMatrixDense*>(M_);
  hiopMatrixDense* mexme = dynamic_cast<hiopMatrixDense*>(mexme_);
  hiopMatrixDense* mexmi = dynamic_cast<hiopMatrixDense*>(mexmi_);
  hiopMatrixDense* mixmi = dynamic_cast<hiopMatrixDense*>(mixmi_);

  //compute terms in M: Jc * Jc^T, J_c * J_d^T, and J_d * J_d^T
  //! streamline the communication (use mxm as a global buffer for the MPI_Allreduce)
  jac_c.timesMatTrans(0.0, *mexme, 1.0, jac_c);
  jac_c.timesMatTrans(0.0, *mexmi, 1.0, jac_d);
  jac_d.timesMatTrans(0.0, *mixmi, 1.0, jac_d);
  mixmi->addDiagonal(1.0);

  M->copyBlockFromMatrix(0,0,*mexme);
  M->copyBlockFromMatrix(0, nlp_->m_eq(), *mexmi);
  M->copyBlockFromMatrix(nlp_->m_eq(),nlp_->m_eq(), *mixmi);

#ifdef HIOP_DEEPCHECKS
  hiopMatrixDense* mixme = dynamic_cast<hiopMatrixDense*>(mixme_);
  hiopMatrixDense* M_copy = dynamic_cast<hiopMatrixDense*>(M_copy_);
  M_copy->copyFrom(*M);
  jac_d.timesMatTrans(0.0, *mixme, 1.0, jac_c);
  M_copy->copyBlockFromMatrix(nlp_->m_eq(), 0, *mixme);
  M_copy->assertSymmetry(1e-12);
#endif

  //bailout in case there is an error in the Cholesky factorization
  int info;
  if((info=this->factorizeMat(*M))) {
    nlp_->log->printf(hovError, "dual lsq update: error %d in the Cholesky factorization.\n", info);
    return false;
  }

  // compute rhs_=[rhsc_,rhsd_]. 
  // [ rhsc_ ] = - [ J_c   0 ] [ vecx ] 
  // [ rhsd_ ]     [ J_d   I ] [ vecd ]
  // [vecx,vecd] = - [ -\nabla f(xk) + zk_l-zk_u, - vk_l + vk_u]. 
  hiopVector& vecx = *vec_n_;
  vecx.copyFrom(grad_f);
  vecx.axpy(-1.0, *iter.get_zl());
  vecx.axpy( 1.0, *iter.get_zu());
  hiopVector& vecd = *vec_mi_;
  vecd.copyFrom(*iter.get_vl());
  vecd.axpy(-1.0, *iter.get_vu());
  jac_c.timesVec(0.0, *rhsc_, -1.0, vecx);
  jac_d.timesVec(0.0, *rhsd_, -1.0, vecx);
  rhsd_->axpy(-1.0, vecd);
  rhs_->copyFromStarting(0, *rhsc_);
  rhs_->copyFromStarting(nlp_->m_eq(), *rhsd_);
  //nlp_->log->write("rhs_", *rhs_, hovSummary);
#ifdef HIOP_DEEPCHECKS
  rhs_copy_->copyFrom(*rhs_);
#endif
  //solve for this rhs_
  if((info=this->solveWithFactors(*M, *rhs_))) {
    nlp_->log->printf(hovError, "dual lsq update: error %d in the solution process.\n", info);
    return false;
  }

  //update yc and yd in iter_plus
  rhs_->copyToStarting(0, *iter.get_yc());
  rhs_->copyToStarting(nlp_->m_eq(), *iter.get_yd());

#ifdef HIOP_DEEPCHECKS
  double nrmrhs = rhs_copy_->twonorm();
  M_copy_->timesVec(-1.0,  *rhs_copy_, 1.0, *rhs_);
  double nrmres = rhs_copy_->twonorm() / (1+nrmrhs);
  if(nrmres>1e-4) {
    nlp_->log->printf(hovError,
                      "hiopDualsLsqUpdate::LSQUpdate linear system residual is dangerously high: %g\n",
                      nrmres);
    assert(false && "hiopDualsLsqUpdate::LSQUpdate linear system residual is dangerously high");
    return false;
  } else {
    if(nrmres>1e-6)
      nlp_->log->printf(hovWarning,
                        "hiopDualsLsqUpdate::LSQUpdate linear system residual is dangerously high: %g\n",
                        nrmres);
  }
#endif

  //nlp_->log->write("yc ini", *iter.get_yc(), hovSummary);
  //nlp_->log->write("yd ini", *iter.get_yd(), hovSummary);
  return true;
};

int hiopDualsLsqUpdate::factorizeMat(hiopMatrixDense& M)
{
#ifdef HIOP_DEEPCHECKS
  assert(M.m()==M.n());
#endif
  if(M.m()==0) return 0;
  char uplo='L'; int N=M.n(), lda=N, info;
  DPOTRF(&uplo, &N, M.local_data(), &lda, &info);
  if(info>0) {
    nlp_->log->printf(hovError,
                      "hiopKKTLinSysLowRank::factorizeMat: dpotrf (Chol fact) detected "
                      "%d minor being indefinite.\n", info);
  } else {
    if(info<0) { 
      nlp_->log->printf(hovError, "hiopKKTLinSysLowRank::factorizeMat: dpotrf returned error %d\n", info);
    }
  }
  assert(info==0);
  return info;
}

int hiopDualsLsqUpdate::solveWithFactors(hiopMatrixDense& M, hiopVector& r)
{
#ifdef HIOP_DEEPCHECKS
  assert(M.m()==M.n());
#endif
  if(M.m()==0) return 0;
  char uplo='L'; //we have upper triangular in C++, but this is lower in fortran
  int N=M.n(), lda=N, nrhs=1, info;
  DPOTRS(&uplo,&N, &nrhs, M.local_data(), &lda, r.local_data(), &lda, &info);
  if(info<0) 
    nlp_->log->printf(hovError, "hiopKKTLinSysLowRank::solveWithFactors: dpotrs returned error %d\n", info);
#ifdef HIOP_DEEPCHECKS
  assert(info<=0);
#endif
  return info;
}

bool hiopDualsLsqUpdate::LSQInitDualSparse(hiopIterate& iter,
                                           const hiopVector& grad_f,
                                           const hiopMatrix& jac_c,
                                           const hiopMatrix& jac_d)
{
  hiopNlpSparse* nlpsp = dynamic_cast<hiopNlpSparse*>(nlp_);
  assert(nullptr!=nlpsp);
  
  const hiopMatrixSparse& Jac_cSp = dynamic_cast<const hiopMatrixSparse&>(jac_c);
  const hiopMatrixSparse& Jac_dSp = dynamic_cast<const hiopMatrixSparse&>(jac_d);

  int nx = Jac_cSp.n(), nd=Jac_dSp.m(), neq=Jac_cSp.m(), nineq=Jac_dSp.m();
  int n = nx + nineq + neq + nineq; 

  int nnz = nx + nd + Jac_cSp.numberOfNonzeros() + Jac_dSp.numberOfNonzeros() + nd + (nx + nd + neq + nineq);
  
  if(!linSys_) {
    if(nlp_->options->GetString("compute_mode")=="cpu") {
      nlp_->log->printf(hovSummary,
                        "LSQ Dual Initialization --- KKT_SPARSE_XYcYd linsys: MA57 size %d (%d cons)\n",
                        n, neq+nineq);
#ifdef HIOP_USE_COINHSL
      linSys_ = new hiopLinSolverIndefSparseMA57(n, nnz, nlp_);
#endif // HIOP_USE_COINHSL          
    } else {
#ifdef HIOP_USE_STRUMPACK        
      hiopLinSolverIndefSparseSTRUMPACK *p = new hiopLinSolverIndefSparseSTRUMPACK(n, nnz, nlp_);
      
      nlp_->log->printf(hovSummary,
                        "LSQ Dual Initialization --- KKT_SPARSE_XDYcYd linsys: using STRUMPACK as an "
                        "indefinite solver, size %d (%d cons) (safe_mode=%d)\n",
                        n, neq+nineq);
      
      p->setFakeInertia(neq + nineq);
      linSys_ = p;
#else
#ifdef HIOP_USE_COINHSL
      nlp_->log->printf(hovSummary,
                        "LSQ Dual Initialization --- KKT_SPARSE_XDYcYd linsys: using MA57 on CPU size "
                        "%d (%d cons)\n",
                        n, neq+nineq);                             
      linSys_ = new hiopLinSolverIndefSparseMA57(n, nnz, nlp_);
#endif // HIOP_USE_COINHSL
#endif // HIOP_USE_STRUMPACK
    }
  }
  
  hiopLinSolverIndefSparse* linSys = dynamic_cast<hiopLinSolverIndefSparse*> (linSys_);
  assert(linSys);

  hiopMatrixSparseTriplet& Msys = linSys->sysMatrix();
  // update linSys system matrix
  {
    Msys.setToZero();

    // copy Jac and Hes to the full iterate matrix
    long long dest_nnz_st{0};
    Msys.copyDiagMatrixToSubblock(1., 0, 0, dest_nnz_st, nx+nd);
    dest_nnz_st += nx+nd;
    Msys.copyRowsBlockFrom(Jac_cSp, 0,   neq,    nx+nd,      dest_nnz_st);
    dest_nnz_st += Jac_cSp.numberOfNonzeros();
    Msys.copyRowsBlockFrom(Jac_dSp, 0,   nineq,  nx+nd+neq,  dest_nnz_st);
    dest_nnz_st += Jac_dSp.numberOfNonzeros();

    // minus identity matrix for slack variables
    Msys.copyDiagMatrixToSubblock(-1., nx+nd+neq, nx, dest_nnz_st, nineq);
    dest_nnz_st += nineq;

    //add 0.0 to diagonal block linSys starting at (0,0)
    Msys.setSubDiagonalTo(0, nx+nd+neq+nineq, 0.0, dest_nnz_st);
    dest_nnz_st += nx+nd+neq+nineq;
          
    /* we've just done
    *
    * [    I    0     Jc^T  Jd^T  ] [ dx]   [ rx_tilde ]
    * [    0    I     0     -I    ] [ dd]   [ rd_tilde ]
    * [    Jc   0     0     0     ] [dyc] = [   ryc    ]
    * [    Jd   -I    0     0     ] [dyd]   [   ryd    ]
    */
    nlp_->log->write("LSQ Dual Initialization --- KKT_SPARSE_XDYcYd linsys:", Msys, hovMatrices);
  }

  int ret_val = linSys->matrixChanged();

  if(ret_val<0) {
    nlp_->log->printf(hovError, "dual lsq update: error %d in the factorization.\n", ret_val);
    return false;
  } 
  
  // compute rhs_=[rhsx, rhss, rhsc_, rhsd_]. 
  // rhsx = - [ \nabla f(xk) - zk_l + zk_u  ]
  // rhss = - [ -vk_l + vk_u ]
  // rhsc_ = rhsd_ = 0
  hiopVector& rhsx = *vec_n_;
  rhsx.copyFrom(grad_f);
  rhsx.negate();
  rhsx.axpy( 1.0, *iter.get_zl());
  rhsx.axpy(-1.0, *iter.get_zu());

  hiopVector& rhss = *vec_mi_;
  rhss.copyFrom(*iter.get_vl());
  rhss.axpy(-1.0, *iter.get_vu());

  rhs_->copyFromStarting(0, rhsx);
  rhs_->copyFromStarting(nx, rhss);
  rhs_->copyFromStarting(nx+nd, *rhsc_);
  rhs_->copyFromStarting(nx+nd+neq, *rhsd_);

  //solve for this rhs_
  bool linsol_ok = linSys_->solve(*rhs_);
  if(!linsol_ok)
  {
    nlp_->log->printf(hovWarning, "dual lsq update: error in the solution process.\n");
    iter.get_yc()->setToZero();
    iter.get_yd()->setToZero();
    return true;
  }

  //update yc and yd in iter_plus
  rhs_->copyToStarting(nx+nd, *iter.get_yc());
  rhs_->copyToStarting(nx+nd+neq, *iter.get_yd());
  
  double ycnrm = iter.get_yc()->infnorm();
  double ydnrm = iter.get_yd()->infnorm();
  double ynrm = (ycnrm>ydnrm)?ycnrm:ydnrm;
  if( ynrm > lsq_dual_init_max_  )
  {
    iter.get_yc()->setToZero();
    iter.get_yd()->setToZero();
  }

  //nlp_->log->write("yc ini", *iter.get_yc(), hovSummary);
  //nlp_->log->write("yd ini", *iter.get_yd(), hovSummary);
  return true;
};

}; //~ end of namespace
