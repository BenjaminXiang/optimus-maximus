//
//  algo.cpp
//  SimDex
//
//

#include "algo.hpp"
#include "arith.hpp"
#include "parser.hpp"
#include "utils.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <queue>

#include <ipps.h>
#ifdef MKL_ILP64
#include <mkl.h>
#else
#include <cblas.h>
#endif

std::vector<float> linspace(const float start, const float end, const int num) {
  float delta = (end - start) / num;

  std::vector<float> linspaced;
  // start from 1; omit start (we only care about upper bounds)
  for (int i = 0; i < num; ++i) {
    linspaced.push_back(start + delta * (i + 1));
  }
  return linspaced;
}

/**
 * Find index of smallest theta_b that is greater than theta_uc,
 * so we can find the right list of sorted upper bounds for a given
 * user
 **/
int find_theta_bin_index(const float theta_uc,
                         const std::vector<float> theta_bins,
                         const int num_bins) {
  for (int i = 0; i < num_bins; ++i) {
    if (theta_uc <= theta_bins[i]) {
      return i;
    }
  }
  return num_bins - 1;
}

#ifdef DEBUG
void check_against_naive(const double *user_weight, const double *item_weights,
                         const int num_items, const int num_latent_factors,
                         const int *computed_top_K,
                         const double *computed_scores, const int K) {
  const int m = num_items;
  const int k = num_latent_factors;

  const float alpha = 1.0;
  const float beta = 0.0;
  const int stride = 1;

  double scores[num_items];
  cblas_dgemv(CblasRowMajor, CblasNoTrans, m, k, alpha, item_weights, k,
              user_weight, stride, beta, scores, stride);

  // Sort item ids by their associated scores in descending order
  std::vector<int> v(num_items);
  std::iota(v.begin(), v.end(), 0);
  std::sort(v.begin(), v.end(),
            [&scores](int i1, int i2) { return scores[i1] > scores[i2]; });
  // Sort scores in descending order, too
  std::sort(scores, scores + num_items, std::greater<double>());
  for (int i = 0; i < K; ++i) {
    if (v[i] != computed_top_K[i]) {
      std::cout << "v[i] = " << v[i]
                << " does not equal computed_top_K[i] = " << computed_top_K[i]
                << ", i = " << i << std::endl;
      exit(1);
    }
  }
}
#endif

