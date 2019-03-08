#include "CPULSSolver.h"

/**
 * @brief Constructor initializes array pointers for fluxes and sources.
 * @details The constructor retrieves the number of energy groups and FSRs
 *          and azimuthal angles from the Geometry and TrackGenerator if
 *          passed in as parameters by the user. The constructor initalizes
 *          the number of OpenMP threads to a default of 1.
 * @param track_generator an optional pointer to the TrackGenerator
 */
CPULSSolver::CPULSSolver(TrackGenerator* track_generator)
    : CPUSolver(track_generator) {

  _FSR_source_constants = NULL;
  _FSR_lin_exp_matrix = NULL;
  _scalar_flux_xyz = NULL;
  _reduced_sources_xyz = NULL;
  _stabilizing_flux_xyz = NULL;
  _stabilize_moments = true;
  _source_type = "Linear";
}


/**
 * @brief Destructor deletes array for linear fluxes, sources and constants.
 *        CPUSolver parent class destructor handles deletion of arrays for flat
 *        fluxes and sources.
 */
CPULSSolver::~CPULSSolver() {

  if (_scalar_flux_xyz != NULL)
    delete [] _scalar_flux_xyz;

  if (_reduced_sources_xyz != NULL)
    delete [] _reduced_sources_xyz;

  if (_stabilizing_flux_xyz != NULL)
    delete [] _stabilizing_flux_xyz;

  if (_FSR_lin_exp_matrix != NULL)
    delete [] _FSR_lin_exp_matrix;

  if (_FSR_source_constants != NULL)
    delete [] _FSR_source_constants;
}

/**
 * @brief Fills an array with the scalar fluxes.
 * @details This class method is a helper routine called by the OpenMOC
 *          Python "openmoc.krylov" module for Krylov subspace methods.
 *          Although this method appears to require two arguments, in
 *          reality it only requires one due to SWIG and would be called
 *          from within Python as follows:
 *
 * @code
 *          num_fluxes = num_groups * num_FSRs
 *          fluxes = solver.getFluxes(num_fluxes)
 * @endcode
 *
 * @param fluxes an array of FSR scalar fluxes in each energy group and moment
 * @param num_fluxes the total number of FSR flux values
 */

