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

    @brief Implements MixedMatrix object.
*/

#ifndef __MIXEDMATRIX_IMPL_HPP__
#define __MIXEDMATRIX_IMPL_HPP__

#include "MixedMatrix.hpp"
#include "mfem.hpp"
#include "utilities.hpp"
#include <memory>

using std::unique_ptr;

namespace smoothg
{

MixedMatrix::MixedMatrix(Graph graph, const mfem::SparseMatrix& w_block)
    : graph_space_(std::move(graph)), edge_d_td_(&graph_space_.EDofToTrueEDof()),
      edge_td_d_(edge_d_td_->Transpose())
{
    const Graph& graph_ref = graph_space_.GetGraph();
    Init(graph_ref.VertexToEdge(), graph_ref.EdgeWeight(), w_block);
}

MixedMatrix::MixedMatrix(GraphSpace graph_space, std::unique_ptr<MBuilder> mbuilder,
                         std::unique_ptr<mfem::SparseMatrix> D,
                         std::unique_ptr<mfem::SparseMatrix> W,
                         mfem::Vector constant_rep)
    : graph_space_(std::move(graph_space)),
      D_(std::move(D)), W_(std::move(W)), edge_d_td_(&graph_space_.EDofToTrueEDof()),
      edge_td_d_(edge_d_td_->Transpose()), mbuilder_(std::move(mbuilder)),
      constant_rep_(std::move(constant_rep))
{
    GenerateRowStarts();
}

void MixedMatrix::SetMFromWeightVector(const mfem::Vector& weight)
{
    const int nedges = weight.Size();

    int* M_fine_i = new int [nedges + 1];
    int* M_fine_j = new int [nedges];
    double* M_fine_data = new double [nedges];
    std::iota(M_fine_i, M_fine_i + nedges + 1, 0);
    std::iota(M_fine_j, M_fine_j + nedges, 0);
    std::copy_n(weight.GetData(), nedges, M_fine_data);

    for (int i = 0; i < nedges; i++)
    {
        assert(M_fine_data[i] != 0.0);
        M_fine_data[i] = 1.0 / std::fabs(M_fine_data[i]);
    }

    M_ = make_unique<mfem::SparseMatrix>(M_fine_i, M_fine_j, M_fine_data,
                                         nedges, nedges);
}

void MixedMatrix::ScaleM(const mfem::Vector& weight)
{
    M_->ScaleRows(weight);
}

void MixedMatrix::UpdateM(const mfem::Vector& agg_weights_inverse)
{
    assert(mbuilder_);
    M_ = mbuilder_->BuildAssembledM(agg_weights_inverse);
}

/// @todo better documentation of the 1/-1 issue, make it optional?
void MixedMatrix::Init(const mfem::SparseMatrix& vertex_edge,
                       const std::vector<mfem::Vector>& edge_weight_split,
                       const mfem::SparseMatrix& w_block)
{
    const int nvertices = vertex_edge.Height();

    //    SetMFromWeightVector(weight);
    mbuilder_ = make_unique<ElementMBuilder>(edge_weight_split, vertex_edge);
    M_ = mbuilder_->BuildAssembledM();

    if (w_block.Height() == nvertices && w_block.Width() == nvertices)
    {
        W_ = make_unique<mfem::SparseMatrix>(w_block);
        (*W_) *= -1.0;
    }

    D_ = ConstructD(vertex_edge, *edge_d_td_);
    GenerateRowStarts();

    constant_rep_.SetSize(D_->NumRows());
    constant_rep_ = 1.0;
}

void MixedMatrix::GenerateRowStarts()
{
    const int nvertices = D_->Height();
    MPI_Comm comm = edge_d_td_->GetComm();
    Drow_start_ = make_unique<mfem::Array<HYPRE_Int>>();
    GenerateOffsets(comm, nvertices, *Drow_start_);
}

unique_ptr<mfem::BlockVector> MixedMatrix::SubVectorsToBlockVector(
    const mfem::Vector& vec_u, const mfem::Vector& vec_p) const
{
    auto blockvec = make_unique<mfem::BlockVector>(GetBlockOffsets());
    blockvec->GetBlock(0) = vec_u;
    blockvec->GetBlock(1) = vec_p;
    return blockvec;
}

mfem::Array<int>& MixedMatrix::GetBlockOffsets() const
{
    if (!blockOffsets_)
    {
        blockOffsets_ = make_unique<mfem::Array<int>>(3);
        (*blockOffsets_)[0] = 0;
        (*blockOffsets_)[1] = edge_d_td_->GetNumRows();
        (*blockOffsets_)[2] = (*blockOffsets_)[1] + D_->Height();
    }

    return *blockOffsets_;
}

mfem::Array<int>& MixedMatrix::GetBlockTrueOffsets() const
{
    if (!blockTrueOffsets_)
    {
        blockTrueOffsets_ = make_unique<mfem::Array<int>>(3);
        (*blockTrueOffsets_)[0] = 0;
        (*blockTrueOffsets_)[1] = edge_d_td_->GetNumCols();
        (*blockTrueOffsets_)[2] = (*blockTrueOffsets_)[1] + D_->Height();
    }

    return *(blockTrueOffsets_);
}

bool MixedMatrix::CheckW() const
{
    const double zero_tol = 1e-6;

    mfem::HypreParMatrix* W = GetParallelW();

    return W && MaxNorm(*W) > zero_tol;
}

std::unique_ptr<mfem::SparseMatrix> MixedMatrix::ConstructD(
    const mfem::SparseMatrix& vertex_edge, const mfem::HypreParMatrix& edge_trueedge)
{
    // Nonzero row of edge_owned means the edge is owned by the local proc
    mfem::SparseMatrix edge_owned;
    edge_trueedge.GetDiag(edge_owned);

    mfem::SparseMatrix graphDT(smoothg::Transpose(vertex_edge));

    // Change the second entries of each row with two nonzeros to 1
    // Change the only entry in the row corresponding to a shared edge that
    // is not owned by the local processor to -1
    int* graphDT_i = graphDT.GetI();
    double* graphDT_data = graphDT.GetData();

    for (int j = 0; j < graphDT.Height(); j++)
    {
        const int row_size = graphDT.RowSize(j);
        assert(row_size == 1 || row_size == 2);

        graphDT_data[graphDT_i[j]] = 1.;

        if (row_size == 2)
        {
            graphDT_data[graphDT_i[j] + 1] = -1.;
        }
        else if (edge_owned.RowSize(j) == 0)
        {
            assert(row_size == 1);
            graphDT_data[graphDT_i[j]] = -1.;
        }
    }
    return unique_ptr<mfem::SparseMatrix>(mfem::Transpose(graphDT));
}

} // namespace smoothg

#endif /* __MIXEDMATRIX_IMPL_HPP__ */
