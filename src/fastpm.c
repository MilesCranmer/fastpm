#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <alloca.h>
#include <mpi.h>
#include <math.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fastpm/libfastpm.h>
#include <fastpm/prof.h>
#include <fastpm/logging.h>
#include <fastpm/string.h>

#include "lua-config.h"

typedef struct {
    int UseFFTW;
    int NprocY;
    int Nwriters;
    LuaConfig * config;
    char * string;
} Parameters;

extern char * 
run_paramfile(char * filename, int runmain, int argc, char ** argv);

#define CONF(prr, name) lua_config_get_ ## name (prr->config)

/* command-line arguments */
static char * ParamFileName;

static void 
parse_args(int * argc, char *** argv, Parameters * prr);

static int 
take_a_snapshot(FastPM * fastpm, PMStore * snapshot, double aout, Parameters * prr);

static void 
_mkdir(const char *dir);
static void 
ensure_dir(const char * path);

int 
read_runpb_ic(FastPM * fastpm, PMStore * p, const char * filename);

void 
read_grafic_gaussian(PM * pm, FastPMFloat * g_x, const char * filename);

int
write_runpb_snapshot(FastPM * fastpm, PMStore * p, const char * filebase);

int
write_snapshot(FastPM * fastpm, PMStore * p, const char * filebase, char * parameters, int Nwriters);

int
read_snapshot(FastPM * fastpm, PMStore * p, const char * filebase);

int
read_parameters(char * filename, Parameters * param, int argc, char ** argv, MPI_Comm comm);

int
read_powerspectrum(FastPMPowerSpectrum *ps, const char filename[], const double sigma8, MPI_Comm comm);

int run_fastpm(FastPM * fastpm, Parameters * prr, MPI_Comm comm);

int main(int argc, char ** argv) {

    MPI_Init(&argc, &argv);

    Parameters * prr = alloca(sizeof(prr[0]));

    parse_args(&argc, &argv, prr);

    MPI_Comm comm = MPI_COMM_WORLD; 

    libfastpm_init();

    fastpm_set_msg_handler(fastpm_default_msg_handler, comm, NULL);

    read_parameters(ParamFileName, prr, argc, argv, comm);

    /* convert parameter files pm_nc_factor into VPMInit */
    VPMInit * vpminit = alloca(sizeof(VPMInit) * (CONF(prr, n_pm_nc_factor) + 1));
    int i;
    for(i = 0; i < CONF(prr, n_pm_nc_factor); i ++) {
        vpminit[i].a_start = CONF(prr, change_pm)[i];
        vpminit[i].pm_nc_factor = CONF(prr, pm_nc_factor)[i];
    }
    /* mark the end */
    vpminit[i].pm_nc_factor = 0;

    fastpm_info("np_alloc_factor = %g\n", CONF(prr, np_alloc_factor));

    FastPM * fastpm = & (FastPM) {
        .nc = CONF(prr, nc),
        .alloc_factor = CONF(prr, np_alloc_factor),
        .vpminit = vpminit,
        .boxsize = CONF(prr, boxsize),
        .omega_m = CONF(prr, omega_m),
        .USE_NONSTDDA = !CONF(prr, cola_stdda),
        .USE_DX1_ONLY = CONF(prr, za),
        .nLPT = -2.5f,
        .K_LINEAR = CONF(prr, enforce_broadband_kmax),
        .USE_SHIFT = 1, /* compatible with old behavior, shift particles by 0.5 mesh*/
        .FORCE_TYPE = CONF(prr, force_mode),
        .USE_MODEL = CONF(prr, enforce_broadband_mode),
        .KERNEL_TYPE = CONF(prr, kernel_type),
        .DEALIASING_TYPE = CONF(prr, dealiasing_type),
    };

    run_fastpm(fastpm, prr, comm);

    libfastpm_cleanup();

    MPI_Finalize();
    return 0;
}

static int 
check_snapshots(FastPM * fastpm, void * unused, Parameters * prr);

static int 
write_powerspectrum(FastPM * fastpm, FastPMFloat * delta_k, double a_x, Parameters * prr);

static void 
prepare_ic(FastPM * fastpm, Parameters * prr, MPI_Comm comm);

