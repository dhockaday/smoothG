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

/**
   @file nldarcy.cpp
   @brief nonlinear Darcy's problem.
*/

#include <fstream>
#include <sstream>
#include <mpi.h>

#include "pde.hpp"

using namespace smoothg;

using std::unique_ptr;

/**
   @brief Nonlinear elliptic problem

   Given \f$f \in L^2(\Omega)\f$, \f$k(p)\f$ a differentiable function of p,
   find \f$p\f$ such that \f$-div(k_0k(p)\nabla p) = f\f$.
*/

class LevelSolver : public NonlinearSolver
{
public:
    /**
       @todo take Kappa(p) as input
    */
    LevelSolver(Hierarchy& hierarchy, int level,
                mfem::Vector Z_vector, NLMGParameter param);

    // Compute Ax = A(x) - b.
    virtual void Mult(const mfem::Vector& x, mfem::Vector& Ax);

    double GetLinearResidualNorm() const { return linear_resid_norm_; }

    void BackTracking(const mfem::Vector &rhs,  double prev_resid_norm,
                      mfem::Vector& x, mfem::Vector& dx, bool interlopate=false);
private:
    void EvalCoef(const mfem::Vector& sol_block1);
    void EvalCoefDerivative(const mfem::Vector& sol_block1);
    void PicardStep(const mfem::BlockVector& rhs, mfem::BlockVector& x);
    void NewtonStep(const mfem::BlockVector& rhs, mfem::BlockVector& x);
    void Build_dMdp(const mfem::BlockVector& iterate);

    virtual void IterationStep(const mfem::Vector& rhs, mfem::Vector& sol);

    virtual mfem::Vector AssembleTrueVector(const mfem::Vector& vec) const;

    double LinearResidualNorm(const mfem::Vector& x, const mfem::Vector& y) const;

    virtual const mfem::Array<int>& GetEssDofs() const
    {
        return hierarchy_.GetMatrix(level_).GetEssDofs();
    }

    int level_;
    Hierarchy& hierarchy_;

    const mfem::Array<int>& offsets_;
    mfem::Vector p_;         // coefficient vector in piecewise 1 basis
    mfem::Vector kp_;        // kp_ = Kappa(p)
    mfem::Vector dkinv_dp_;  // dkinv_dp_ = d ( Kappa(p)^{-1} ) / dp

    std::vector<mfem::DenseMatrix> dMdp_;
    mfem::Vector Z_vector_;

    // for debug purpose
    int print_help = 0;

    double diff_tol_;
    int max_num_backtrack_;
};

// nonlinear elliptic hierarchy
class EllipticNLMG : public NonlinearMG
{
public:
    EllipticNLMG(Hierarchy& hierarchy_, const mfem::Vector& Z_fine,
                 NLMGParameter param);
    void Solve(const mfem::Vector& rhs, mfem::Vector& sol)
    {
        NonlinearSolver::Solve(rhs, sol);
    }


    LevelSolver& GetLevelSolver(int level) { return solvers_[level]; }

private:
    virtual void Mult(int level, const mfem::Vector& x, mfem::Vector& Ax);
    virtual void Solve(int level, const mfem::Vector& rhs, mfem::Vector& sol);
    virtual void Restrict(int level, const mfem::Vector& fine, mfem::Vector& coarse) const;
    virtual void Interpolate(int level, const mfem::Vector& coarse, mfem::Vector& fine) const;
    virtual void Project(int level, const mfem::Vector& fine, mfem::Vector& coarse) const;
    virtual void Smoothing(int level, const mfem::Vector& in, mfem::Vector& out);
    virtual mfem::Vector AssembleTrueVector(const mfem::Vector& vec) const;
    virtual int LevelSize(int level) const;

    const mfem::Array<int>& Offsets(int level) const;

    virtual const mfem::Array<int>& GetEssDofs() const { return GetEssDofs(0); }

    virtual void BackTracking(int level, const mfem::Vector& rhs, double prev_resid_norm,
                              mfem::Vector& x, mfem::Vector& dx)
    {
        solvers_[level].BackTracking(rhs, prev_resid_norm, x, dx, true);
    }

    virtual mfem::Vector AssembleTrueVector(int level, const mfem::Vector& vec_dof) const
    {
        return hierarchy_.GetMatrix(level).AssembleTrueVector(vec_dof);
    }

    virtual const mfem::Array<int>& GetEssDofs(int level) const
    {
        return hierarchy_.GetMatrix(level).GetEssDofs();
    }

