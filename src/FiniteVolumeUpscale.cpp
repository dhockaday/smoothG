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

    @brief Implements FiniteVolumeUpscale class
*/

#include "FiniteVolumeUpscale.hpp"

namespace smoothg
{

// TODO(gelever1): Refactor these two constructors into one (or use Init function)
FiniteVolumeUpscale::FiniteVolumeUpscale(MPI_Comm comm,
                                         const mfem::SparseMatrix& vertex_edge,
                                         const mfem::Vector& weight,
                                         const mfem::Array<int>& partitioning,
                                         const mfem::HypreParMatrix& edge_d_td,
                                         const mfem::SparseMatrix& edge_boundary_att,
                                         const mfem::Array<int>& ess_attr,
                                         const UpscaleParameters& param)
    : Upscale(comm, vertex_edge.Height()),
      edge_d_td_(edge_d_td),
      edge_boundary_att_(edge_boundary_att),
      ess_attr_(ess_attr),
      param_(param)
{
    mfem::StopWatch chrono;
    chrono.Start();

    // Hypre may modify the original vertex_edge, which we seek to avoid
    mfem::SparseMatrix ve_copy(vertex_edge);

    mixed_laplacians_.emplace_back(vertex_edge, weight, edge_d_td_,
                                   MixedMatrix::DistributeWeight::False);

    auto graph_topology = make_unique<GraphTopology>(ve_copy, edge_d_td_, partitioning,
                                                     &edge_boundary_att_);

    bool coarse_coefficient = false;
    coarsener_ = make_unique<SpectralAMG_MGL_Coarsener>(
                     mixed_laplacians_[0], std::move(graph_topology), param_.spect_tol,
                     param_.max_evects, param_.dual_target, param_.scaled_dual, param_.energy_dual,
                     coarse_coefficient);
    coarsener_->construct_coarse_subspace();

    mixed_laplacians_.push_back(coarsener_->GetCoarse());

    mfem::SparseMatrix& Dref = GetCoarseMatrix().GetD();
    mfem::Array<int> marker(Dref.Width());
    marker = 0;

    MarkDofsOnBoundary(coarsener_->get_GraphTopology_ref().face_bdratt_,
                       coarsener_->construct_face_facedof_table(),
                       ess_attr, marker);

    if (param_.hybridization) // Hybridization solver
    {
        auto face_bdratt = coarsener_->get_GraphTopology_ref().face_bdratt_;
        coarse_solver_ = make_unique<HybridSolver>(
                             comm, mixed_laplacians_.back(), *coarsener_,
                             &face_bdratt, &marker, 0, param_.saamge_param);
    }
    else // L2-H1 block diagonal preconditioner
    {
        GetCoarseMatrix().BuildM();
        mfem::SparseMatrix& Mref = GetCoarseMatrix().GetM();
        for (int mm = 0; mm < marker.Size(); ++mm)
        {
            // Assume M diagonal, no ess data
            if (marker[mm])
                Mref.EliminateRow(mm, true);
        }

        Dref.EliminateCols(marker);

        coarse_solver_ = make_unique<MinresBlockSolverFalse>(comm, GetCoarseMatrix());
    }

    MakeCoarseVectors();

    chrono.Stop();
    setup_time_ += chrono.RealTime();
}

FiniteVolumeUpscale::FiniteVolumeUpscale(MPI_Comm comm,
                                         const mfem::SparseMatrix& vertex_edge,
                                         const mfem::Vector& weight,
                                         const mfem::SparseMatrix& w_block,
                                         const mfem::Array<int>& partitioning,
                                         const mfem::HypreParMatrix& edge_d_td,
                                         const mfem::SparseMatrix& edge_boundary_att,
                                         const mfem::Array<int>& ess_attr,
                                         const UpscaleParameters& param)
    : Upscale(comm, vertex_edge.Height()),
      edge_d_td_(edge_d_td),
      edge_boundary_att_(edge_boundary_att),
      ess_attr_(ess_attr),
      param_(param)
{
    mfem::StopWatch chrono;
    chrono.Start();

    // Hypre may modify the original vertex_edge, which we seek to avoid
    mfem::SparseMatrix ve_copy(vertex_edge);

    mixed_laplacians_.emplace_back(vertex_edge, weight, w_block, edge_d_td_,
                                   MixedMatrix::DistributeWeight::False);

    auto graph_topology = make_unique<GraphTopology>(
                              ve_copy, edge_d_td_, partitioning, &edge_boundary_att_);

    bool coarse_coefficient = false;
    coarsener_ = make_unique<SpectralAMG_MGL_Coarsener>(
                     mixed_laplacians_[0], std::move(graph_topology), param_.spect_tol,
                     param_.max_evects, param_.dual_target, param_.scaled_dual, param_.energy_dual,
                     coarse_coefficient);
    coarsener_->construct_coarse_subspace();

    mixed_laplacians_.push_back(coarsener_->GetCoarse());

    mfem::SparseMatrix& Dref = GetCoarseMatrix().GetD();
    mfem::Array<int> marker(Dref.Width());
    marker = 0;

    MarkDofsOnBoundary(coarsener_->get_GraphTopology_ref().face_bdratt_,
                       coarsener_->construct_face_facedof_table(),
                       ess_attr, marker);

    if (param_.hybridization) // Hybridization solver
    {
        auto face_bdratt = coarsener_->get_GraphTopology_ref().face_bdratt_;
        coarse_solver_ = make_unique<HybridSolver>(
                             comm, mixed_laplacians_.back(), *coarsener_,
                             &face_bdratt, &marker, 0, param_.saamge_param);
    }
    else // L2-H1 block diagonal preconditioner
    {
        GetCoarseMatrix().BuildM();
        mfem::SparseMatrix& Mref = GetCoarseMatrix().GetM();
        for (int mm = 0; mm < marker.Size(); ++mm)
        {
            // Assume M diagonal, no ess data
            if (marker[mm])
                Mref.EliminateRow(mm, true);
        }

        Dref.EliminateCols(marker);

        coarse_solver_ = make_unique<MinresBlockSolverFalse>(comm, mixed_laplacians_.back());
    }

    MakeCoarseVectors();

    chrono.Stop();
    setup_time_ += chrono.RealTime();
}

void FiniteVolumeUpscale::MakeFineSolver()
{
    mfem::Array<int> marker;
    BooleanMult(edge_boundary_att_, ess_attr_, marker);

    if (!fine_solver_)
    {
        if (param_.hybridization) // Hybridization solver
        {
            fine_solver_ = make_unique<HybridSolver>(comm_, GetFineMatrix(),
                                                     &edge_boundary_att_, &marker);
        }
        else // L2-H1 block diagonal preconditioner
        {
            mfem::SparseMatrix& Mref = GetFineMatrix().GetM();
            mfem::SparseMatrix& Dref = GetFineMatrix().GetD();
            const bool w_exists = GetFineMatrix().CheckW();

            for (int mm = 0; mm < marker.Size(); ++mm)
            {
                if (marker[mm])
                {
                    //Mref.EliminateRowCol(mm, ess_data[k][mm], *(rhs[k]));

                    const bool set_diag = true;
                    Mref.EliminateRow(mm, set_diag);
                }
            }
            Dref.EliminateCols(marker);
            if (!w_exists && myid_ == 0)
            {
                Dref.EliminateRow(0);
            }

            fine_solver_ = make_unique<MinresBlockSolverFalse>(comm_, GetFineMatrix());
        }
    }
}

} // namespace smoothg