int run_fastpm(FastPM * fastpm, Parameters * prr, MPI_Comm comm) {
    CLOCK(init);
    CLOCK(ic);
    CLOCK(evolve);

    const double rho_crit = 27.7455;
    const double M0 = CONF(prr, omega_m) * rho_crit
                    * pow(CONF(prr, boxsize) / CONF(prr, nc), 3.0);
    fastpm_info("mass of a particle is %g 1e10 Msun/h\n", M0); 

    MPI_Barrier(comm);
    ENTER(init);

    fastpm_init(fastpm, 
        prr->NprocY, prr->UseFFTW, 
        comm);

    LEAVE(init);

    fastpm_add_extension(fastpm,
        FASTPM_EXT_AFTER_FORCE,
        write_powerspectrum,
        prr);

    fastpm_add_extension(fastpm,
        FASTPM_EXT_BEFORE_KICK,
        check_snapshots,
        prr);

    fastpm_add_extension(fastpm,
        FASTPM_EXT_BEFORE_DRIFT,
        check_snapshots,
        prr);

    MPI_Barrier(comm);
    ENTER(ic);
    prepare_ic(fastpm, prr, comm);
    fastpm_info("dx1  : %g %g %g %g\n", 
            fastpm->info.dx1[0], fastpm->info.dx1[1], fastpm->info.dx1[2],
            (fastpm->info.dx1[0] + fastpm->info.dx1[1] + fastpm->info.dx1[2]) / 3.0);
    fastpm_info("dx2  : %g %g %g %g\n", 
            fastpm->info.dx2[0], fastpm->info.dx2[1], fastpm->info.dx2[2],
            (fastpm->info.dx2[0] + fastpm->info.dx2[1] + fastpm->info.dx2[2]) / 3.0);

    LEAVE(ic);

    MPI_Barrier(comm);
    ENTER(evolve);

    fastpm_evolve(fastpm, CONF(prr, time_step), CONF(prr, n_time_step));

    LEAVE(evolve);

    fastpm_destroy(fastpm);

    fastpm_clock_stat(comm);
    return 0;
}

