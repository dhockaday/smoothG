/*BHEADER**********************************************************************
 *
 * Copyright (c) 2017,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * LLNL-CODE-XXXXXX. All Rights reserved.
 *
 * This file is part of smoothG.  See file COPYRIGHT for details.
 * For more information and source code availability see XXXXX.
 *
 * smoothG is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 ***********************************************************************EHEADER*/

/** @file

    @brief Implements Graph object.
*/

#include "Graph.hpp"
#include "MetisGraphPartitioner.hpp"

#if SMOOTHG_USE_PARMETIS
#include "ParMetisGraphPartitioner.hpp"
#endif

using std::shared_ptr;
using std::unique_ptr;

namespace smoothg
{

Graph::Graph(MPI_Comm comm,
             const mfem::SparseMatrix& vertex_edge_global,
             const mfem::Vector& edge_weight_global)
{
    Distribute(comm, vertex_edge_global, edge_weight_global);
}

Graph::Graph(const mfem::SparseMatrix& vertex_edge_local,
             const mfem::HypreParMatrix& edge_trueedge,
             const mfem::Vector& edge_weight_local)
    : vertex_edge_local_(vertex_edge_local)
{
    // temporary work-around (TODO: make a copy function for HypreParMatrix)
    unique_ptr<mfem::HypreParMatrix> trueedge_edge(edge_trueedge.Transpose());
    edge_trueedge_.reset(trueedge_edge->Transpose());

    SplitEdgeWeight(edge_weight_local);
}

Graph::Graph(const mfem::SparseMatrix& vertex_edge_local,
             const mfem::HypreParMatrix& edge_trueedge,
             const std::vector<mfem::Vector>& edge_weight_split)
    : vertex_edge_local_(vertex_edge_local), edge_weight_split_(edge_weight_split)
{
    // temporary work-around (TODO: make a copy function for HypreParMatrix)
    unique_ptr<mfem::HypreParMatrix> trueedge_edge(edge_trueedge.Transpose());
    edge_trueedge_.reset(trueedge_edge->Transpose());
}

Graph::Graph(Graph&& other) noexcept
{
    swap(*this, other);
}

Graph& Graph::operator=(Graph other) noexcept
{
    swap(*this, other);

    return *this;
}

void swap(Graph& lhs, Graph& rhs) noexcept
{
    lhs.vertex_edge_local_.Swap(rhs.vertex_edge_local_);
    std::swap(lhs.edge_weight_split_, rhs.edge_weight_split_);
    std::swap(lhs.edge_trueedge_, rhs.edge_trueedge_);
    std::swap(lhs.vertex_trueedge_, rhs.vertex_trueedge_);

    mfem::Swap(lhs.vert_loc_to_glo_, rhs.vert_loc_to_glo_);
    mfem::Swap(lhs.edge_loc_to_glo_, rhs.edge_loc_to_glo_);
    mfem::Swap(lhs.vertex_starts_, rhs.vertex_starts_);
}

void Graph::Distribute(MPI_Comm comm,
                       const mfem::SparseMatrix& vertex_edge_global,
                       const mfem::Vector& edge_weight_global)
{
    DistributeVertexEdge(comm, vertex_edge_global);
    mfem::Vector edge_weight_local = DistributeEdgeWeight(edge_weight_global);
    SplitEdgeWeight(edge_weight_local);
}

void Graph::DistributeVertexEdge(MPI_Comm comm,
                                 const mfem::SparseMatrix& vert_edge_global)
{
    MFEM_VERIFY(HYPRE_AssumedPartitionCheck(),
                "this method can not be used without assumed partition");

    int num_procs;
    int myid;
    MPI_Comm_size(comm, &num_procs);
    MPI_Comm_rank(comm, &myid);

    mfem::SparseMatrix vert_vert = AAt(vert_edge_global);
    mfem::Array<int> partition;
    Partition(vert_vert, partition, num_procs);

    // Construct processor to vertex/edge from global partition
    mfem::SparseMatrix proc_vert = PartitionToMatrix(partition, num_procs);
    mfem::SparseMatrix proc_edge = smoothg::Mult(proc_vert, vert_edge_global);
    proc_edge.SortColumnIndices(); // TODO: this may not be needed once SEC is fixed

    // Construct vertex/edge local to global index array
    GetTableRowCopy(proc_vert, myid, vert_loc_to_glo_);
    GetTableRowCopy(proc_edge, myid, edge_loc_to_glo_);

    // Extract local submatrix of the global vertex to edge relation table
    auto tmp = ExtractRowAndColumns(vert_edge_global, vert_loc_to_glo_, edge_loc_to_glo_);
    vertex_edge_local_.Swap(tmp);

    MakeEdgeTrueEdge(comm, myid, proc_edge);

    // Compute vertex_trueedge (needed for redistribution)
    GenerateOffsets(comm, vertex_edge_local_.Height(), vertex_starts_);
    //    vertex_trueedge_.reset(
    //        edge_trueedge_->LeftDiagMult(vertex_edge_local_, vertex_starts_) );
}

void Graph::MakeEdgeTrueEdge(MPI_Comm comm, int myid, const mfem::SparseMatrix& proc_edge)
{
    const int num_procs = proc_edge.Height();
    const int nedges_local = proc_edge.RowSize(myid);

    mfem::SparseMatrix edge_proc = smoothg::Transpose(proc_edge);

    // Count number of true edges in each processor
    int ntedges_global = proc_edge.Width();
    mfem::Array<int> tedge_couters(num_procs + 1);
    tedge_couters = 0;
    for (int i = 0; i < ntedges_global; i++)
        tedge_couters[edge_proc.GetRowColumns(i)[0] + 1]++;
    int ntedges_local = tedge_couters[myid + 1];
    tedge_couters.PartialSum();
    assert(tedge_couters.Last() == ntedges_global);

    // Renumber true edges so that the new numbering is contiguous in processor
    mfem::Array<int> tedge_old2new(ntedges_global);
    for (int i = 0; i < ntedges_global; i++)
        tedge_old2new[i] = tedge_couters[edge_proc.GetRowColumns(i)[0]]++;

    // Construct edge to true edge table
    int* e_te_diag_i = new int[nedges_local + 1];
    int* e_te_diag_j = new int[ntedges_local];
    double* e_te_diag_data = new double[ntedges_local];
    e_te_diag_i[0] = 0;
    std::fill_n(e_te_diag_data, ntedges_local, 1.0);

    assert(nedges_local - ntedges_local >= 0);
    int* e_te_offd_i = new int[nedges_local + 1];
    int* e_te_offd_j = new int[nedges_local - ntedges_local];
    double* e_te_offd_data = new double[nedges_local - ntedges_local];
    HYPRE_Int* e_te_col_map = new HYPRE_Int[nedges_local - ntedges_local];
    e_te_offd_i[0] = 0;
    std::fill_n(e_te_offd_data, nedges_local - ntedges_local, 1.0);

    for (int i = num_procs - 1; i > 0; i--)
        tedge_couters[i] = tedge_couters[i - 1];
    tedge_couters[0] = 0;

    mfem::Array<mfem::Pair<HYPRE_Int, int> > offdmap_pair(
        nedges_local - ntedges_local);

    int tedge_new;
    int tedge_begin = tedge_couters[myid];
    int tedge_end = tedge_couters[myid + 1];
    int diag_counter(0), offd_counter(0);
    for (int i = 0; i < nedges_local; i++)
    {
        tedge_new = tedge_old2new[edge_loc_to_glo_[i]];
        if ( (tedge_new >= tedge_begin) && (tedge_new < tedge_end) )
        {
            e_te_diag_j[diag_counter++] = tedge_new - tedge_begin;
        }
        else
        {
            offdmap_pair[offd_counter].two = offd_counter;
            offdmap_pair[offd_counter++].one = tedge_new;
        }
        e_te_diag_i[i + 1] = diag_counter;
        e_te_offd_i[i + 1] = offd_counter;
    }
    assert(offd_counter == nedges_local - ntedges_local);

    // Entries of the offd_col_map for edge_e_te_ should be in ascending order
    mfem::SortPairs<HYPRE_Int, int>(offdmap_pair, offd_counter);

    for (int i = 0; i < offd_counter; i++)
    {
        e_te_offd_j[offdmap_pair[i].two] = i;
        e_te_col_map[i] = offdmap_pair[i].one;
    }

    // Generate the "start" array for edge and true edge
    mfem::Array<HYPRE_Int> edge_starts, tedge_starts;
    mfem::Array<HYPRE_Int>* starts[2] = {&edge_starts, &tedge_starts};
    HYPRE_Int size[2] = {nedges_local, ntedges_local};
    GenerateOffsets(comm, 2, size, starts);

    edge_trueedge_ = make_unique<mfem::HypreParMatrix>(
                         comm, edge_starts.Last(), ntedges_global, edge_starts, tedge_starts,
                         e_te_diag_i, e_te_diag_j, e_te_diag_data,
                         e_te_offd_i, e_te_offd_j, e_te_offd_data, offd_counter, e_te_col_map);
    edge_trueedge_->CopyRowStarts();
    edge_trueedge_->CopyColStarts();
}

mfem::Vector Graph::DistributeEdgeWeight(const mfem::Vector& edge_weight_global)
{
    mfem::Vector edge_weight_local(vertex_edge_local_.Width());
    if (edge_weight_global.Size())
    {
        edge_weight_global.GetSubVector(edge_loc_to_glo_, edge_weight_local);
    }
    else
    {
        edge_weight_local = 1.0;
    }

    // for edges shared by two processes, multiply the weight by 2 (M is divided by 2)
    unique_ptr<mfem::HypreParMatrix> e_te_e = AAt(*edge_trueedge_);
    mfem::SparseMatrix edge_is_shared;
    HYPRE_Int* junk_map;
    e_te_e->GetOffd(edge_is_shared, junk_map);

    assert(edge_is_shared.Height() == edge_weight_local.Size());
    for (int edge = 0; edge < edge_is_shared.Height(); ++edge)
    {
        if (edge_is_shared.RowSize(edge))
        {
            edge_weight_local[edge] *= 2.0;
        }
    }

    return edge_weight_local;
}

void Graph::SplitEdgeWeight(const mfem::Vector& edge_weight_local)
{
    // for edges having two vertices, multiply the weight by 2 (M is divided by 2)
    const mfem::SparseMatrix edge_vert = smoothg::Transpose(vertex_edge_local_);
    edge_weight_split_.resize(edge_vert.Width());

    mfem::Array<int> edges;
    for (int vert = 0; vert < edge_vert.Width(); vert++)
    {
        GetTableRow(vertex_edge_local_, vert, edges);
        edge_weight_split_[vert].SetSize(edges.Size());
        for (int i = 0; i < edges.Size(); i++)
        {
            const int edge = edges[i];
            double ratio = edge_vert.RowSize(edge) == 2 ? 2.0 : 1.0;
            edge_weight_split_[vert][i] = edge_weight_local[edge] * ratio;
        }
    }
}

mfem::Vector Graph::ReadVertexVector(const std::string& filename) const
{
    assert(vert_loc_to_glo_.Size() == vertex_edge_local_.Height());
    return ReadVector(filename, vertex_starts_.Last(), vert_loc_to_glo_);
}

mfem::Vector Graph::ReadVector(const std::string& filename, int global_size,
                               const mfem::Array<int>& local_to_global) const
{
    assert(global_size > 0);

    std::ifstream file(filename);
    assert(file.is_open());

    mfem::Vector global_vect(global_size);
    mfem::Vector local_vect;

    global_vect.Load(file, global_size);
    global_vect.GetSubVector(local_to_global, local_vect);

    return local_vect;
}

void Graph::WriteVertexVector(const mfem::Vector& vec_loc, const std::string& filename) const
{
    assert(vert_loc_to_glo_.Size() == vertex_edge_local_.Height());
    WriteVector(vec_loc, filename, vertex_starts_.Last(), vert_loc_to_glo_);
}

void Graph::WriteVector(const mfem::Vector& vect, const std::string& filename,
                        int global_size, const mfem::Array<int>& local_to_global) const
{
    assert(global_size > 0);
    assert(vect.Size() <= global_size);

    int num_procs;
    int myid;
    MPI_Comm_size(GetComm(), &num_procs);
    MPI_Comm_rank(GetComm(), &myid);

    mfem::Vector global_local(global_size);
    global_local = 0.0;
    global_local.SetSubVector(local_to_global, vect);

    mfem::Vector global_global(global_size);
    MPI_Scan(global_local.GetData(), global_global.GetData(), global_size,
             MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    if (myid == num_procs - 1)
    {
        std::ofstream out_file(filename);
        out_file.precision(16);
        out_file << std::scientific;
        global_global.Print(out_file, 1);
    }
}

} // namespace smoothg
