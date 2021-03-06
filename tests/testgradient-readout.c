#include <stdio.h>
#include <string.h>
#include <alloca.h>
#include <mpi.h>
#include <math.h>

#include <fastpm/libfastpm.h>
#include <fastpm/logging.h>

static double
chi2(PM * pm, FastPMStore * p)
{
    double sum = 0;
    ptrdiff_t i;
    for(i = 0; i < p->np; i ++) {
        double v = p->to_double(p, i, PACK_ACC_X);
        sum += 0.5 * v * v;
    }
    MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_DOUBLE, MPI_SUM, pm_comm(pm));
    return sum;
}

static void
chi2_gradient(PM * pm, FastPMStore * p, FastPMStore * grad)
{
    fastpm_store_copy(p, grad);
}

static double
objective(FastPMSolver * solver,  FastPMFloat * field, FastPMStore * p)
{
    FastPMPainter painter[1];
    fastpm_painter_init(painter, solver->basepm, FASTPM_PAINTER_CIC, 2);

    fastpm_readout(painter, field, p, NULL, PACK_ACC_X);

    double s = chi2(solver->basepm, p);

    return s;
}

static void
objective_gradient(FastPMSolver * solver, FastPMFloat * field, FastPMStore * p, FastPMFloat * grad_field, FastPMStore * grad_pos)
{
    FastPMStore tmp[1];
    fastpm_store_init(tmp, p->np, p->attributes);

    /* redo rho_init_x, since we do not record rho_init_x on a tape */
    FastPMPainter painter[1];
    fastpm_painter_init(painter, solver->basepm, FASTPM_PAINTER_CIC, 2);

    fastpm_readout(painter, field, p, NULL, PACK_ACC_X);

    chi2_gradient(solver->basepm, p, tmp);

    fastpm_readout_gradient(painter, tmp, field, p, NULL, PACK_ACC_X, grad_field, grad_pos);

    fastpm_store_destroy(tmp);
}

int main(int argc, char * argv[]) {

    MPI_Init(&argc, &argv);

    libfastpm_init();

    MPI_Comm comm = MPI_COMM_WORLD;

    fastpm_set_msg_handler(fastpm_default_msg_handler, comm, NULL);

    FastPMConfig * config = & (FastPMConfig) {
        .nc = 8,
        .boxsize = 8.,
        .alloc_factor = 2.0,
        .omega_m = 0.292,
        .vpminit = (VPMInit[]) {
            {.a_start = 0, .pm_nc_factor = 2},
            {.a_start = -1, .pm_nc_factor = 0},
        },
        .FORCE_TYPE = FASTPM_FORCE_FASTPM,
        .nLPT = 2.5,
        .K_LINEAR = 0.04,
    };

    FastPMSolver solver[1];
    fastpm_solver_init(solver, config, comm);
    FastPMFloat * rho_init_ktruth = pm_alloc(solver->basepm);

    /* First establish the truth by 2lpt -- this will be replaced with PM. */
    struct fastpm_powerspec_eh_params eh = {
        .Norm = 50000.0, /* FIXME: this is not any particular sigma8. */
        .hubble_param = 0.7,
        .omegam = 0.260,
        .omegab = 0.044,
    };
    fastpm_ic_fill_gaussiank(solver->basepm, rho_init_ktruth, 2004, FASTPM_DELTAK_GADGET);
    fastpm_ic_induce_correlation(solver->basepm, rho_init_ktruth, (fastpm_fkfunc)fastpm_utils_powerspec_eh, &eh);
    fastpm_solver_setup_ic(solver, rho_init_ktruth);
    //fastpm_solver_evolve(solver, (double[]){1.0, }, 1);

    FastPMFloat * gradient1 = pm_alloc(solver->basepm);

    FastPMStore gradient2[1];
    fastpm_store_init(gradient2, solver->p->np, solver->p->attributes);

    pm_c2r(solver->basepm, rho_init_ktruth);
    ptrdiff_t i;
    for(i = 0; i < solver->p->np; i ++) {
        solver->p->x[i][0] += 0.5;
        solver->p->x[i][1] += 0.5;
        solver->p->x[i][2] += 0.5;
    }

    for(i = 0; i < pm_allocsize(solver->basepm); i ++) {
        //rho_init_ktruth[i] = 1.0;
    }

    double obj1 = objective(solver, rho_init_ktruth, solver->p);
    int d;
    for(i = 0; i < 4; i ++) {
        for(d = 0; d < 3; d ++) {
            solver->p->x[0][d] += 1e-2;
            double obj2 = objective(solver, rho_init_ktruth, solver->p);
            solver->p->x[0][d] -= 1e-2;

            fastpm_info("objective1 = %.18e\n", obj1);
            fastpm_info("objective2 = %.18e\n", obj2);

            objective_gradient(solver, rho_init_ktruth, solver->p, gradient1, gradient2);
            fastpm_info("gradient2 ad  = %g\n", gradient2->x[0][d]);
            fastpm_info("gradient2 num = %g\n", (obj2 - obj1) / 1e-2);

        }
    }
    for(i = 0; i < 8; i ++) {
        double old = rho_init_ktruth[i];
        rho_init_ktruth[i] += 1e-2 * old;
        double obj3 = objective(solver, rho_init_ktruth, solver->p);
        rho_init_ktruth[i] = old;
        fastpm_info("objective3 = %.18e\n", obj3);
        fastpm_info("gradient1 ad  = %g\n", gradient1[i]);
        fastpm_info("gradient1 num = %g\n", (obj3 - obj1) / (1e-2 * old));
    }
    fastpm_store_destroy(gradient2);
    pm_free(solver->basepm, gradient1);
    pm_free(solver->basepm, rho_init_ktruth);
    fastpm_solver_destroy(solver);
    libfastpm_cleanup();
    MPI_Finalize();
    return 0;
}