static void 
prepare_ic(FastPM * fastpm, Parameters * prr, MPI_Comm comm) 
{
    /* we may need a read gadget ic here too */
    if(CONF(prr, read_runpbic)) {
        read_runpb_ic(fastpm, fastpm->p, CONF(prr, read_runpbic));
        fastpm_setup_ic(fastpm, NULL);
        return;
    } 

    /* at this point generating the ic involves delta_k */
    FastPMFloat * delta_k = pm_alloc(fastpm->pm_2lpt);

    if(CONF(prr, read_lineark)) {
        fastpm_info("Reading Fourier space linear overdensity from %s\n", CONF(prr, read_lineark));
        fastpm_utils_load(fastpm->pm_2lpt, CONF(prr, read_lineark), delta_k);

        if(CONF(prr, inverted_ic)) {
            fastpm_apply_multiply_transfer(fastpm->pm_2lpt, delta_k, delta_k, -1);
        }
        goto produce;
    }

    /* at this power we need a powerspectrum file to convolve the guassian */
    if(!CONF(prr, read_powerspectrum)) {
        fastpm_raise(-1, "Need a power spectrum to start the simulation.\n");
    }

    FastPMPowerSpectrum linear_powerspectrum;

    read_powerspectrum(&linear_powerspectrum, CONF(prr, read_powerspectrum), CONF(prr, sigma8), comm);

    if(CONF(prr, read_grafic)) {
        fastpm_info("Reading grafic white noise file from '%s'.\n", CONF(prr, read_grafic));
        fastpm_info("GrafIC noise is Fortran ordering. FastPM is in C ordering.\n");
        fastpm_info("The simulation will be transformed x->z y->y z->x.\n");

        FastPMFloat * g_x = pm_alloc(fastpm->pm_2lpt);

        read_grafic_gaussian(fastpm->pm_2lpt, g_x, CONF(prr, read_grafic));

        pm_r2c(fastpm->pm_2lpt, g_x, delta_k);

        pm_free(fastpm->pm_2lpt, g_x);

        goto induce;
    }

    if(CONF(prr, read_whitenoisek)) {
        fastpm_info("Reading Fourier white noise file from '%s'.\n", CONF(prr, read_whitenoisek));

        fastpm_utils_load(fastpm->pm_2lpt, CONF(prr, read_whitenoisek), delta_k);
        goto induce;
    }

    /* Nothing to read from, just generate a gadget IC with the seed. */

    fastpm_ic_fill_gaussiank(fastpm->pm_2lpt, delta_k, CONF(prr, random_seed), FASTPM_DELTAK_GADGET);

induce:

    if(CONF(prr, remove_cosmic_variance)) {
        fastpm_info("Remove Cosmic variance from initial condition.\n");
        fastpm_ic_remove_variance(fastpm->pm_2lpt, delta_k);
    }

    if(CONF(prr, inverted_ic)) {
        fastpm_apply_multiply_transfer(fastpm->pm_2lpt, delta_k, delta_k, -1);
    }

    if(CONF(prr, write_whitenoisek)) {
        fastpm_info("Writing Fourier white noise to file '%s'.\n", CONF(prr, write_whitenoisek));
        ensure_dir(CONF(prr, write_whitenoisek));
        fastpm_utils_dump(fastpm->pm_2lpt, CONF(prr, write_whitenoisek), delta_k);
    }

    /* FIXME: use enums */
    if(CONF(prr, f_nl_type) == FASTPM_FNL_NONE) {
        fastpm_info("Inducing correlation to the white noise.\n");

        fastpm_ic_induce_correlation(fastpm->pm_2lpt, delta_k,
            (fastpm_fkfunc) fastpm_powerspectrum_eval2, &linear_powerspectrum);
    } else {
        FastPMPNGaussian png = {
            .fNL = CONF(prr, f_nl),
            .type = CONF(prr, f_nl_type),
            .pkfunc = (fastpm_fkfunc) fastpm_powerspectrum_eval2,
            .pkdata = &linear_powerspectrum,
            .h = CONF(prr, h),
            .scalar_amp = CONF(prr, scalar_amp),
            .scalar_spectral_index = CONF(prr, scalar_spectral_index),
            .scalar_pivot = CONF(prr, scalar_pivot)
        };
        fastpm_info("Inducing non gaussian correlation to the white noise.\n");
        fastpm_png_induce_correlation(&png, fastpm->pm_2lpt, delta_k);
    }
    fastpm_powerspectrum_destroy(&linear_powerspectrum);

    /* our write out and clean up stuff.*/
produce:

    if(CONF(prr, write_lineark)) {
        fastpm_info("Writing fourier space linear field to %s\n", CONF(prr, write_lineark));
        ensure_dir(CONF(prr, write_lineark));
        fastpm_utils_dump(fastpm->pm_2lpt, CONF(prr, write_lineark), delta_k);
    }

    fastpm_setup_ic(fastpm, delta_k);

    pm_free(fastpm->pm_2lpt, delta_k);
}

static int check_snapshots(FastPM * fastpm, void * unused, Parameters * prr) {
    fastpm_interp(fastpm, CONF(prr, aout), CONF(prr, n_aout), (fastpm_interp_action)take_a_snapshot, prr);
    return 0;
}