    Hierarchy& hierarchy_;
    std::vector<LevelSolver> solvers_;
};

double alpha;
// Kappa(p) = exp(\alpha p)
void Kappa(const mfem::Vector& p, mfem::Vector& kp);
void dKinv_dp(const mfem::Vector& p, mfem::Vector& dkinv_dp);

// Kappa(p) = K\alpha / (\alpha + |p(x, y, z) - z|^\beta)
void Kappa(const mfem::Vector& p, const mfem::Vector& Z_vec, mfem::Vector& kp);
void dKinv_dp(const mfem::Vector& p, const mfem::Vector& Z_vec, mfem::Vector& dkinv_dp);

int main(int argc, char* argv[])
{
    int num_procs, myid;

    // 1. Initialize MPI
    mpi_session session(argc, argv);
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &num_procs);
    MPI_Comm_rank(comm, &myid);

    // program options from command line
    UpscaleParameters upscale_param;
    upscale_param.spect_tol = 1.0;
    upscale_param.hybridization = true;
    mfem::OptionsParser args(argc, argv);

    NLMGParameter mg_param;

    const char* problem_name = "spe10";
    args.AddOption(&problem_name, "-mp", "--model-problem",
                   "Model problem (spe10, egg, lognormal, richard)");
    const char* perm_file = "spe_perm_rescaled.dat";
    args.AddOption(&perm_file, "-p", "--perm", "SPE10 permeability file data.");
    int dim = 2;
    args.AddOption(&dim, "-d", "--dim",
                   "Dimension of the physical space.");
    int slice = 0;
    args.AddOption(&slice, "-s", "--slice",
                   "Slice of SPE10 data to take for 2D run.");
    int num_sr = 0;
    args.AddOption(&num_sr, "-nsr", "--num-serial-refine",
                   "Number of serial refinement");
    int num_pr = 0;
    args.AddOption(&num_pr, "-npr", "--num-parallel-refine",
                   "Number of parallel refinement");
    double correlation = 0.1;
    args.AddOption(&correlation, "-cl", "--correlation-length",
                   "Correlation length");
    double alpha_in = 0.0;
    args.AddOption(&alpha_in, "-alpha", "--alpha", "alpha");
    bool use_newton = true;
    args.AddOption(&use_newton, "-newton", "--use-newton", "-picard",
                   "--use-picard", "Use Newton or Picard iteration.");
    bool use_vcycle = true;
    args.AddOption(&use_vcycle, "-VCycle", "--use-VCycle", "-FMG",
                   "--use-FMG", "Use V-cycle or FMG-cycle.");
    bool visualization = false;
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization", "Enable visualization.");
    // Read upscaling options from command line into upscale_param object
    upscale_param.RegisterInOptionsParser(args);
    mg_param.RegisterInOptionsParser(args);
    args.Parse();
    if (!args.Good())
    {
        if (myid == 0)
        {
            args.PrintUsage(std::cout);
        }
        MPI_Finalize();
        return 1;
    }
    if (myid == 0)
    {
        args.PrintOptions(std::cout);
    }
    mg_param.cycle = use_vcycle ? V_CYCLE : FMG;
    mg_param.solve_type = use_newton ? Newton : Picard;

    // Setting up finite volume discretization problem
    double use_metis = true;
    std::string problem(problem_name);
    mfem::Array<int> ess_attr(problem == "egg" ? 3 : (dim == 3 ? 6 : 4));
    ess_attr = 0;

    mfem::Vector Z_fine;
    unique_ptr<DarcyProblem> fv_problem;
    if (problem == "spe10")
    {
        ess_attr = 1;
        ess_attr[dim - 2] = 0;
//        ess_attr[dim] = 0;
        fv_problem.reset(new SPE10Problem(perm_file, dim, 5, slice, use_metis, ess_attr));
        alpha = 1.*(1e-3);
    }
    else if (problem == "egg")
    {
        ess_attr = 1;
        ess_attr[1] = 0;

        use_metis = true;
        fv_problem.reset(new EggModel(num_sr, num_pr, ess_attr));
        alpha = 3.;//-5e-1;
    }
    else if (problem == "lognormal")
    {
        fv_problem.reset(new LognormalModel(dim, num_sr, num_pr, correlation, ess_attr));
        alpha = -8e0;
    }
    else if (problem == "richard")
    {
        ess_attr = 1;
        ess_attr[0] = 0;
        fv_problem.reset(new Richards(num_sr, ess_attr));
        Z_fine = fv_problem->GetZVector();
        alpha = 124.6;          // Loam
//        alpha = 1.175e6;    // sand
    }
    else
    {
        mfem::mfem_error("Unknown model problem!");
    }
    alpha = alpha_in == 0.0 ? alpha : alpha_in;
    if (myid == 0)
    {
        std::cout << "alpha = " << alpha <<"\n";
    }

    Graph graph = fv_problem->GetFVGraph(true);

    mfem::Array<int> partitioning;
    mfem::Array<int> coarsening_factors(dim);

    if (use_metis)
    {
        coarsening_factors = 1;
        coarsening_factors[0] = upscale_param.coarse_factor;
    }
    else
    {
        coarsening_factors[0] = 10;
        coarsening_factors[1] = 22;
        coarsening_factors.Last() = dim == 3 ? 2 : 10;
        if (myid == 0)
        {
            std::cout << "Coarsening factors: " << coarsening_factors[0]
                      << " x " << coarsening_factors[1];
            if (dim == 3)
            {
                std::cout << " x " << coarsening_factors[2] << "\n";
            }
            else
            {
                std::cout << "\n";
            }
        }
    }

    if (upscale_param.max_levels > 1)
    {
        fv_problem->Partition(use_metis, coarsening_factors, partitioning);
        upscale_param.num_iso_verts = fv_problem->NumIsoVerts();
    }

    // Create hierarchy
    Hierarchy hierarchy(graph, upscale_param, &partitioning, &ess_attr);
    hierarchy.PrintInfo();