void computeTopKForCluster(
    int *top_K_items, const int cluster_id, const double *centroid,
    const std::vector<int> &user_ids_in_cluster, const double *user_weights,
    const double *item_weights, const float *item_norms, const float *theta_ics,
    const float &centroid_norm, const int num_items,
    const int num_latent_factors, const int K, const int item_batch_size,
    int num_users_to_compute, std::ofstream &user_stats_file) {
  /*, float *upper_bounds,
    int *sorted_upper_bounds_indices, float *sorted_upper_bounds,
    double *sorted_item_weights, std::ofstream &user_stats_file) {*/

    const int num_bins = 1;

    float* upper_bounds = (float*)_malloc(num_bins * num_items * sizeof(float));
    int* sorted_upper_bounds_indices = (int*)_malloc(num_items * sizeof(int));
    float* sorted_upper_bounds = (float*)_malloc(num_items * sizeof(float));
    double* sorted_item_weights = (double*)_malloc(
        sizeof(double) * num_bins * num_items * num_latent_factors);

#ifdef STATS
  bench_timer_t upper_bounds_start = time_start();
#endif

  if (num_users_to_compute * item_batch_size < 0) {
    std::cout << "ERROR! num_users_to_compute*item_batch_size overflowed!"
              << std::endl;
    exit(1);
  }

  double *users_dot_items = (double *)_malloc(
      sizeof(double) * num_users_to_compute * item_batch_size);
  float *user_norm_times_upper_bound =
      (float *)_malloc(sizeof(float) * item_batch_size);
  const int mod =
      item_batch_size - 1;  // assumes item_batch_size is a power of 2, so
  // mod is all 1's in binary, therefore
  // ind % item_batch_size == ind & mod

  // compute user_norms and theta_ucs for every user assigned to this cluster
  float *user_norms = compute_norms_vector(user_weights, num_users_to_compute,
                                           num_latent_factors);
  float *theta_ucs = compute_theta_ucs_for_centroid(
      user_weights, user_norms, centroid, num_users_to_compute,
      num_latent_factors, centroid_norm);
  // NOTE: both are now already in the right order, i.e., you can access
  // them sequentially. This is because we reordered the user weights to be
  // in cluster order in main (see build_cluster_index)

  const float theta_max =
      theta_ucs[cblas_isamax(num_users_to_compute, theta_ucs, 1)];
  // theta_bins is correct

  int i, j, l;

  std::fill(upper_bounds, &upper_bounds[num_items], theta_max);
#ifdef MKL_ILP64
  vsSub(num_items, theta_ics, upper_bounds, upper_bounds);
// upper_bounds[i] = theta_ic - theta_b
#else
#pragma simd
  for (int i = 0; i < num_items; ++i) {
    upper_bounds[i] = theta_ics[i] - upper_bounds[i];
  }
#endif

  ippsThreshold_32f_I((Ipp32f *)upper_bounds, num_items, (Ipp32f)0.0f,
                      ippCmpLess);
// all values less than 0 in upper_bounds[i] now set to 0
// Same as:
// for (i = 0; i < num_bins; ++i) {
//   for (j = 0; j < num_items; ++j) {
//     if (upper_bounds[i][j] < 0) {
//       upper_bounds[i][j] = 0.F;
//     }
//   }
// }
#ifdef MKL_ILP64  //
  vsCos(num_items, upper_bounds, upper_bounds);
  // upper_bounds[i] = cos(theta_ic - theta_b)
  vsMul(num_items, item_norms, upper_bounds, upper_bounds);
// upper_bounds[i] = ||i|| * cos(theta_ic - theta_b)
#else
#pragma simd
  for (int i = 0; i < num_items; ++i) {
    upper_bounds[i] = item_norms[i] * cos(upper_bounds[i]);
  }
#endif

#ifdef STATS
  const double upper_bounds_time = time_stop(upper_bounds_start);

  bench_timer_t sort_start = time_start();
#endif

  int *pBufSize = (int *)_malloc(sizeof(int));
  ippsSortRadixIndexGetBufferSize(num_items, ipp32f, pBufSize);
  Ipp8u *pBuffer = (Ipp8u *)_malloc(*pBufSize * sizeof(Ipp8u));
  const int src_stride_bytes = 4;
  ippsSortRadixIndexDescend_32f((Ipp32f *)upper_bounds, src_stride_bytes,
                                (Ipp32s *)sorted_upper_bounds_indices,
                                num_items, pBuffer);

#ifdef STATS
  const double sort_time = time_stop(sort_start);

  bench_timer_t batch_start = time_start();
#endif

  int item_id;

  /**
   * prepare initial batches upper_bounds, sorted_upper_bounds,
   * sorted_upper_bounds_indices are all [num_items] long, but we populate them
   * item_batch_size at a time, because user_norm_times_upper_bound is only
   * [item_batch_size] long
   */
  for (j = 0; j < item_batch_size; j++) {
    item_id = sorted_upper_bounds_indices[j];
    sorted_upper_bounds[j] = upper_bounds[item_id];
    // copy item_weights into sorted_item_weights based on order of
    // sorted_upper_bounds_indices
    cblas_dcopy(num_latent_factors, &item_weights[item_id * num_latent_factors],
                1, &sorted_item_weights[j * num_latent_factors], 1);
  }
  int batch_counter = item_batch_size;

#ifdef STATS
  const double batch_time = time_stop(batch_start);
#endif

// ----------Computer Per User TopK Below------------------
#ifdef DEBUG
  num_users_to_compute = num_users_to_compute < 40 ? num_users_to_compute : 40;
#endif

  const int m = num_users_to_compute;
  int n = std::min(item_batch_size,
                   num_items);  // not const, because it may be adjusted later
  const int k = num_latent_factors;

  const float alpha = 1.0f;
  const float beta = 0.0f;
  const int stride = 1;

#ifndef NAIVE
  cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, m, n, k, alpha,
              user_weights, k, sorted_item_weights, k, beta, users_dot_items,
              n);
#else
  // user_weights: m x k
  // sorted_item_weights: n x k
  // users_dot_items : m x n
  for (int a = 0; a < m; ++a) {
    for (int b = 0; b < n; ++b) {
      users_dot_items[a * n + b] = 0.0;
      for (int c = 0; c < k; ++c) {
        users_dot_items[a * n + b] +=
            user_weights[a * k + c] * sorted_item_weights[b * k + c];
      }
    }
  }
