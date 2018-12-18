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
#include "pde.hpp"

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
    int spe10_scale = 5;
    args.AddOption(&spe10_scale, "-sc", "--spe10-scale",
                   "Scale of problem, 1=small, 5=full SPE10.");
    bool visualization = false;
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization", "Enable visualization.");
    bool lateral_pressure = false;
    args.AddOption(&lateral_pressure, "-lat-pres", "--lateral-pressure",
                   "-no-lat-pres", "--no-lateral-pressure",
                   "Impose Dirichlet pressure condition on lateral sides.");
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

    mfem::Array<int> coarsening_factors(nDimensions);
    coarsening_factors = 10;
    coarsening_factors.Last() = nDimensions == 3 ? 5 : 10;

    mfem::Array<int> ess_attr(nDimensions == 3 ? 6 : 4);
    ess_attr = 1;
    if (lateral_pressure)
    {
        ess_attr[0] = ess_attr[2] = 0;
    }

    // Setting up finite volume discretization problem
    SPE10Problem spe10problem(permFile, nDimensions, spe10_scale, slice,
                              metis_agglomeration, ess_attr);
    Graph graph = spe10problem.GetFVGraph();

    // Construct agglomerated topology based on METIS or Cartesian agglomeration
    mfem::Array<int> partitioning;
    spe10problem.Partition(metis_agglomeration, coarsening_factors, partitioning);

    // Create Upscaler and Solve
    Upscale upscale(graph, upscale_param, &partitioning, &ess_attr);

    upscale.PrintInfo();
    upscale.ShowSetupTime();

    mfem::BlockVector rhs_fine(upscale.GetBlockVector(0));
    rhs_fine.GetBlock(0) = spe10problem.GetEdgeRHS();
    rhs_fine.GetBlock(1) = spe10problem.GetVertexRHS();

    /// [Solve]
    std::vector<mfem::BlockVector> sol(upscale_param.max_levels, rhs_fine);
    for (int level = 0; level < upscale_param.max_levels; ++level)
    {
        upscale.Solve(level, rhs_fine, sol[level]);
        upscale.ShowSolveInfo(level);

        if (lateral_pressure)
        {
            double QoI = mfem::InnerProduct(comm, sol[level], rhs_fine);
            if (myid == 0)
            {
                std::cout << "Quantity of interest on level " << level
                          << " = " << QoI << "\n";
            }
        }

        auto error_info = upscale.ComputeErrors(sol[level], sol[0]);

        if (level > 0 && myid == 0)
        {
            ShowErrors(error_info);
        }

        // Visualize the solution
        if (visualization)
        {
            mfem::socketstream vis_v;
            spe10problem.VisSetup(vis_v, sol[level].GetBlock(1));
        }
    }
    /// [Solve]

    return EXIT_SUCCESS;
}