//    if (upscale_param.hybridization)
//    {
//        hierarchy.SetRelTol(1e-12);
//        hierarchy.SetAbsTol(1e-15);
//    }

    mfem::BlockVector rhs(hierarchy.GetMatrix(0).BlockOffsets());
//    if (problem != "lognormal")
    {
        rhs.GetBlock(0) = fv_problem->GetEdgeRHS();
        rhs.GetBlock(1) = fv_problem->GetVertexRHS();
    }
//    else
//    {
//        rhs.GetBlock(0) = 0.0;
//        rhs.GetBlock(1) = -fv_problem->CellVolume();
//    }

    mfem::BlockVector sol_nlmg(rhs);
    sol_nlmg = 0.0;
    mfem::BlockVector sol_nlmg2(rhs);
    sol_nlmg2 = 0.0;


    Upscale upscale(std::move(hierarchy));
    auto& hierarchy_ = upscale.GetHierarchy();


    EllipticNLMG nlmg(hierarchy_, Z_fine, mg_param);
    nlmg.SetPrintLevel(1);
    nlmg.SetMaxIter(150);


//    std::vector<double> alpha_choices{ 1.0, 10., 100., 1000. };
    std::vector<double> alpha_choices{ 0.1, 0.2, 0.4, 0.8, 1.6 };
