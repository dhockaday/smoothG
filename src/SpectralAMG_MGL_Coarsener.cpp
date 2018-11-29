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

    @brief Implements SpectralAMG_MGL_Coarsener object.
*/

#ifndef __SPECTRALAMG_MGL_COARSENER_IMPL_HPP__
#define __SPECTRALAMG_MGL_COARSENER_IMPL_HPP__

#include "SpectralAMG_MGL_Coarsener.hpp"
#include "LocalMixedGraphSpectralTargets.hpp"
#include "GraphCoarsen.hpp"

using std::unique_ptr;

namespace smoothg
{

SpectralAMG_MGL_Coarsener::SpectralAMG_MGL_Coarsener(const MixedMatrix& mgL,
                                                     const UpscaleParameters& param,
                                                     const mfem::Array<int>* partitioning)
    : Mixed_GL_Coarsener(mgL), param_(param), partitioning_(partitioning)
{
}

void SpectralAMG_MGL_Coarsener::do_construct_coarse_subspace(const mfem::Vector& constant_rep)
{

    Graph coarse_graph;
    if (partitioning_)
    {
        coarse_graph = graph_topology_.Coarsen(*partitioning_);
    }
    else
    {
        coarse_graph = graph_topology_.Coarsen(param_.coarse_factor);
    }

    using LMGST = LocalMixedGraphSpectralTargets;

    std::vector<mfem::DenseMatrix> local_edge_traces;
    std::vector<mfem::DenseMatrix> local_spectral_vertex_targets;

    LMGST localtargets(mgL_, graph_topology_, param_);
    localtargets.Compute(local_edge_traces, local_spectral_vertex_targets,
                         constant_rep);

    if (param_.coarse_components)
    {
        coarse_m_builder_ = make_unique<CoefficientMBuilder>(graph_topology_);
    }
    else
    {
        coarse_m_builder_ = make_unique<ElementMBuilder>();
    }

    coarse_graph_space_ = graph_coarsen_->BuildCoarseGraphSpace(
                local_spectral_vertex_targets, local_edge_traces, std::move(coarse_graph));

    graph_coarsen_->BuildInterpolation(local_edge_traces,
                                       local_spectral_vertex_targets,
                                       Pu_, Psigma_, face_facedof_table_,
                                       *coarse_m_builder_, constant_rep);



    coarse_D_ = graph_coarsen_->GetCoarseD();
    coarse_W_ = graph_coarsen_->GetCoarseW();
}

} // namespace smoothg

#endif /* __SPECTRALAMG_MGL_COARSENER_IMPL_HPP__ */
