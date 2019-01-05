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
   @file finitevolume.cpp
   @brief This is an example for upscaling a graph Laplacian coming from a finite
   volume discretization of a simple reservior model in parallel.

   A simple way to run the example:

   mpirun -n 4 ./finitevolume
*/

#include <fstream>
#include <sstream>
#include <mpi.h>

#include "mfem.hpp"
#include "spe10.hpp"

#include "../src/picojson.h"
#include "../src/smoothG.hpp"

using namespace smoothg;

int main(int argc, char* argv[])
{
    int num_procs, myid;
    picojson::object serialize;

    // 1. Initialize MPI
    mpi_session session(argc, argv);
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &num_procs);
    MPI_Comm_rank(comm, &myid);

    // program options from command line
    UpscaleParameters upscale_param;
    mfem::OptionsParser args(argc, argv);
    const char* permFile = "spe_perm.dat";
    args.AddOption(&permFile, "-p", "--perm",
                   "SPE10 permeability file data.");
    int nDimensions = 2;
    args.AddOption(&nDimensions, "-d", "--dim",
                   "Dimension of the physical space.");
    int slice = 0;
    args.AddOption(&slice, "-s", "--slice",
                   "Slice of SPE10 data to take for 2D run.");
    bool metis_agglomeration = false;
    args.AddOption(&metis_agglomeration, "-ma", "--metis-agglomeration",
                   "-nm", "--no-metis-agglomeration",
                   "Use Metis as the partitioner (instead of geometric).");
    double proc_part_ubal = 2.0;
    args.AddOption(&proc_part_ubal, "-pub", "--part-unbalance",
                   "Processor partition unbalance factor.");
    int spe10_scale = 5;
    args.AddOption(&spe10_scale, "-sc", "--spe10-scale",
                   "Scale of problem, 1=small, 5=full SPE10.");
    bool visualization = false;
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization", "Enable visualization.");
    // Read upscaling options from command line into upscale_param object
    upscale_param.RegisterInOptionsParser(args);
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

    mfem::Array<int> coarseningFactor(nDimensions);
    coarseningFactor[0] = 10;
    coarseningFactor[1] = 10;
    if (nDimensions == 3)
        coarseningFactor[2] = 5;

    int nbdr;
    if (nDimensions == 3)
        nbdr = 6;
    else
        nbdr = 4;
    mfem::Array<int> ess_zeros(nbdr);
    mfem::Array<int> nat_one(nbdr);
    mfem::Array<int> nat_zeros(nbdr);
    ess_zeros = 1;
    nat_one = 0;
    nat_zeros = 0;

    mfem::Array<int> ess_attr;
    mfem::Vector weight;
    mfem::Vector rhs_u_fine;

    // Setting up finite volume discretization problem
    SPE10Problem spe10problem(permFile, nDimensions, spe10_scale, slice,
                              metis_agglomeration, proc_part_ubal, coarseningFactor);

    mfem::ParMesh* pmesh = spe10problem.GetParMesh();

    if (myid == 0)
    {
        std::cout << pmesh->GetNEdges() << " fine edges, " <<
                  pmesh->GetNFaces() << " fine faces, " <<
                  pmesh->GetNE() << " fine elements\n";
    }

    ess_attr.SetSize(nbdr);
    for (int i(0); i < nbdr; ++i)
        ess_attr[i] = ess_zeros[i];

    // Construct "finite volume mass" matrix using mfem instead of parelag
    mfem::RT_FECollection sigmafec(0, nDimensions);
    mfem::ParFiniteElementSpace sigmafespace(pmesh, &sigmafec);

    mfem::ParBilinearForm a(&sigmafespace);
    a.AddDomainIntegrator(
        new FiniteVolumeMassIntegrator(*spe10problem.GetKInv()) );
    a.Assemble();
    a.Finalize();
    a.SpMat().GetDiag(weight);

    for (int i = 0; i < weight.Size(); ++i)
    {
        weight[i] = 1.0 / weight[i];
    }

    mfem::L2_FECollection ufec(0, nDimensions);
    mfem::ParFiniteElementSpace ufespace(pmesh, &ufec);

    mfem::LinearForm q(&ufespace);
    q.AddDomainIntegrator(
        new mfem::DomainLFIntegrator(*spe10problem.GetForceCoeff()) );
    q.Assemble();
    rhs_u_fine = q;

    // Construct vertex_edge table in mfem::SparseMatrix format
    auto& vertex_edge_table = nDimensions == 2 ? pmesh->ElementToEdgeTable()
                              : pmesh->ElementToFaceTable();
    mfem::SparseMatrix vertex_edge = TableToMatrix(vertex_edge_table);

    // Construct agglomerated topology based on METIS or Cartesian agglomeration
    mfem::Array<int> partitioning;
    if (metis_agglomeration)
    {
        FESpaceMetisPartition(partitioning, sigmafespace, ufespace, coarseningFactor);
    }
    else
    {
        auto num_procs_xyz = spe10problem.GetNumProcsXYZ();
        FVMeshCartesianPartition(partitioning, num_procs_xyz, *pmesh, coarseningFactor);
    }

    const auto& edge_d_td(sigmafespace.Dof_TrueDof_Matrix());

    auto edge_boundary_att = GenerateBoundaryAttributeTable(pmesh);

    Graph graph(vertex_edge, *edge_d_td, weight, &edge_boundary_att);

    // Create Upscaler and Solve
    Upscale upscale(graph, upscale_param, &partitioning, &ess_attr);

    upscale.PrintInfo();
    upscale.ShowSetupTime();

    mfem::BlockVector rhs_fine(upscale.GetBlockVector(0));
    rhs_fine.GetBlock(0) = 0.0;
    rhs_fine.GetBlock(1) = rhs_u_fine;

    /// [Solve]
    std::vector<mfem::BlockVector> sol(upscale_param.max_levels, rhs_fine);
    for (int level = 0; level < upscale_param.max_levels; ++level)
    {
        upscale.Solve(level, rhs_fine, sol[level]);
        upscale.ShowSolveInfo(level);

        if (level > 0)
        {
            upscale.ShowErrors(sol[level], sol[0], level);
        }
    }
    /// [Solve]

    // Visualize the solution
    if (visualization)
    {
        mfem::ParGridFunction field(&ufespace);

        auto Visualize = [&](const mfem::Vector & sol)
        {
            char vishost[] = "localhost";
            int  visport   = 19916;

            mfem::socketstream vis_v;
            vis_v.open(vishost, visport);
            vis_v.precision(8);

            field = sol;

            vis_v << "parallel " << pmesh->GetNRanks() << " " << pmesh->GetMyRank() << "\n";
            vis_v << "solution\n" << *pmesh << field;
            vis_v << "window_size 500 800\n";
            vis_v << "window_title 'pressure'\n";
            vis_v << "autoscale values\n";

            if (nDimensions == 2)
            {
                vis_v << "view 0 0\n"; // view from top
                vis_v << "keys ]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\n";  // increase size
            }

            vis_v << "keys cjl\n";

            MPI_Barrier(comm);
        };

        for (int level = 0; level < upscale_param.max_levels; ++level)
        {
            Visualize(sol[level].GetBlock(1));
        }
    }

    return EXIT_SUCCESS;
}