//    std::vector<double> alpha_choices{ 0.1, 0.4, 1.6, 6.4 };
    mfem::Array<double> timings;
    timings.Reserve(alpha_choices.size());
    for (auto& alpha_ : alpha_choices)
    {
        alpha = alpha_;
        if (myid == 0) std::cout << "alpha = " << alpha << "\n";
        sol_nlmg = 0.0;
        nlmg.Solve(rhs, sol_nlmg);
        timings.Append(nlmg.GetTiming());

//        std::vector<mfem::BlockVector> rhs_;
//        std::vector<mfem::BlockVector> sol_;
//        rhs_.reserve(hierarchy_.NumLevels());
//        sol_.reserve(hierarchy_.NumLevels());

//        rhs_.push_back(rhs);
//        sol_.emplace_back(hierarchy_.BlockOffsets(0));
//        for (int i = 0; i < upscale_param.max_levels - 1; ++i)
//        {
//            rhs_.emplace_back(hierarchy_.BlockOffsets(i + 1));
//            sol_.emplace_back(hierarchy_.BlockOffsets(i + 1));
//            hierarchy_.Restrict(i, rhs_[i], rhs_[i + 1]);
//            sol_.back() = 0.0;
//        }

//        for (int l = upscale_param.max_levels - 1; l >= 0; l--)
//        {
//            auto solver_l = nlmg.GetLevelSolver(l);

//            if (l == 0)
//            {
//                sol_[l] = 0.0;
//    //            sol_[l].GetBlock(0) = sol_nlmg.GetBlock(0);
//    //              sol_[l] = hierarchy_.Project(0, sol_nlmg);

//    //            hierarchy_.SetPrintLevel(1);
//    //            hierarchy_.SetMaxIter(1000);
//                sol_[l] = hierarchy_.Interpolate(1, sol_[1]);
//    //            sol_[l].GetBlock(0) = 0.0;
////                sol_[l].GetBlock(0) = sol_nlmg.GetBlock(0);

//            }

//            solver_l.Solve(rhs_[l], sol_[l]);

//            for (int i = l; i > 0; --i)
//            {
//                hierarchy_.Interpolate(i, sol_[i], sol_[i - 1]);
//            }

//    //        if (l > 0)
//            {
//                upscale.ShowErrors(sol_[0], sol_nlmg, 0);
//    //            sol_[0] -= sol_nlmg;
//            }
//    //        else { sol_nlmg = sol_[0]; }

////            if (visualization && l == 0)
//            {
//                mfem::socketstream sout;
//                fv_problem->VisSetup2(sout, sol_[0].GetBlock(0), 0.0, 0.0, "level "+std::to_string(l));
//                fv_problem->VisSetup(sout, sol_[0].GetBlock(1), 0.0, 4.5, "level "+std::to_string(l));
//                if (problem == "richard")
//                    sout << "keys ]]]]]]]]]]]]]]]]]]]]]]]]]]]]fmm\n";
//            }
//        }

    }
    if (myid == 0) timings.Print(std::cout, timings.Size());








    if (visualization)
    {
        if (problem == "richard")
        {
            sol_nlmg.GetBlock(1) -= Z_fine;
        }

        mfem::socketstream sout;
        fv_problem->VisSetup2(sout, sol_nlmg.GetBlock(0), 0.0, 0.0, "coarse flux");
        fv_problem->VisSetup(sout, sol_nlmg.GetBlock(1), 0.0, 0.0, "coarse pressure");
//        fv_problem->VisSetup2(sout, sol_nlmg2.GetBlock(0), 0.0, 0.0, "fine flux");
//        fv_problem->VisSetup(sout, sol_nlmg2.GetBlock(1), 0.0, 0.0, "fine pressure");
        if (problem == "richard")
            sout << "keys ]]]]]]]]]]]]]]]]]]]]]]]]]]]]fmm\n";
    }

    return EXIT_SUCCESS;
}

/// @todo take MixedMatrix only
LevelSolver::LevelSolver(Hierarchy& hierarchy, int level,
                         mfem::Vector Z_vector, NLMGParameter param)
    : NonlinearSolver(hierarchy.GetComm(), hierarchy.BlockOffsets(level)[2],
                      param.solve_type, param.solve_type? "Picard" : "Newton",
                      param.initial_linear_tol),
      level_(level), hierarchy_(hierarchy), offsets_(hierarchy_.BlockOffsets(level)),
      p_(hierarchy_.NumVertices(level)), kp_(p_.Size()), dkinv_dp_(p_.Size()),
      Z_vector_(std::move(Z_vector))
{
    hierarchy_.SetPrintLevel(level_, 0);
    hierarchy_.SetMaxIter(level_, 200);
    diff_tol_ = level ? param.coarse_diff_tol : param.diff_tol;
    max_num_backtrack_ = param.max_num_backtrack;
    if (myid_ == 0)
    {
        std::cout << "\nMG level " << level << " parameters:\n"
                  << "  Pressure change tol: " << diff_tol_ << "\n"
                  << "  Max number of residual-based backtracking: = "
                  << max_num_backtrack_ << "\n";
    }
}

void LevelSolver::Mult(const mfem::Vector& x, mfem::Vector& Ax)
{
    assert(size_ == Ax.Size());
    assert(size_ == x.Size());
    mfem::BlockVector block_x(x.GetData(), offsets_);
    mfem::BlockVector block_Ax(Ax.GetData(), offsets_);

    if (level_ >= 0)
    {
        EvalCoef(block_x.GetBlock(1));
        hierarchy_.GetMatrix(level_).Mult(kp_, block_x, block_Ax);
    }
    else // exact evaluation of nonlinear problem by going to fine grid
    {
        hierarchy_.RescaleCoefficient(level_, block_x.GetBlock(1), Kappa);
        kp_ = 1.0;
        hierarchy_.GetMatrix(level_).Mult(kp_, block_x, block_Ax);
    }
}