static int 
take_a_snapshot(FastPM * fastpm, PMStore * snapshot, double aout, Parameters * prr) 
{
    CLOCK(io);
    CLOCK(meta);

    if(CONF(prr, write_snapshot)) {
        char filebase[1024];
        double z_out= 1.0/aout - 1.0;
        int Nwriters = prr->Nwriters;
        if(Nwriters == 0) {
            MPI_Comm_size(fastpm->comm, &Nwriters);
        }
        sprintf(filebase, "%s_%0.04f", CONF(prr, write_snapshot), aout);

        fastpm_info("Writing snapshot %s at z = %6.4f a = %6.4f with %d writers\n", 
                filebase, z_out, aout, Nwriters);

        ENTER(meta);
        ensure_dir(filebase);
        LEAVE(meta);

        MPI_Barrier(fastpm->comm);
        ENTER(io);
        write_snapshot(fastpm, snapshot, filebase, prr->string, Nwriters);
        LEAVE(io);

        fastpm_info("snapshot %s written\n", filebase);
    }
    if(CONF(prr, write_runpb_snapshot)) {
        char filebase[1024];
        double z_out= 1.0/aout - 1.0;

        sprintf(filebase, "%s_%0.04f.bin", CONF(prr, write_runpb_snapshot), aout);
        ENTER(meta);
        ensure_dir(filebase);
        LEAVE(meta);

        MPI_Barrier(fastpm->comm);
        ENTER(io);
        write_runpb_snapshot(fastpm, snapshot, filebase);

        LEAVE(io);

        fastpm_info("snapshot %s written z = %6.4f a = %6.4f\n", 
                filebase, z_out, aout);

    }
    if(CONF(prr, write_nonlineark)) {
        char * filename = fastpm_strdup_printf("%s_%0.04f", CONF(prr, write_nonlineark), aout);
        FastPMFloat * rho_k = pm_alloc(fastpm->pm_2lpt);
        fastpm_utils_paint(fastpm->pm_2lpt, snapshot, NULL, rho_k, NULL, 0);
        ensure_dir(filename);
        fastpm_utils_dump(fastpm->pm_2lpt, filename, rho_k);
        pm_free(fastpm->pm_2lpt, rho_k);
        free(filename);
    }
    return 0;
}

static int
write_powerspectrum(FastPM * fastpm, FastPMFloat * delta_k, double a_x, Parameters * prr) 
{
    CLOCK(compute);
    CLOCK(io);

    fastpm_info("==== Step %d a_x = %6.4f a_x1 = %6.4f a_v = %6.4f a_v1 = %6.4f Nmesh = %d ====\n", 
        fastpm->info.istep,
        fastpm->info.a_x,
        fastpm->info.a_x1,
        fastpm->info.a_v,
        fastpm->info.a_v1,
        fastpm->info.Nmesh);

    fastpm_info("Load imbalance is - %g / + %g\n",
        fastpm->info.imbalance.min, fastpm->info.imbalance.max);

    fastpm_report_memory(fastpm->comm);

    MPI_Barrier(fastpm->comm);
    ENTER(compute);

    FastPMPowerSpectrum ps;
    /* calculate the power spectrum */
    fastpm_powerspectrum_init_from_delta(&ps, fastpm->pm, delta_k, delta_k);

    double Plin = fastpm_powerspectrum_large_scale(&ps, fastpm->K_LINEAR);

    double Sigma8 = fastpm_powerspectrum_sigma(&ps, 8);

    Plin /= pow(fastpm_growth_factor(fastpm, a_x), 2.0);
    Sigma8 /= pow(fastpm_growth_factor(fastpm, a_x), 2.0);

    fastpm_info("D^2(%g, 1.0) P(k<%g) = %g Sigma8 = %g\n", a_x, fastpm->K_LINEAR * 6.28 / fastpm->boxsize, Plin, Sigma8);

    LEAVE(compute);

    MPI_Barrier(fastpm->comm);

    ENTER(io);
    if(CONF(prr, write_powerspectrum)) {
        if(fastpm->ThisTask == 0) {
            ensure_dir(CONF(prr, write_powerspectrum));
            char buf[1024];
            sprintf(buf, "%s_%0.04f.txt", CONF(prr, write_powerspectrum), a_x);
            fastpm_powerspectrum_write(&ps, buf, pow(fastpm->nc, 3.0));
        }
    }
    LEAVE(io);

    fastpm_powerspectrum_destroy(&ps);

    return 0;
}