void CPULSolver::getFluxes(FP_PRECISION* outfluxes, int num_fluxes){
  if(num_fluxes != _num_groups * _num_FSRS)
    log_printf(ERROR, "Unable to get FSR scalar fluxes since there are "
      "%d groups and %d FSRs which does not match the requested "
      "d flux values", _num_groups, _num_FSRs, num_fluxes);

  else if (_scalar_flux_xyz == NULL)
    log_printf(ERROR, "Unable to get FSR scalar fluxes since they"
      "have not yet been allocated");
    /*If the user called setFluxes(...) they already have the flux */
  if(_user_fluxes && _scalar_flux_xyz == out_fluxes)
    return;
  /*Otherwise, copy the fluxes into the input array*/
  else{
#pragma omp parallel for schedule(guided)
    for(int r=0; r < _num_FSRs; r++){
      for(int e=0; e< _num_groups; e++)
        for(int p = 0; p <3; x++)
          out_fluxes[r*_num_groups+e] = _scalar_flux_xyz(r,e,p);
    }
  }

/**

/**
 * @brief Allocates memory for boundary and scalar fluxes.
 * @details Deletes memory for old flux arrays if they were allocated
 *          for a previous simulation. Calls the CPUSolver parent class
 *          initializeFluxArrays for track anguar fluxes and flat scalar fluxes.
 */
void CPULSSolver::initializeFluxArrays() {
  CPUSolver::initializeFluxArrays();

  /* Delete old flux moment arrays if they exist */
  if (_scalar_flux_xyz != NULL)
    delete [] _scalar_flux_xyz;

  try {
    /* Allocate an array for the FSR scalar flux */
    long size = _num_FSRs * _num_groups * 3;
    long max_size = size;
#ifdef MPIX
    if (_geometry->isDomainDecomposed())
      MPI_Allreduce(&size, &max_size, 1, MPI_LONG, MPI_MAX,
                    _geometry->getMPICart());
#endif
    double max_size_mb = (double) (max_size * sizeof(FP_PRECISION))
        / (double) (1e6);

    if (_stabilize_transport && _stabilize_moments)
      max_size_mb *= 2;

    log_printf(NORMAL, "Max linear flux storage per domain = %6.2f MB",
               max_size_mb);

    _scalar_flux_xyz = new FP_PRECISION[size]();

    if (_stabilize_transport && _stabilize_moments)
      _stabilizing_flux_xyz = new FP_PRECISION[size]();
  }
  catch (std::exception &e) {
    log_printf(ERROR, "Could not allocate memory for the scalar flux moments");
  }
}


/**
 * @brief Allocates memory for FSR source arrays.
 * @details Deletes memory for old source arrays if they were allocated for a
 *          previous simulation.
 */
void CPULSSolver::initializeSourceArrays() {
  CPUSolver::initializeSourceArrays();

  /* Delete old sources moment arrays if they exist */
  if (_reduced_sources_xyz != NULL)
    delete [] _reduced_sources_xyz;

  long size = _num_FSRs * _num_groups * 3;

  /* Allocate memory for all source arrays */
  try {
    long max_size = size;
#ifdef MPIX
    if (_geometry->isDomainDecomposed())
      MPI_Allreduce(&size, &max_size, 1, MPI_LONG, MPI_MAX,
                    _geometry->getMPICart());
#endif
    double max_size_mb = (double) (max_size * sizeof(FP_PRECISION))
        / (double) (1e6);
    log_printf(NORMAL, "Max linear source storage per domain = %6.2f MB",
               max_size_mb);

    /* Initialize source moments to zero */
    _reduced_sources_xyz = new FP_PRECISION[size]();
  }
  catch(std::exception &e) {
    log_printf(ERROR, "Could not allocate memory for FSR source moments");
  }
}


/**
 * @brief Initializes the FSR constant linear source component, volumes and
 *        Materials array.
 * @details This method calls parent class CPUSolver's initalizeFSRs to allocate
 *          and initialize an array of OpenMP mutual exclusion locks for each
 *          FSR for use in the transport sweep algorithm.
 */
void CPULSSolver::initializeFSRs() {

  CPUSolver::initializeFSRs();

  /* Initialize constant source components and source expansion matrices */
  initializeLinearSourceConstants();

  /* Generate linear source coefficients */
  log_printf(NORMAL, "Generating linear expansion coefficients");
  LinearExpansionGenerator lin_src_coeffs(this);
  lin_src_coeffs.execute();
  log_printf(NORMAL, "Linear expansion coefficient generation complete");
}


/**
 * @brief Set the scalar flux constants for each FSR and energy group to some
 *        value and the scalar flux moments to zero.
 * @param value the value to assign to each FSR scalar flux
 */
void CPULSSolver::flattenFSRFluxes(FP_PRECISION value) {
  CPUSolver::flattenFSRFluxes(value);

#pragma omp parallel for schedule(static)
  for (long r=0; r < _num_FSRs; r++) {
    for (int e=0; e < _num_groups; e++) {
      _scalar_flux_xyz(r,e,0) = 0.0;
      _scalar_flux_xyz(r,e,1) = 0.0;
      _scalar_flux_xyz(r,e,2) = 0.0;
    }
  }
}


/**
 * @brief Normalizes all FSR scalar fluxes and Track boundary angular
 *        fluxes to the total fission source (times \f$ \nu \f$).
 * @return norm_factor the normalization factor on the scalar fluxes and moments
 */
double CPULSSolver::normalizeFluxes() {

  /* Normalize scalar fluxes in each FSR */
  double norm_factor = CPUSolver::normalizeFluxes();

#pragma omp parallel for schedule(static)
  for (long r=0; r < _num_FSRs; r++) {
    for (int e=0; e < _num_groups; e++) {
      _scalar_flux_xyz(r,e,0) *= norm_factor;
      _scalar_flux_xyz(r,e,1) *= norm_factor;
      _scalar_flux_xyz(r,e,2) *= norm_factor;
    }
  }

  return norm_factor;
}


/**
 * @brief Computes the total source (fission, scattering, fixed) in each FSR.
 * @details This method computes the total source in each FSR based on
 *          this iteration's current approximation to the scalar flux and its
 *          moments. Fixed source moments are currently not supported.
 */
void CPULSSolver::computeFSRSources(int iteration) {
  CPUSolver::computeFSRSources(iteration);

  int num_coeffs = 3;
  if (_SOLVE_3D)
    num_coeffs = 6;

#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    Material* material;
    FP_PRECISION* sigma_t;
    FP_PRECISION* nu_sigma_f;
    FP_PRECISION* chi;
    FP_PRECISION* sigma_s;
    FP_PRECISION src_x, src_y, src_z;

    /* Compute the total source for each FSR */
#pragma omp for schedule(guided)
    for (long r=0; r < _num_FSRs; r++) {

      material = _FSR_materials[r];
      nu_sigma_f = material->getNuSigmaF();
      chi = material->getChi();
      sigma_t = material->getSigmaT();
      sigma_s = material->getSigmaS();

      /* Initialize the fission sources to zero */
      double fission_source_x = 0.0;
      double fission_source_y = 0.0;
      double fission_source_z = 0.0;

      /* Compute fission sources */
      if (material->isFissionable()) {
        FP_PRECISION* fission_sources_x = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++)
          fission_sources_x[g_prime] =
              nu_sigma_f[g_prime] * _scalar_flux_xyz(r,g_prime,0);
        fission_source_x =
            pairwise_sum<FP_PRECISION>(fission_sources_x, _num_groups);

        FP_PRECISION* fission_sources_y = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++)
          fission_sources_y[g_prime] =
              nu_sigma_f[g_prime] * _scalar_flux_xyz(r,g_prime,1);
        fission_source_y =
            pairwise_sum<FP_PRECISION>(fission_sources_y, _num_groups);

        FP_PRECISION* fission_sources_z = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++)
          fission_sources_z[g_prime] =
              nu_sigma_f[g_prime] * _scalar_flux_xyz(r,g_prime,2);
        fission_source_z =
            pairwise_sum<FP_PRECISION>(fission_sources_z, _num_groups);

        fission_source_x /= _k_eff;
        fission_source_y /= _k_eff;
        fission_source_z /= _k_eff;
      }

      /* Compute scatter + fission source for group g */
      for (int g=0; g < _num_groups; g++) {

        int first_scattering_index = g * _num_groups;

        /* Compute scatter sources */
        FP_PRECISION* scatter_sources_x = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++) {
          int idx = first_scattering_index + g_prime;
          scatter_sources_x[g_prime] = sigma_s[idx] *
               _scalar_flux_xyz(r,g_prime,0);
        }
        double scatter_source_x =
            pairwise_sum<FP_PRECISION>(scatter_sources_x, _num_groups);

        FP_PRECISION* scatter_sources_y = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++) {
          int idx = first_scattering_index + g_prime;
          scatter_sources_y[g_prime] = sigma_s[idx] *
               _scalar_flux_xyz(r,g_prime,1);
        }
        double scatter_source_y =
            pairwise_sum<FP_PRECISION>(scatter_sources_y, _num_groups);

        FP_PRECISION* scatter_sources_z = _groupwise_scratch.at(tid);
        for (int g_prime=0; g_prime < _num_groups; g_prime++) {
          int idx = first_scattering_index + g_prime;
          scatter_sources_z[g_prime] = sigma_s[idx] *
               _scalar_flux_xyz(r,g_prime,2);
        }
        double scatter_source_z =
            pairwise_sum<FP_PRECISION>(scatter_sources_z, _num_groups);

        /* Compute total (scatter + fission) source */
        src_x = scatter_source_x + chi[g] * fission_source_x;
        src_y = scatter_source_y + chi[g] * fission_source_y;
        src_z = scatter_source_z + chi[g] * fission_source_z;

        /* Compute total (scatter+fission) reduced source moments */
        if (_SOLVE_3D) {
          if (_reduced_sources(r,g) > 1e-15 || iteration > 29) {
            _reduced_sources_xyz(r,g,0) = ONE_OVER_FOUR_PI / 2 *
                 (_FSR_lin_exp_matrix[r*num_coeffs  ] * src_x +
                  _FSR_lin_exp_matrix[r*num_coeffs+2] * src_y +
                  _FSR_lin_exp_matrix[r*num_coeffs+3] * src_z);
            _reduced_sources_xyz(r,g,1) = ONE_OVER_FOUR_PI / 2 *
                 (_FSR_lin_exp_matrix[r*num_coeffs+2] * src_x +
                  _FSR_lin_exp_matrix[r*num_coeffs+1] * src_y +
                  _FSR_lin_exp_matrix[r*num_coeffs+4] * src_z);
            _reduced_sources_xyz(r,g,2) = ONE_OVER_FOUR_PI / 2 *
                 (_FSR_lin_exp_matrix[r*num_coeffs+3] * src_x +
                  _FSR_lin_exp_matrix[r*num_coeffs+4] * src_y +
                  _FSR_lin_exp_matrix[r*num_coeffs+5] * src_z);
          }
          else {
            _reduced_sources_xyz(r,g,0) = 1e-20;
            _reduced_sources_xyz(r,g,1) = 1e-20;
            _reduced_sources_xyz(r,g,2) = 1e-20;
          }
        }
        else {
          if (_reduced_sources(r,g) > 1e-15 || iteration > 29) {
            _reduced_sources_xyz(r,g,0) = ONE_OVER_FOUR_PI / 2 *
                 (_FSR_lin_exp_matrix[r*num_coeffs  ] * src_x +
                  _FSR_lin_exp_matrix[r*num_coeffs+2] * src_y);
            _reduced_sources_xyz(r,g,1) = ONE_OVER_FOUR_PI / 2 *
                 (_FSR_lin_exp_matrix[r*num_coeffs+2] * src_x +
                  _FSR_lin_exp_matrix[r*num_coeffs+1] * src_y);
          }
          else {
            _reduced_sources_xyz(r,g,0) = 1e-20;
            _reduced_sources_xyz(r,g,1) = 1e-20;
          }
        }
      }
    }
  }
}