mfem::Vector LevelSolver::AssembleTrueVector(const mfem::Vector& vec) const
{
    return hierarchy_.GetMatrix(level_).AssembleTrueVector(vec);
}

void LevelSolver::IterationStep(const mfem::Vector& rhs, mfem::Vector& sol)
{
    mfem::BlockVector block_sol(sol.GetData(), offsets_);
    mfem::BlockVector block_rhs(rhs.GetData(), offsets_);

    if (max_num_iter_ > 1)
    {
        hierarchy_.SetRelTol(level_, linear_tol_);
    }

    if (solve_type_ == Picard)
    {
        PicardStep(block_rhs, block_sol);
    }
    else
    {
        NewtonStep(block_rhs, block_sol);
    }
}

void LevelSolver::EvalCoef(const mfem::Vector& sol_block1)
{
    p_ = hierarchy_.PWConstProject(level_, sol_block1);

//    double p_max = AbsMax(p_, comm_);
//    double p_min = Min(p_, comm_);
//    if (myid_==0)
//    {
//        std::cout << "  Level " << level_ <<"\n";
//        std::cout << "  \tmax pressure =  " << p_max <<"\n";
//        std::cout << "  \tmin pressure =  " << p_min <<"\n";
//    }
    if (Z_vector_.Size())
        Kappa(p_, Z_vector_, kp_);
    else
        Kappa(p_, kp_);
}

void LevelSolver::EvalCoefDerivative(const mfem::Vector& sol_block1)
{
//    p_ = hierarchy_.PWConstProject(level_, sol_block1);
    if (Z_vector_.Size())
        dKinv_dp(p_, Z_vector_, dkinv_dp_);
    else
        dKinv_dp(p_, dkinv_dp_);
}

void LevelSolver::PicardStep(const mfem::BlockVector& rhs, mfem::BlockVector& x)
{
    mfem::BlockVector delta_x(x);
    prev_resid_norm_ = ResidualNorm(x, rhs);

//    double p_max = AbsMax(p_, comm_);
//    double p_min = Min(p_, comm_);

//    if (myid_==0)
//    {
//        if (level_ == 0 && !(print_help % 3))
//            std::cout << "\n";
//        std::cout << "  Level " << level_ << " before solving:\n";
//        std::cout << "  \tmax pressure =  " << p_max <<"\n";
//        std::cout << "  \tmin pressure =  " << p_min <<"\n\n";
//    }

    if (level_ >= 0)
    {
//        EvalCoef(x.GetBlock(1));
        hierarchy_.RescaleCoefficient(level_, kp_);
    }
    else // exact evaluation of nonlinear problem
    {
        hierarchy_.RescaleCoefficient(level_, x.GetBlock(1), Kappa);
    }

//    mfem::BlockVector rhs_plus_b(rhs);
//    rhs_plus_b += b_;

    hierarchy_.Solve(level_, rhs, x);

    delta_x -= x;

    BackTracking(rhs, prev_resid_norm_, x, delta_x);

    if (linear_tol_criterion_ == TaylorResidual)
    {
        linear_resid_norm_ = LinearResidualNorm(x, rhs);
    }
}

void LevelSolver::NewtonStep(const mfem::BlockVector& rhs, mfem::BlockVector& x)
{
    Mult(x, residual_);
    residual_ -= rhs;
    for (int i = 0; i < x.BlockSize(0); ++i)
    {
        if (GetEssDofs()[i])
            residual_[i] = 0.0;
    }

    Build_dMdp(x);

//    double p_max = AbsMax(p_, comm_);
//    double p_min = Min(p_, comm_);

//    if (myid_==0)
//    {
//        if (level_ == 0 && !(print_help % 3))
//            std::cout << "\n";
//        std::cout << "  Level " << level_ << " before solving:\n";
//        std::cout << "  \tmax pressure =  " << p_max <<"\n";
//        std::cout << "  \tmin pressure =  " << p_min <<"\n\n";
//    }

    hierarchy_.UpdateJacobian(level_, kp_, dMdp_);

    mfem::BlockVector block_residual(residual_.GetData(), offsets_);
    mfem::BlockVector delta_x = hierarchy_.Solve(level_, block_residual);

    mfem::Vector true_resid = AssembleTrueVector(block_residual);
    prev_resid_norm_ = mfem::ParNormlp(true_resid, 2, comm_);

    x -= delta_x;

    BackTracking(rhs, prev_resid_norm_, x, delta_x);

    if (linear_tol_criterion_ == TaylorResidual)
    {
        linear_resid_norm_ = LinearResidualNorm(delta_x, block_residual);
    }
}

