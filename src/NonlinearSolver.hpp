/*BHEADER**********************************************************************
 *
 * Copyright (c) 2018, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * LLNL-CODE-745247. All Rights reserved. See file COPYRIGHT for details.
 *
 * This file is part of smoothG. For more information and source code
 * availability, see https://www.github.com/llnl/smoothG.
 *
 * smoothG is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 ***********************************************************************EHEADER*/

/** @file

    @brief Contains class NonlinearMG
*/

#ifndef __NONLINEARMG_HPP__
#define __NONLINEARMG_HPP__

#include "utilities.hpp"
#include "Hierarchy.hpp"

namespace smoothg
{

/// Linearization method
enum Linearization { Newton, Picard };

/// Parameter list for abstract nonlinear solver
struct NLSolverParameters
{
    int print_level = 0;
    int max_num_iter = 50;
    double rtol = 1e-8;
    double atol = 1e-10;

    bool check_converge = true;
    Linearization linearization = Newton;
    int num_backtrack = 0;
    double diff_tol = -1.0;
    double init_linear_tol = 1e-8;
};

/**
   @brief Abstract iterative solver class for nonlinear problems.

       This class takes care of nonlinear iterations; computation of nonlinear
       residual and iteration step need to be defined in derived class.
*/
class NonlinearSolver
{
public:
    /// Constructor
    NonlinearSolver(MPI_Comm comm, NLSolverParameters param);

    /// Solve A(sol) = rhs
    void Solve(const mfem::Vector& rhs, mfem::Vector& sol);

    ///@name Set solver parameters
    ///@{
    void SetPrintLevel(int print_level) { param_.print_level = print_level; }
    void SetMaxIter(int max_num_iter) { param_.max_num_iter = max_num_iter; }
    void SetRelTol(double rtol) { param_.rtol = rtol; }
    void SetAbsTol(double atol) { param_.atol = atol; }
    ///@}

    ///@name Get results of iterative solve
    ///@{
    int GetNumIterations() const { return iter_; }
    double GetTiming() const { return timing_; }
    bool IsConverged() const { return converged_; }
    ///@}
protected:
    /// Update linear tolerance based on choice 2 in Eisenstat & Walker, SISC 1996
    void UpdateLinearSolveTol();

    /// @return || A(sol) - rhs ||
    virtual double ResidualNorm(const mfem::Vector& sol, const mfem::Vector& rhs) = 0;

    virtual void IterationStep(const mfem::Vector& x, mfem::Vector& y) = 0;

    int iter_;
    double timing_;
    bool converged_;

    MPI_Comm comm_;
    int myid_;
    std::string tag_;

    double adjusted_tol_;  // max(atol_, rtol_ * || rhs ||)
    double rhs_norm_;
    double resid_norm_;
    double prev_resid_norm_;
    double linear_tol_;

    NLSolverParameters param_;
};

enum Cycle { V_CYCLE, FMG };

struct FASParameters
{
    int num_levels = 1;             // number of multigrid levels
    Cycle cycle = V_CYCLE;          // multigrid cycle type
    double coarse_correct_tol;      // no coarse correction if rel resid < tol
    NLSolverParameters nl_solve;    // for FAS itself as a nonlinear solver
    NLSolverParameters fine;        // for finest level nonlinear solve
    NLSolverParameters mid;         // for intermediate levels nonlinear solves
    NLSolverParameters coarse;      // for coarsest level nonlinear solve
};

/**
   @brief Nonlinear multigrid solver using full approximation scheme.

       Abstract class for FAS. Operations like smoothing, interpolation,
       restriction, projection, etc. need to be provided.
*/
class FAS : public NonlinearSolver
{
public:
    /// Constructor
    FAS(MPI_Comm comm, FASParameters param);
protected:
    void MG_Cycle(int level);

    void IterationStep(const mfem::Vector& rhs, mfem::Vector& sol) override;

    double ResidualNorm(const mfem::Vector& sol, const mfem::Vector& rhs) override;

    /// Restrict a vector from level to level+1 (coarser level)
    virtual void Restrict(int level, const mfem::Vector& fine, mfem::Vector& coarse) const = 0;

    /// Interpolate a vector from level to level-1 (finer level)
    virtual void Interpolate(int level, const mfem::Vector& coarse, mfem::Vector& fine) const = 0;

    /// Project a vector from level to level+1 (coarser level)
    virtual void Project(int level, const mfem::Vector& fine, mfem::Vector& coarse) const = 0;

    /// Relaxation on each level
    virtual void Smoothing(int level, const mfem::Vector& in, mfem::Vector& out) = 0;

    /// Backtracking on each level
    virtual void BackTracking(int level, const mfem::Vector& rhs, double prev_resid_norm,
                              mfem::Vector& x, mfem::Vector& dx) = 0;

    /// Evaluates residual resid = A[level](sol) - rhs
    virtual mfem::Vector ComputeResidual(int level, const mfem::Vector& sol,
                                         const mfem::Vector& rhs) = 0;

    /// Evaluates residual norm on each level
    virtual double ResidualNorm(int level, const mfem::Vector& resid) = 0;

    std::vector<mfem::Vector> rhs_;
    std::vector<mfem::Vector> sol_;
    mutable std::vector<mfem::Vector> help_;
    std::vector<double> resid_norms_;

    FASParameters param_;
};

} // namespace smoothg

#endif /* __NONLINEARMG_HPP__ */