#endif

  for (i = 0; i < num_users_to_compute; i++) {
#ifdef STATS
    bench_timer_t user_top_K_start = time_start();
#endif

    std::priority_queue<std::pair<double, int>,
                        std::vector<std::pair<double, int> >,
                        std::greater<std::pair<double, int> > >
        q;

    double score = 0.0;

#pragma simd
    for (j = 0; j < item_batch_size; j++) {
      user_norm_times_upper_bound[j] = user_norms[i] * sorted_upper_bounds[j];
    }

    for (j = 0; j < K; j++) {
      item_id = sorted_upper_bounds_indices[j];
      score = users_dot_items[i * item_batch_size + j];
      q.push(std::make_pair(score, item_id));
    }

#ifdef STATS
    int num_items_visited = K;
#endif

    for (j = K; j < num_items; j++) {
      if ((j & mod) == 0) {  // same as j % item_batch_size == 0
        if (j == batch_counter) {
          // all previous batches exhausted, need to add an additional batch
          // if we're at the very end, we may not need a full batch
          n = std::min(item_batch_size,
                       num_items - batch_counter);  // change for upcoming sgemv
          // operation
          for (int l = 0; l < n; l++) {
            item_id = sorted_upper_bounds_indices[batch_counter + l];
            sorted_upper_bounds[batch_counter + l] = upper_bounds[item_id];
            cblas_dcopy(
                num_latent_factors, &item_weights[item_id * num_latent_factors],
                1,
                &sorted_item_weights[(batch_counter + l) * num_latent_factors],
                1);
          }
          batch_counter += n;
        }

#ifndef NAIVE
        // if we've gone through an entire batch, load users_dot_items and
        // user_norm_times_upper_bound with another batch
        cblas_dgemv(CblasRowMajor, CblasNoTrans, n, k, alpha,
                    &sorted_item_weights[j * num_latent_factors], k,
                    &user_weights[i * num_latent_factors], stride, beta,
                    &users_dot_items[i * item_batch_size], stride);
#else
        // item_weights_ptr: n * k
        // user_weights_ptr: 1 x k
        // users_dot_items : m x n
        double *item_weights_ptr = &sorted_item_weights[j * num_latent_factors];
        const double *user_weights_ptr = &user_weights[i * num_latent_factors];
        double *users_dot_items_ptr = &users_dot_items[i * item_batch_size];
        for (int a = 0; a < n; ++a) {
          users_dot_items_ptr[a] = 0.0;
          for (int b = 0; b < k; ++b) {
            users_dot_items_ptr[a] +=
                user_weights_ptr[b] * item_weights_ptr[a * k + b];
          }
        }
#endif

#pragma simd
        for (l = 0; l < item_batch_size; l++) {
          user_norm_times_upper_bound[l] =
              user_norms[i] * sorted_upper_bounds[j + l];
        }
      }

      if (q.top().first > user_norm_times_upper_bound[j & mod]) {
        break;
      }
      item_id = sorted_upper_bounds_indices[j];
      score = users_dot_items[i * item_batch_size + (j & mod)];
#ifdef STATS
      num_items_visited++;
#endif
      if (q.top().first < score) {
        q.pop();
        q.push(std::make_pair(score, item_id));
      }
    }
#ifdef DEBUG
    double top_K_scores[K];
#endif
    for (j = 0; j < K; j++) {
      std::pair<double, int> p = q.top();
#ifdef DEBUG
      top_K_scores[K - 1 - j] = p.first;
#endif
      // don't need to store score
      top_K_items[i * K + K - 1 - j] = p.second;  // store item ID
      q.pop();
    }
#ifdef STATS
    const double user_top_K_time = time_stop(user_top_K_start);
#endif

#ifdef DEBUG
    std::cout << "User ID " << user_ids_in_cluster[i] << std::endl;
    check_against_naive(&user_weights[i * num_latent_factors], item_weights,
                        num_items, num_latent_factors, &top_K_items[i * K],
                        top_K_scores, K);
#endif
#ifdef STATS
    const double total_user_time_ms =
        1000 * (user_top_K_time + batch_time + sort_time + upper_bounds_time);
    user_stats_file << cluster_id << "," << theta_ucs[i] << "," << theta_max
                    << "," << num_items_visited << "," << total_user_time_ms
                    << std::endl;
#endif
  }

  // ----------Free Allocated Memory Below-------
  _free(pBufSize);
  _free(pBuffer);
  _free(user_norms);
  _free(theta_ucs);
  _free(users_dot_items);
  _free(user_norm_times_upper_bound);
#ifdef MKL_ILP64
  MKL_Free_Buffers();
#endif
}