void LevelSolver::BackTracking(const mfem::Vector& rhs, double prev_resid_norm,
                               mfem::Vector& x, mfem::Vector& dx, bool interlopate)
{
    int k = 0;

    print_help++;

    mfem::BlockVector block_dx(dx.GetData(), offsets_);
//    mfem::BlockVector block_x(x.GetData(), offsets_);
//    mfem::BlockVector block_r(residual_.GetData(), offsets_);

    // check percentage change and enforce it to be under certain percent
    if (!interlopate)
    {
        auto delta_p = hierarchy_.PWConstProject(level_, block_dx.GetBlock(1));

        double max_change_threshold = std::log(diff_tol_);
        double max_pressure_change = AbsMax(delta_p, comm_);

        double relative_change = max_pressure_change * alpha / max_change_threshold;

        if (relative_change > 1)
        {
//            if (myid_ ==0)
//            {
//                std::cout << "  Level " << level_ <<":\n";
//                std::cout << "  \trelative_change =  " << relative_change <<"\n";
//                std::cout << "  \tmax_pressure_change =  " << max_pressure_change <<"\n";
//            }

            dx /= relative_change;
            x.Add(relative_change - 1.0, dx);
        }
    }

    if (max_num_backtrack_ > 0)
        resid_norm_ = ResidualNorm(x, rhs);

    while (k < max_num_backtrack_ && resid_norm_ > prev_resid_norm)
    {
        double backtracking_resid_norm = resid_norm_;

//        double blk1_norm = mfem::ParNormlp(block_r.GetBlock(1), 2, comm_);
//        if (myid_ == 0)std::cout<<"blk 1 resid norm before = "<<blk1_norm<<"\n";

//        block_dx.GetBlock(1) *= 0.5;
//        block_x.GetBlock(1) += block_dx.GetBlock(1);

        dx *= 0.5;
        x += dx;


        resid_norm_ = ResidualNorm(x, rhs);


//        blk1_norm = mfem::ParNormlp(block_r.GetBlock(1), 2, comm_);
//        if (myid_ == 0)std::cout<<"blk 1 resid norm after = "<<blk1_norm<<"\n";

        if (resid_norm_ > 0.9 * backtracking_resid_norm)
        {
//            block_x.GetBlock(1) -= block_dx.GetBlock(1);
            x -= dx;
            break;
        }

        if (myid_ == 0  && print_level_ > 1)
        {
            if (k == 0)
            {
                std::cout << "  Level " << level_ << " backtracking: || R(u) ||";
            }
            std::cout << " -> " << backtracking_resid_norm;
        }
        k++;
    }

    if (k > 0 && myid_ == 0 && print_level_ > 1)
    {
        std::cout << "\n";
    }
}

double LevelSolver::LinearResidualNorm(const mfem::Vector& x, const mfem::Vector& y) const
{
    const MixedMatrix& mixed_system = hierarchy_.GetMatrix(level_);

    mfem::BlockVector block_x(x.GetData(), offsets_);
    mfem::BlockVector block_y(y.GetData(), offsets_);

    mfem::BlockVector linear_resid(offsets_);
    mixed_system.Mult(kp_, block_x, linear_resid);
    linear_resid -= block_y;

    if (solve_type_ == Newton)
    {
        auto& vert_vdof = mixed_system.GetGraphSpace().VertexToVDof();
        auto& vert_edof = mixed_system.GetGraphSpace().VertexToEDof();

        mfem::Array<int> local_edofs, local_vdofs;
        mfem::Vector x_loc;
        mfem::Vector y_loc;
        for (int i = 0; i < vert_vdof.NumRows(); ++i)
        {
            GetTableRow(vert_vdof, i, local_vdofs);
            GetTableRow(vert_edof, i, local_edofs);

            block_x.GetBlock(1).GetSubVector(local_vdofs, x_loc);

            y_loc.SetSize(local_edofs.Size());
            dMdp_[i].Mult(x_loc, y_loc);

            for (int j = 0; j < local_edofs.Size(); ++j)
            {
                linear_resid[local_edofs[j]] += y_loc[j];
            }
        }
    }

    mfem::Vector true_linear_resid = AssembleTrueVector(linear_resid);

    return mfem::ParNormlp(true_linear_resid, 2, comm_);
}