/**
 * @brief Computes the contribution to the LSR scalar flux from a Track segment.
 * @details This method integrates the angular flux for a Track segment across
 *          energy groups (and polar angles in 2D), and tallies it into the
 *          scalar flux buffers, and updates the Track's angular flux.
 * @param curr_segment a pointer to the Track segment of interest
 * @param azim_index azimuthal angle index for this 3D Track
 * @param polar_index polar angle index for this 3D Track
 * @param fsr_flux buffer to store segment contribution to region scalar flux
 * @param fsr_flux_x buffer to store contribution to the x scalar flux moment
 * @param fsr_flux_y buffer to store contribution to the y scalar flux moment
 * @param fsr_flux_z buffer to store contribution to the z scalar flux moment
 * @param track_flux a pointer to the Track's angular flux
 * @param direction the segment's direction
 */
void CPULSSolver::tallyLSScalarFlux(segment* curr_segment, int azim_index,
                                    int polar_index,
                                    FP_PRECISION* __restrict__ fsr_flux,
                                    FP_PRECISION* __restrict__ fsr_flux_x,
                                    FP_PRECISION* __restrict__ fsr_flux_y,
                                    FP_PRECISION* __restrict__ fsr_flux_z,
                                    float* track_flux,
                                    FP_PRECISION direction[3]) {

  long fsr_id = curr_segment->_region_id;
  FP_PRECISION length = curr_segment->_length;
  FP_PRECISION* sigma_t = curr_segment->_material->getSigmaT();
  FP_PRECISION* position = curr_segment->_starting_position;
  ExpEvaluator* exp_evaluator = _exp_evaluators[azim_index][polar_index];

  if (_SOLVE_3D) {

    /* Compute the segment midpoint (with factor 2 for LS) */
    FP_PRECISION center_x2[3];
    for (int i=0; i<3; i++)
      center_x2[i] = 2 * position[i] + length * direction[i];

    FP_PRECISION wgt = _quad->getWeightInline(azim_index, polar_index);
    FP_PRECISION length_2D = exp_evaluator->convertDistance3Dto2D(length);

    // Compute the exponential terms
    FP_PRECISION exp_F1[_num_groups] __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION exp_F2[_num_groups] __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION exp_H[_num_groups] __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION tau[_num_groups] __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(sigma_t, tau, exp_F1, exp_F2, exp_H)
    for (int e=0; e < _num_groups; e++) {
      tau[e] = sigma_t[e] * length_2D;
      exp_evaluator->retrieveExponentialComponents(tau[e], 0, &exp_F1[e],
                                                   &exp_F2[e], &exp_H[e]);
    }

    // Compute the sources
    FP_PRECISION src_flat[_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION src_linear[_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(src_flat, src_linear)
    for (int e=0; e < _num_groups; e++) {
      src_flat[e] = _reduced_sources(fsr_id, e);
      for (int i=0; i<3; i++)
        src_flat[e] += _reduced_sources_xyz(fsr_id, e, i) * center_x2[i];
      src_linear[e] = _reduced_sources_xyz(fsr_id, e, 0) * direction[0];
      src_linear[e] += _reduced_sources_xyz(fsr_id, e, 1) * direction[1];
      src_linear[e] += _reduced_sources_xyz(fsr_id, e, 2) * direction[2];
    }

    // Compute the flux attenuation and tally contribution
#pragma omp simd aligned(tau, src_flat, src_linear, exp_F1, exp_F2, exp_H, \
     fsr_flux, fsr_flux_x, fsr_flux_y, fsr_flux_z)
    for (int e=0; e < _num_groups; e++) {

      // Compute the change in flux across the segment
      exp_H[e] *= length * track_flux[e] * tau[e] * wgt;
      FP_PRECISION delta_psi = (tau[e] * track_flux[e] - length_2D * src_flat[e])
           * exp_F1[e] - src_linear[e] * length_2D * length_2D * exp_F2[e];
      track_flux[e] -= delta_psi;
      delta_psi *= wgt;

      // Increment the fsr scalar flux and scalar flux moments
      fsr_flux[e] += delta_psi;
      fsr_flux_x[e] += exp_H[e] * direction[0] + delta_psi * position[0];
      fsr_flux_y[e] += exp_H[e] * direction[1] + delta_psi * position[1];
      fsr_flux_z[e] += exp_H[e] * direction[2] + delta_psi * position[2];
    }
  }
  else {

    int num_polar_2 = _num_polar / 2;

    /* Compute the segment midpoint (with factor 2 for LS) */
    FP_PRECISION center[2];
    for (int i=0; i<2; i++)
      center[i] = 2 * position[i] + length * direction[i];

    /* Compute tau in advance to simplify attenation loop */
    FP_PRECISION tau[_num_groups * num_polar_2]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(tau)
    for (int pe=0; pe < num_polar_2 * _num_groups; pe++)
      tau[pe] = sigma_t[pe % _num_groups] * length;

    /* Compute exponentials */
    FP_PRECISION exp_F1[num_polar_2*_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION exp_F2[num_polar_2*_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));
    FP_PRECISION exp_H[num_polar_2*_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(tau, exp_F1, exp_F2, exp_H)
    for (int pe=0; pe < num_polar_2 * _num_groups; pe++)
      exp_evaluator->retrieveExponentialComponents(tau[pe], int(pe/_num_groups),
                                                   &exp_F1[pe], &exp_F2[pe],
                                                   &exp_H[pe]);

    /* Compute flat part of the source */
    FP_PRECISION src_flat[_num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(src_flat)
    for (int e=0; e < _num_groups; e++) {
      src_flat[e] = _reduced_sources(fsr_id, e);
      for (int i=0; i<2; i++)
        src_flat[e] += _reduced_sources_xyz(fsr_id, e, i) * center[i];
    }

    /* Compute linear part of the source */
    FP_PRECISION src_linear[num_polar_2 * _num_groups]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(src_linear)
    for (int pe=0; pe < num_polar_2 * _num_groups; pe++) {
      FP_PRECISION sin_the = _quad->getSinThetaInline(azim_index,
                                                      int(pe/_num_groups));
      src_linear[pe] = direction[0] * sin_the *
            _reduced_sources_xyz(fsr_id, pe % _num_groups, 0);
      src_linear[pe] += direction[1] * sin_the *
            _reduced_sources_xyz(fsr_id, pe % _num_groups, 1);
    }

    /* Compute attenuation of track angular flux */
    FP_PRECISION delta_psi[_num_groups * num_polar_2]
                 __attribute__ ((aligned(VEC_ALIGNMENT)));

#pragma omp simd aligned(tau, src_flat, src_linear, delta_psi, exp_F1, exp_F2, exp_H)
    for (int pe=0; pe < num_polar_2 * _num_groups; pe++) {

      FP_PRECISION wgt = _quad->getWeightInline(azim_index, int(pe/_num_groups));
      exp_H[pe] *=  wgt * tau[pe] * length * track_flux[pe];

      // Compute the change in flux across the segment
      delta_psi[pe] = (tau[pe] * track_flux[pe] - length
            * src_flat[pe % _num_groups]) * exp_F1[pe] - length * length
            * src_linear[pe] * exp_F2[pe];
      track_flux[pe] -= delta_psi[pe];
      delta_psi[pe] *= wgt;
    }

    /* Increment the fsr scalar flux and scalar flux moments buffers */
    //TODO Change loop to accept 'pe' indexing, and keep vectorized
    for (int p=0; p < num_polar_2; p++) {

#pragma omp simd aligned(fsr_flux, fsr_flux_x, fsr_flux_y)
      for (int e=0; e < _num_groups; e++) {

        fsr_flux[e] += delta_psi[p*_num_groups + e];
        fsr_flux_x[e] += exp_H[p*_num_groups + e] * direction[0] +
                                    delta_psi[p*_num_groups + e] * position[0];
        fsr_flux_y[e] += exp_H[p*_num_groups + e] * direction[1] +
                                    delta_psi[p*_num_groups + e] * position[1];
      }
    }
  }

  /* Advance starting position for the next segment on this track */
  for (int i=0; i < 3; i++)
    position[i] += direction[i] * length;
}


/**
 * @brief Move from buffers to global arrays the contributions of one (or
 *        several for per-stack solving) segments.
 * @param fsr_id region index
 * @param fsr_flux buffer storing contribution to the region's scalar flux
 */
void CPULSSolver::accumulateLinearFluxContribution(long fsr_id,
                                       FP_PRECISION* __restrict__ fsr_flux) {

  int num_groups_aligned = (_num_groups / VEC_ALIGNMENT + 1) * VEC_ALIGNMENT;
  FP_PRECISION* fsr_flux_x = &fsr_flux[num_groups_aligned];
  FP_PRECISION* fsr_flux_y = &fsr_flux[2*num_groups_aligned];
  FP_PRECISION* fsr_flux_z = &fsr_flux[3*num_groups_aligned];

  // Atomically increment the FSR scalar flux from the temporary array
  omp_set_lock(&_FSR_locks[fsr_id]);

#pragma omp simd aligned(fsr_flux, fsr_flux_x, fsr_flux_y, fsr_flux_z)
  for (int e=0; e < _num_groups; e++) {

    // Add to global scalar flux vector
    _scalar_flux(fsr_id, e) += fsr_flux[e];
    _scalar_flux_xyz(fsr_id, e, 0) += fsr_flux_x[e];
    _scalar_flux_xyz(fsr_id, e, 1) += fsr_flux_y[e];
    _scalar_flux_xyz(fsr_id, e, 2) += fsr_flux_z[e];
  }

  omp_unset_lock(&_FSR_locks[fsr_id]);

  /* Reset buffers to 0 */
  memset(fsr_flux, 0, 4 * num_groups_aligned * sizeof(FP_PRECISION));
}


/**
 * @brief Add the source term contribution in the transport equation to
 *        the FSR scalar flux.
 */
void CPULSSolver::addSourceToScalarFlux() {

  int nc = 3;
  if (_SOLVE_3D)
    nc = 6;

#pragma omp parallel
  {
    FP_PRECISION volume;
    FP_PRECISION flux_const;
    FP_PRECISION* sigma_t;

    /* Add in source term and normalize flux to volume for each FSR */
    /* Loop over FSRs, energy groups */
#pragma omp for
    for (long r=0; r < _num_FSRs; r++) {
      volume = _FSR_volumes[r];
      sigma_t = _FSR_materials[r]->getSigmaT();

      for (int e=0; e < _num_groups; e++) {

        flux_const = FOUR_PI * 2;

        _scalar_flux(r,e) /= volume;
        _scalar_flux(r,e) += (FOUR_PI * _reduced_sources(r,e));
        _scalar_flux(r,e) /= sigma_t[e];

        _scalar_flux_xyz(r,e,0) /= volume;
        _scalar_flux_xyz(r,e,0) += flux_const * _reduced_sources_xyz(r,e,0)
            * _FSR_source_constants[r*_num_groups*nc + nc*e    ];
        _scalar_flux_xyz(r,e,0) += flux_const * _reduced_sources_xyz(r,e,1)
            * _FSR_source_constants[r*_num_groups*nc + nc*e + 2];

        _scalar_flux_xyz(r,e,1) /= volume;
        _scalar_flux_xyz(r,e,1) += flux_const * _reduced_sources_xyz(r,e,0)
            * _FSR_source_constants[r*_num_groups*nc + nc*e + 2];
        _scalar_flux_xyz(r,e,1) += flux_const * _reduced_sources_xyz(r,e,1)
            * _FSR_source_constants[r*_num_groups*nc + nc*e + 1];

        if (_SOLVE_3D) {
          _scalar_flux_xyz(r,e,0) += flux_const * _reduced_sources_xyz(r,e,2)
              * _FSR_source_constants[r*_num_groups*nc + nc*e + 3];
          _scalar_flux_xyz(r,e,1) += flux_const * _reduced_sources_xyz(r,e,2)
              * _FSR_source_constants[r*_num_groups*nc + nc*e + 4];

          _scalar_flux_xyz(r,e,2) /= volume;
          _scalar_flux_xyz(r,e,2) += flux_const * _reduced_sources_xyz(r,e,0)
              * _FSR_source_constants[r*_num_groups*nc + nc*e + 3];
          _scalar_flux_xyz(r,e,2) += flux_const * _reduced_sources_xyz(r,e,1)
              * _FSR_source_constants[r*_num_groups*nc + nc*e + 4];
          _scalar_flux_xyz(r,e,2) += flux_const * _reduced_sources_xyz(r,e,2)
              * _FSR_source_constants[r*_num_groups*nc + nc*e + 5];
        }

        _scalar_flux_xyz(r,e,0) /= sigma_t[e];
        _scalar_flux_xyz(r,e,1) /= sigma_t[e];
        if (_SOLVE_3D)
          _scalar_flux_xyz(r,e,2) /= sigma_t[e];
      }
    }
  }
}


/**
 * @brief Computes the stabilizing flux for transport stabilization
 */
void CPULSSolver::computeStabilizingFlux() {

  /* Compute flat stabilizing flux */
  CPUSolver::computeStabilizingFlux();

  /* Check whether moment stabilization is requested */
  if (!_stabilize_moments)
    return;

  if (_stabilization_type == DIAGONAL) {
    /* Loop over all flat source regions, compute stabilizing flux moments */
#pragma omp parallel for
    for (long r=0; r < _num_FSRs; r++) {

      /* Extract the scattering matrix */
      FP_PRECISION* scattering_matrix = _FSR_materials[r]->getSigmaS();

      /* Extract total cross-sections */
      FP_PRECISION* sigma_t = _FSR_materials[r]->getSigmaT();

      for (int e=0; e < _num_groups; e++) {

        /* Extract the in-scattering (diagonal) element */
        FP_PRECISION sigma_s = scattering_matrix[e*_num_groups+e];

        /* For negative cross-sections, add the absolute value of the
           in-scattering rate to the stabilizing flux */
        if (sigma_s < 0.0) {
          for (int i=0; i < 3; i++) {
            _stabilizing_flux_xyz(r, e, i) = -_scalar_flux_xyz(r,e,i) *
                _stabilization_factor * sigma_s / sigma_t[e];
          }
        }
      }
    }
  }
  else if (_stabilization_type == YAMAMOTO) {

    /* Treat each group */
#pragma omp parallel for
    for (int e=0; e < _num_groups; e++) {

      /* Look for largest absolute scattering ratio */
      FP_PRECISION max_ratio = 0.0;
      for (long r=0; r < _num_FSRs; r++) {

        /* Extract the scattering value */
        FP_PRECISION scat = _FSR_materials[r]->getSigmaSByGroup(e+1, e+1);

        /* Extract total cross-sections */
        FP_PRECISION total = _FSR_materials[r]->getSigmaTByGroup(e+1);

        /* Determine scattering ratio */
        FP_PRECISION ratio = std::abs(scat / total);
        if (ratio > max_ratio)
          ratio = max_ratio;
      }
      max_ratio *= _stabilization_factor;
      for (long r=0; r < _num_FSRs; r++) {
        for (int i=0; i < 3; i++) {
          _stabilizing_flux_xyz(r, e, i) = _scalar_flux_xyz(r,e,i) * max_ratio;
        }
      }
    }
  }
  else if (_stabilization_type == GLOBAL) {

    /* Get the multiplicative factor */
    FP_PRECISION mult_factor = 1.0 / _stabilization_factor - 1.0;

    /* Apply the global multiplicative factor */
#pragma omp parallel for
    for (long r=0; r < _num_FSRs; r++)
      for (int e=0; e < _num_groups; e++)
        for (int i=0; i <3; i++)
          _stabilizing_flux_xyz(r, e, i) = _scalar_flux_xyz(r, e, i)
             * mult_factor;
  }
}


/**
 * @brief Adjusts the scalar flux for transport stabilization.
 */
void CPULSSolver::stabilizeFlux() {

  /* Stabalize the flat scalar flux */
  CPUSolver::stabilizeFlux();

  /* Check whether moment stabilization is requested */
  if (!_stabilize_moments)
    return;

  if (_stabilization_type == DIAGONAL) {
    /* Loop over all flat source regions, apply stabilizing flux moments */
#pragma omp parallel for
    for (long r=0; r < _num_FSRs; r++) {

      /* Extract the scattering matrix */
      FP_PRECISION* scattering_matrix = _FSR_materials[r]->getSigmaS();

      /* Extract total cross-sections */
      FP_PRECISION* sigma_t = _FSR_materials[r]->getSigmaT();

      for (int e=0; e < _num_groups; e++) {

        /* Extract the in-scattering (diagonal) element */
        FP_PRECISION sigma_s = scattering_matrix[e*_num_groups+e];

        /* For negative cross-sections, add the stabilizing flux
           and divide by the diagonal matrix element used to form it so that
           no bias is introduced but the source iteration is stabilized */
        if (sigma_s < 0.0) {
          for (int i=0; i < 3; i++) {
            _scalar_flux_xyz(r, e, i) += _stabilizing_flux_xyz(r, e, i);
            _scalar_flux_xyz(r, e, i) /= (1.0 - _stabilization_factor * sigma_s /
                                         sigma_t[e]);
          }
        }
      }
    }
  }
  else if (_stabilization_type == YAMAMOTO) {

    /* Treat each group */
#pragma omp parallel for
    for (int e=0; e < _num_groups; e++) {

      /* Look for largest absolute scattering ratio */
      FP_PRECISION max_ratio = 0.0;
      for (long r=0; r < _num_FSRs; r++) {

        /* Extract the scattering value */
        FP_PRECISION scat = _FSR_materials[r]->getSigmaSByGroup(e+1, e+1);

        /* Extract total cross-sections */
        FP_PRECISION total = _FSR_materials[r]->getSigmaTByGroup(e+1);

        /* Determine scattering ratio */
        FP_PRECISION ratio = std::abs(scat / total);
        if (ratio > max_ratio)
          ratio = max_ratio;
      }
      max_ratio *= _stabilization_factor;
      for (long r=0; r < _num_FSRs; r++) {
        for (int i=0; i < 3; i++) {
          _scalar_flux_xyz(r, e, i) += _stabilizing_flux_xyz(r, e, i);
          _scalar_flux_xyz(r, e, i) /= (1 + max_ratio);
        }
      }
    }
  }
  else if (_stabilization_type == GLOBAL) {

    /* Apply the damping factor */
#pragma omp parallel for
    for (long r=0; r < _num_FSRs; r++) {
      for (int e=0; e < _num_groups; e++) {
        for (int i=0; i <3; i++) {
          _scalar_flux_xyz(r, e, i) += _stabilizing_flux_xyz(r, e, i);
          _scalar_flux_xyz(r, e, i) *= _stabilization_factor;
        }
      }
    }
  }
}


/**
 * @brief Checks to see if limited XS should be reset.
 * @details For the linear source, the linear expansion coefficients should also
 *          be reset, to use the non-limited cross sections.
 * @param iteration The MOC iteration number
 */
void CPULSSolver::checkLimitXS(int iteration) {

  Solver::checkLimitXS(iteration);

  if (iteration != _reset_iteration)
    return;

  /* Generate linear source coefficients */
  log_printf(NORMAL, "Generating linear expansion coefficients");
  LinearExpansionGenerator lin_src_coeffs(this);
  lin_src_coeffs.execute();
  log_printf(NORMAL, "Linear expansion coefficient generation complete");
}


/**
 * @brief Get the flux at a specific point in the geometry.
 * @param coords The coords of the point to get the flux at
 * @param group the energy group
 */
FP_PRECISION CPULSSolver::getFluxByCoords(LocalCoords* coords, int group) {

  double x, y, z, xc, yc, zc;

  coords->setUniverse(_geometry->getRootUniverse());
  Cell* cell = _geometry->findCellContainingCoords(coords);
  long fsr = _geometry->getFSRId(coords);
  Point* centroid = _geometry->getFSRCentroid(fsr);
  x = coords->getX();
  y = coords->getY();
  z = coords->getZ();
  xc = centroid->getX();
  yc = centroid->getY();
  zc = centroid->getZ();

  FP_PRECISION flux = _scalar_flux(fsr, group);
  double flux_x = 0.0;
  double flux_y = 0.0;
  double flux_z = 0.0;

  if (_SOLVE_3D) {
    flux_x = (x - xc) *
        (_FSR_lin_exp_matrix[fsr*6  ] * _scalar_flux_xyz(fsr, group, 0) +
         _FSR_lin_exp_matrix[fsr*6+2] * _scalar_flux_xyz(fsr, group, 1) +
         _FSR_lin_exp_matrix[fsr*6+3] * _scalar_flux_xyz(fsr, group, 2));
    flux_y = (y - yc) *
        (_FSR_lin_exp_matrix[fsr*6+2] * _scalar_flux_xyz(fsr, group, 0) +
         _FSR_lin_exp_matrix[fsr*6+1] * _scalar_flux_xyz(fsr, group, 1) +
         _FSR_lin_exp_matrix[fsr*6+4] * _scalar_flux_xyz(fsr, group, 2));
    flux_z = (z - zc) *
        (_FSR_lin_exp_matrix[fsr*6+3] * _scalar_flux_xyz(fsr, group, 0) +
         _FSR_lin_exp_matrix[fsr*6+4] * _scalar_flux_xyz(fsr, group, 1) +
         _FSR_lin_exp_matrix[fsr*6+5] * _scalar_flux_xyz(fsr, group, 2));
  }
  else {
    flux_x = (x - xc) *
        (_FSR_lin_exp_matrix[fsr*3  ] * _scalar_flux_xyz(fsr, group, 0) +
         _FSR_lin_exp_matrix[fsr*3+2] * _scalar_flux_xyz(fsr, group, 1));
    flux_y = (y - yc) *
        (_FSR_lin_exp_matrix[fsr*3+2] * _scalar_flux_xyz(fsr, group, 0) +
         _FSR_lin_exp_matrix[fsr*3+1] * _scalar_flux_xyz(fsr, group, 1));
  }

  flux += flux_x + flux_y + flux_z;
  return flux;
}


/**
 * @brief Initializes a Cmfd object for acceleration prior to source iteration.
 * @details For the linear source solver, a pointer to the flux moments is
 *          passed to the Cmfd object so that they can be updated as well in
 *          the prolongation phase.
 */
void CPULSSolver::initializeCmfd() {
  Solver::initializeCmfd();
  if (_cmfd != NULL)
    _cmfd->setFluxMoments(_scalar_flux_xyz);
}


/**
 * @brief Initializes new ExpEvaluator objects to compute exponentials.
 * @details Using the linear source incurs calculating extra exponential terms.
 */
void CPULSSolver::initializeExpEvaluators() {
  for (int a=0; a < _num_exp_evaluators_azim; a++)
    for (int p=0; p < _num_exp_evaluators_polar; p++)
      _exp_evaluators[a][p]->useLinearSource();
  Solver::initializeExpEvaluators();
}


/**
 * @brief Initialize linear source constant component and matrix coefficients.
 */
void CPULSSolver::initializeLinearSourceConstants() {

  if (_FSR_source_constants != NULL)
    delete[] _FSR_source_constants;
  if (_FSR_lin_exp_matrix != NULL)
    delete[] _FSR_lin_exp_matrix;

#pragma omp critical
  {
    /* Initialize linear source constant component */
    long size = 3 * _geometry->getNumEnergyGroups() * _geometry->getNumFSRs();
    if (_SOLVE_3D)
      size *= 2;

    long max_size = size;
#ifdef MPIX
    if (_geometry->isDomainDecomposed())
    MPI_Allreduce(&size, &max_size, 1, MPI_LONG, MPI_MAX,
                  _geometry->getMPICart());
#endif
    double max_size_mb = (double) (max_size * sizeof(FP_PRECISION))
         / (double) (1e6);
    log_printf(NORMAL, "Max linear constant storage per domain = %6.2f MB",
               max_size_mb);

    _FSR_source_constants = new FP_PRECISION[size]();

    /* Initialize linear source matrix coefficients */
    size = _geometry->getNumFSRs() * 3;
    if (_SOLVE_3D)
      size *= 2;
    _FSR_lin_exp_matrix = new double[size]();
  }
}


/**
 * @brief Returns a memory buffer to the linear source expansion coefficent
 *        matrix.
 * @return _FSR_lin_exp_matrix a pointer to the linear source coefficient matrix
 */
double* CPULSSolver::getLinearExpansionCoeffsBuffer() {

  return _FSR_lin_exp_matrix;
}


/**
 * @brief Returns a memory buffer to the constant part (constant between MOC
 *        iterations) of the linear source.
 * @return _FSR_source_constants a pointer to the linear source constant part
 */
FP_PRECISION* CPULSSolver::getSourceConstantsBuffer() {

  return _FSR_source_constants;
}
