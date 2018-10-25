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

    @brief Contains GraphUpscale class
*/

#ifndef __GRAPHUPSCALE_HPP__
#define __GRAPHUPSCALE_HPP__

#include "MinresBlockSolver.hpp"
#include "HybridSolver.hpp"
#include "SpectralAMG_MGL_Coarsener.hpp"
#include "MetisGraphPartitioner.hpp"
#include "MixedMatrix.hpp"
#include "Upscale.hpp"
#include "mfem.hpp"
#include "Graph.hpp"

namespace smoothg
{

/**
   @brief Use upscaling as operator.
*/
class GraphUpscale : public Upscale
{
public:
    /**
       @brief Constructor

       @param comm MPI communicator
       @param global_vertex_edge relationship between vertices and edge
       @param global_partitioning partition of global vertices for the first
              graph coarsening (use upscale.coarse_factor for further coarsening)
       @param weight edge weights. if not provided, set to an empty vector
    */
    GraphUpscale(MPI_Comm comm,
                 const mfem::SparseMatrix& global_vertex_edge,
                 const mfem::Array<int>& partitioning,
                 const UpscaleParameters& param = UpscaleParameters(),
                 const mfem::Vector& global_weight = mfem::Vector());

    /**
       @brief Constructor

       @param comm MPI communicator
       @param global_vertex_edge relationship between vertices and edge
    */
    GraphUpscale(MPI_Comm comm,
                 const mfem::SparseMatrix& global_vertex_edge,
                 const UpscaleParameters& param = UpscaleParameters(),
                 const mfem::Vector& global_weight = mfem::Vector());

    /// Create Fine Level Solver
    void MakeFineSolver();

private:
    void Init(const mfem::SparseMatrix& vertex_edge,
              const mfem::Array<int>& partitioning,
              const mfem::Vector& weight);

    std::unique_ptr<smoothg::Graph> graph_;

    const int global_edges_;
    const int global_vertices_;
    const UpscaleParameters& param_;
};

} // namespace smoothg

#endif /* __GRAPHUPSCALE_HPP__ */