void LevelSolver::Build_dMdp(const mfem::BlockVector& iterate)
{
    auto& mixed_system = hierarchy_.GetMatrix(level_);
    auto& vert_edof = mixed_system.GetGraphSpace().VertexToEDof();
    auto& vert_vdof = mixed_system.GetGraphSpace().VertexToVDof();

    auto& MB = dynamic_cast<const ElementMBuilder&>(mixed_system.GetMBuilder());
    auto& M_el = MB.GetElementMatrices();

    auto& proj_pwc = const_cast<mfem::SparseMatrix&>(mixed_system.GetPWConstProj());

    dMdp_.resize(M_el.size());
    mfem::Array<int> local_edofs, local_vdofs, vert(1);
    mfem::Vector sigma_loc, Msigma_vec;
    mfem::DenseMatrix proj_pwc_loc;

    EvalCoefDerivative(iterate.GetBlock(1)); // dkinv_dp_ is updated

    for (int i = 0; i < vert_edof.NumRows(); ++i)
    {
        GetTableRow(vert_edof, i, local_edofs);
        GetTableRow(vert_vdof, i, local_vdofs);
        vert[0] = i;

        iterate.GetBlock(0).GetSubVector(local_edofs, sigma_loc);
        Msigma_vec.SetSize(local_edofs.Size());
        M_el[i].Mult(sigma_loc, Msigma_vec);
        mfem::DenseMatrix Msigma_loc(Msigma_vec.GetData(), M_el[i].Size(), 1);

        proj_pwc_loc.SetSize(1, local_vdofs.Size());
        proj_pwc_loc = 0.0;
        proj_pwc.GetSubMatrix(vert, local_vdofs, proj_pwc_loc);
        proj_pwc_loc *= dkinv_dp_[i];

        dMdp_[i].SetSize(local_edofs.Size(), local_vdofs.Size());
        mfem::Mult(Msigma_loc, proj_pwc_loc, dMdp_[i]);
    }
}

EllipticNLMG::EllipticNLMG(Hierarchy& hierarchy, const mfem::Vector& Z_fine,
                           NLMGParameter param)
    : NonlinearMG(hierarchy.GetComm(), hierarchy.BlockOffsets(0)[2],
                  hierarchy.NumLevels(), param),
      hierarchy_(hierarchy)
{
    std::vector<mfem::Vector> help(hierarchy.NumLevels());
    solvers_.reserve(num_levels_);
    for (int level = 0; level < num_levels_; ++level)
    {
        mfem::Vector Z_l;
        if (Z_fine.Size())
        {
            if (level == 0)
            {
                help[level].SetDataAndSize(Z_fine.GetData(), Z_fine.Size());
            }
            else
            {
                help[level] = hierarchy.Project(level - 1, help[level - 1]);
            }
            Z_l = hierarchy.PWConstProject(level, help[level]);
        }

        if (level > 0)
        {
            rhs_[level].SetSize(LevelSize(level));
            sol_[level].SetSize(LevelSize(level));
            rhs_[level] = 0.0;
            sol_[level] = 0.0;
        }
        help_[level].SetSize(LevelSize(level));
        help_[level] = 0.0;

        if (level == 0)
        {
            solvers_.emplace_back(hierarchy, level, std::move(Z_l), param);
        }
        else
        {
            solvers_.emplace_back(hierarchy, level, std::move(Z_l), param);
        }
        solvers_[level].SetPrintLevel(cycle_ == V_CYCLE ? -1 : 0);
//        solvers_[level].SetRelTol(level == 0 ? 1e-8 : 1e-4);

        const int num_relax = level == 0 ? param.num_relax_fine :
                              (level < num_levels_ - 1 ?
                              param.num_relax_middle : param.num_relax_coarse);
        solvers_[level].SetMaxIter(num_relax);
        if (myid_ == 0) std::cout << "  Number of smoothing: " << num_relax << "\n";
    }
    if (myid_ == 0) std::cout << "\n";
}

void EllipticNLMG::Mult(int level, const mfem::Vector& x, mfem::Vector& Ax)
{
    solvers_[level].Mult(x, Ax);
}

void EllipticNLMG::Solve(int level, const mfem::Vector& rhs, mfem::Vector& sol)
{
    Smoothing(level, rhs, sol);
}