int
read_powerspectrum(FastPMPowerSpectrum * ps, const char filename[], const double sigma8, MPI_Comm comm)
{
    fastpm_info("Powerspecectrum file: %s\n", filename);

    int myrank;
    MPI_Comm_rank(comm, &myrank);
    char * content;
    if(myrank == 0) {
        content = fastpm_file_get_content(filename);
        int size = strlen(content);
        MPI_Bcast(&size, 1, MPI_INT, 0, comm);
        MPI_Bcast(content, size + 1, MPI_BYTE, 0, comm);
    } else {
        int size = 0;
        MPI_Bcast(&size, 1, MPI_INT, 0, comm);
        content = malloc(size + 1);
        MPI_Bcast(content, size + 1, MPI_BYTE, 0, comm);
    }
    if (0 != fastpm_powerspectrum_init_from_string(ps, content)) {
        fastpm_raise(-1, "Failed to parse the powerspectrum\n");
    }
    free(content);

    fastpm_info("Found %d pairs of values in input spectrum table\n", ps->size);

    double sigma8_input= fastpm_powerspectrum_sigma(ps, 8);
    fastpm_info("Input power spectrum sigma8 %f\n", sigma8_input);

    if(sigma8 > 0) {
        fastpm_info("Expected power spectrum sigma8 %g; correction applied. \n", sigma8);
        fastpm_powerspectrum_scale(ps, pow(sigma8 / sigma8_input, 2));
    }
    return 0;
}


static void 
parse_args(int * argc, char *** argv, Parameters * prr) 
{
    char opt;
    extern int optind;
    extern char * optarg;
    prr->UseFFTW = 0;
    ParamFileName = NULL;
    prr->NprocY = 0;    
    prr->Nwriters = 0;
    while ((opt = getopt(*argc, *argv, "h?y:fW:")) != -1) {
        switch(opt) {
            case 'y':
                prr->NprocY = atoi(optarg);
            break;
            case 'f':
                prr->UseFFTW = 1;
            break;
            case 'W':
                prr->Nwriters = atoi(optarg);
            break;
            case 'h':
            case '?':
            default:
                goto usage;
            break;
        }
    }
    if(optind >= *argc) {
        goto usage;
    }

    ParamFileName = (*argv)[optind];
    *argv += optind;
    *argc -= optind; 
    return;

usage:
    printf("Usage: fastpm [-W Nwriters] [-f] [-y NprocY] paramfile\n"
    "-f Use FFTW \n"
    "-y Set the number of processes in the 2D mesh\n"
    "-n Throttle IO (bigfile only) \n"
);
    MPI_Finalize();
    exit(1);
}

static void 
ensure_dir(const char * path) 
{
    int i = strlen(path);
    char * dup = alloca(strlen(path) + 1);
    strcpy(dup, path);
    char * p;
    for(p = i + dup; p >= dup && *p != '/'; p --) {
        continue;
    }
    /* plain file name in current directory */
    if(p < dup) return;
    
    /* p == '/', so set it to NULL, dup is the dirname */
    *p = 0;
    _mkdir(dup);
}

static void 
_mkdir(const char *dir) 
{
    char * tmp = alloca(strlen(dir) + 1);
    strcpy(tmp, dir);
    char *p = NULL;
    size_t len;

    len = strlen(tmp);
    if(tmp[len - 1] == '/')
            tmp[len - 1] = 0;
    for(p = tmp + 1; *p; p++)
            if(*p == '/') {
                    *p = 0;
                    mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO);
                    *p = '/';
            }
    mkdir(tmp, S_IRWXU | S_IRWXG | S_IRWXO);
}

int read_parameters(char * filename, Parameters * param, int argc, char ** argv, MPI_Comm comm)
{
    int ThisTask;
    MPI_Comm_rank(comm, &ThisTask);

    /* read the configuration file */
    char * confstr;
    int confstr_len;

    /* run the parameter file on root rank.
     * other ranks use the serialized string to avoid duplicated
     * error reports */
    if(ThisTask == 0) {
        confstr = run_paramfile(filename, 0, argc, argv);
        confstr_len = strlen(confstr) + 1;
        MPI_Bcast(&confstr_len, 1, MPI_INT, 0, comm);
        MPI_Bcast(confstr, confstr_len, MPI_BYTE, 0, comm);
    } else {
        MPI_Bcast(&confstr_len, 1, MPI_INT, 0, comm);
        confstr = malloc(confstr_len);
        MPI_Bcast(confstr, confstr_len, MPI_BYTE, 0, comm);
    }

    fastpm_info("Configuration %s\n", confstr);

    param->config = lua_config_new(confstr);
    param->string = confstr;
    if(lua_config_error(param->config)) {
        fastpm_raise(-1, "error: %s\n", lua_config_error(param->config));
    }
    return 0;
}