void EllipticNLMG::Restrict(int level, const mfem::Vector& fine, mfem::Vector& coarse) const
{
    mfem::BlockVector block_fine(fine.GetData(), Offsets(level));
    coarse = hierarchy_.Restrict(level, block_fine);
}

void EllipticNLMG::Interpolate(int level, const mfem::Vector& coarse, mfem::Vector& fine) const
{
    mfem::BlockVector block_coarse(coarse.GetData(), Offsets(level));
    fine = hierarchy_.Interpolate(level, block_coarse);
}

void EllipticNLMG::Project(int level, const mfem::Vector& fine, mfem::Vector& coarse) const
{
    mfem::BlockVector block_fine(fine.GetData(), Offsets(level));
    coarse = hierarchy_.Project(level, block_fine);
}

void EllipticNLMG::Smoothing(int level, const mfem::Vector& in, mfem::Vector& out)
{
    double ratio = solve_type_ == Newton ? 1e-6 : 1e-2;
    hierarchy_.SetRelTol(level, std::max((level ? ratio : 1.0) * linear_tol_, 1e-8));

    solvers_[level].Solve(in, out);

    if (level == 0 && linear_tol_criterion_ == TaylorResidual)
    {
        linear_resid_norm_ = solvers_[0].GetLinearResidualNorm();
    }
}

const mfem::Array<int>& EllipticNLMG::Offsets(int level) const
{
    return hierarchy_.BlockOffsets(level);
}

mfem::Vector EllipticNLMG::AssembleTrueVector(const mfem::Vector& vec) const
{
    return AssembleTrueVector(0, vec);
}

int EllipticNLMG::LevelSize(int level) const
{
    return hierarchy_.GetMatrix(level).NumTotalDofs();
}

// SPE10: -7-6    Egg: -5e-1     Lognormal: -8e0 (cl = 0.1)
void Kappa(const mfem::Vector& p, mfem::Vector& kp)
{
    assert(kp.Size() == p.Size());
//    std::cout<<"p_max = " <<p.Max()<<"\n";
//    std::cout<<"p_min = " <<p.Min()<<"\n";
//    std::cout<<"size = "<<p.Size()<<"\n";

    for (int i = 0; i < p.Size(); i++)
    {
//        assert(p[i] >= 0.0);
        kp[i] = std::exp(alpha * p[i]);

        if (kp[i] == 0.0)
        {
//            std::cout<<"p_max = " <<p.Max()<<"\n";
//            std::cout<<"p_min = " <<p.Min()<<"\n";
//            std::cout<<"size = "<<p.Size()<<"\n";
//            std::cout<<"(p, kp) = "<<p[i] << " " <<kp[i]<<"\n";
        }

//        assert(kp[i] > 0.0);
    }
}

void dKinv_dp(const mfem::Vector& p, mfem::Vector& dkinv_dp)
{
    assert(dkinv_dp.Size() == p.Size());
    for (int i = 0; i < p.Size(); i++)
    {
        double exp_ap = std::exp(alpha * p[i]);
//        assert(exp_ap > 0.0);
        dkinv_dp[i] = -(alpha * exp_ap) / (exp_ap * exp_ap);
    }
}

// Loam
double beta = 1.77;
double K_s = 1.067;//* 0.01; // cm/day

// Sand
//double beta = 4.74;
//double K_s = 816.0;// * 0.01; // cm/day

void Kappa(const mfem::Vector& p, const mfem::Vector& Z_vec, mfem::Vector& kp)
{
    double alpha_K_s = K_s * alpha;

    assert(kp.Size() == p.Size());
    assert(Z_vec.Size() == p.Size());
    for (int i = 0; i < p.Size(); i++)
    {
        kp[i] = alpha_K_s / (alpha + std::pow(std::fabs(p[i] - Z_vec[i]), beta));
        assert(kp[i] > 0.0);
    }
}

void dKinv_dp(const mfem::Vector& p, const mfem::Vector& Z_vec, mfem::Vector& dkinv_dp)
{
    double b_over_a_K_s = beta / (K_s * alpha);

    assert(dkinv_dp.Size() == p.Size());
    assert(Z_vec.Size() == p.Size());
    for (int i = 0; i < p.Size(); i++)
    {
        double p_head = p[i] - Z_vec[i];
        double sign = p_head < 0.0 ? -1.0 : 1.0;
        dkinv_dp[i] = sign * b_over_a_K_s * std::pow(std::fabs(p_head), beta - 1.0);
    }
}
