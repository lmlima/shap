/**
 * Fast recursive computation of SHAP values in trees.
 * See https://arxiv.org/abs/1802.03888 for details.
 *
 * Scott Lundberg, 2018
 */

#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdio.h> 
using namespace std;

typedef double tfloat;

// data we keep about our decision path
// note that pweight is included for convenience and is not tied with the other attributes
// the pweight of the i'th path element is the permuation weight of paths with i-1 ones in them
struct PathElement {
  int feature_index;
  tfloat zero_fraction;
  tfloat one_fraction;
  tfloat pweight;
  PathElement() {}
  PathElement(int i, tfloat z, tfloat o, tfloat w) :
    feature_index(i), zero_fraction(z), one_fraction(o), pweight(w) {}
};


// extend our decision path with a fraction of one and zero extensions
inline void extend_path(PathElement *unique_path, unsigned unique_depth,
                        tfloat zero_fraction, tfloat one_fraction, int feature_index) {
  unique_path[unique_depth].feature_index = feature_index;
  unique_path[unique_depth].zero_fraction = zero_fraction;
  unique_path[unique_depth].one_fraction = one_fraction;
  unique_path[unique_depth].pweight = (unique_depth == 0 ? 1.0f : 0.0f);
  for (int i = unique_depth - 1; i >= 0; i--) {
    unique_path[i + 1].pweight += one_fraction * unique_path[i].pweight * (i + 1)
                                  / static_cast<tfloat>(unique_depth + 1);
    unique_path[i].pweight = zero_fraction * unique_path[i].pweight * (unique_depth - i)
                             / static_cast<tfloat>(unique_depth + 1);
  }
}

// undo a previous extension of the decision path
inline void unwind_path(PathElement *unique_path, unsigned unique_depth, unsigned path_index) {
  const tfloat one_fraction = unique_path[path_index].one_fraction;
  const tfloat zero_fraction = unique_path[path_index].zero_fraction;
  tfloat next_one_portion = unique_path[unique_depth].pweight;

  for (int i = unique_depth - 1; i >= 0; --i) {
    if (one_fraction != 0) {
      const tfloat tmp = unique_path[i].pweight;
      unique_path[i].pweight = next_one_portion * (unique_depth + 1)
                               / static_cast<tfloat>((i + 1) * one_fraction);
      next_one_portion = tmp - unique_path[i].pweight * zero_fraction * (unique_depth - i)
                               / static_cast<tfloat>(unique_depth + 1);
    } else {
      unique_path[i].pweight = (unique_path[i].pweight * (unique_depth + 1))
                               / static_cast<tfloat>(zero_fraction * (unique_depth - i));
    }
  }

  for (unsigned i = path_index; i < unique_depth; ++i) {
    unique_path[i].feature_index = unique_path[i+1].feature_index;
    unique_path[i].zero_fraction = unique_path[i+1].zero_fraction;
    unique_path[i].one_fraction = unique_path[i+1].one_fraction;
  }
}

// determine what the total permuation weight would be if
// we unwound a previous extension in the decision path
inline tfloat unwound_path_sum(const PathElement *unique_path, unsigned unique_depth,
                                  unsigned path_index) {
  const tfloat one_fraction = unique_path[path_index].one_fraction;
  const tfloat zero_fraction = unique_path[path_index].zero_fraction;
  tfloat next_one_portion = unique_path[unique_depth].pweight;
  tfloat total = 0;
  for (int i = unique_depth - 1; i >= 0; --i) {
    if (one_fraction != 0) {
      const tfloat tmp = next_one_portion * (unique_depth + 1)
                            / static_cast<tfloat>((i + 1) * one_fraction);
      total += tmp;
      next_one_portion = unique_path[i].pweight - tmp * zero_fraction * ((unique_depth - i)
                         / static_cast<tfloat>(unique_depth + 1));
    } else {
      total += (unique_path[i].pweight / zero_fraction) / ((unique_depth - i)
               / static_cast<tfloat>(unique_depth + 1));
    }
  }
  return total;
}

// recursive computation of SHAP values for a decision tree
inline void tree_shap_recursive(const unsigned num_outputs, const int *children_left,
                                const int *children_right,
                                const int *children_default, const int *features,
                                const tfloat *thresholds, const tfloat *values,
                                const tfloat *node_sample_weight,
                                const tfloat *x, const bool *x_missing, tfloat *phi,
                                unsigned node_index, unsigned unique_depth,
                                PathElement *parent_unique_path, tfloat parent_zero_fraction,
                                tfloat parent_one_fraction, int parent_feature_index,
                                int condition, unsigned condition_feature,
                                tfloat condition_fraction, bool less_than_or_equal) {

  // stop if we have no weight coming down to us
  if (condition_fraction == 0) return;

  // extend the unique path
  PathElement *unique_path = parent_unique_path + unique_depth + 1;
  std::copy(parent_unique_path, parent_unique_path + unique_depth + 1, unique_path);

  if (condition == 0 || condition_feature != static_cast<unsigned>(parent_feature_index)) {
    extend_path(unique_path, unique_depth, parent_zero_fraction,
                parent_one_fraction, parent_feature_index);
  }
  const unsigned split_index = features[node_index];

  // leaf node
  if (children_right[node_index] < 0) {
    for (unsigned i = 1; i <= unique_depth; ++i) {
      const tfloat w = unwound_path_sum(unique_path, unique_depth, i);
      const PathElement &el = unique_path[i];
      const unsigned phi_offset = el.feature_index * num_outputs;
      const unsigned values_offset = node_index * num_outputs;
      const tfloat scale = w * (el.one_fraction - el.zero_fraction) * condition_fraction;
      for (unsigned j = 0; j < num_outputs; ++j) {
        phi[phi_offset + j] += scale * values[values_offset + j];
      }
    }

  // internal node
  } else {
    // find which branch is "hot" (meaning x would follow it)
    unsigned hot_index = 0;
    if (x_missing[split_index]) {
      hot_index = children_default[node_index];
    } else if ((less_than_or_equal && x[split_index] <= thresholds[node_index]) ||
               (!less_than_or_equal && x[split_index] < thresholds[node_index])) {
      hot_index = children_left[node_index];
    } else {
      hot_index = children_right[node_index];
    }
    const unsigned cold_index = (static_cast<int>(hot_index) == children_left[node_index] ?
                                 children_right[node_index] : children_left[node_index]);
    const tfloat w = node_sample_weight[node_index];
    const tfloat hot_zero_fraction = node_sample_weight[hot_index] / w;
    const tfloat cold_zero_fraction = node_sample_weight[cold_index] / w;
    tfloat incoming_zero_fraction = 1;
    tfloat incoming_one_fraction = 1;

    // see if we have already split on this feature,
    // if so we undo that split so we can redo it for this node
    unsigned path_index = 0;
    for (; path_index <= unique_depth; ++path_index) {
      if (static_cast<unsigned>(unique_path[path_index].feature_index) == split_index) break;
    }
    if (path_index != unique_depth + 1) {
      incoming_zero_fraction = unique_path[path_index].zero_fraction;
      incoming_one_fraction = unique_path[path_index].one_fraction;
      unwind_path(unique_path, unique_depth, path_index);
      unique_depth -= 1;
    }

    // divide up the condition_fraction among the recursive calls
    tfloat hot_condition_fraction = condition_fraction;
    tfloat cold_condition_fraction = condition_fraction;
    if (condition > 0 && split_index == condition_feature) {
      cold_condition_fraction = 0;
      unique_depth -= 1;
    } else if (condition < 0 && split_index == condition_feature) {
      hot_condition_fraction *= hot_zero_fraction;
      cold_condition_fraction *= cold_zero_fraction;
      unique_depth -= 1;
    }

    tree_shap_recursive(
      num_outputs, children_left, children_right, children_default, features, thresholds, values,
      node_sample_weight, x, x_missing, phi, hot_index, unique_depth + 1, unique_path,
      hot_zero_fraction * incoming_zero_fraction, incoming_one_fraction,
      split_index, condition, condition_feature, hot_condition_fraction, less_than_or_equal
    );

    tree_shap_recursive(
      num_outputs, children_left, children_right, children_default, features, thresholds, values,
      node_sample_weight, x, x_missing, phi, cold_index, unique_depth + 1, unique_path,
      cold_zero_fraction * incoming_zero_fraction, 0,
      split_index, condition, condition_feature, cold_condition_fraction, less_than_or_equal
    );
  }
}

inline int compute_expectations(unsigned num_outputs, const int *children_left, const int *children_right,
                                const tfloat *node_sample_weight, tfloat *values,
                                int i, int depth) {
    if (children_right[i] == -1) {
      return 0;
    } else {
      const unsigned li = children_left[i];
      const unsigned ri = children_right[i];
      const unsigned depth_left = compute_expectations(
        num_outputs, children_left, children_right, node_sample_weight, values, li, depth + 1
      );
      const unsigned depth_right = compute_expectations(
        num_outputs, children_left, children_right, node_sample_weight, values, ri, depth + 1
      );
      const tfloat left_weight = node_sample_weight[li];
      const tfloat right_weight = node_sample_weight[ri];
      const unsigned li_offset = li * num_outputs;
      const unsigned ri_offset = ri * num_outputs;
      const unsigned i_offset = i * num_outputs;
      for (unsigned j = 0; j < num_outputs; ++j) {
        const tfloat v = (left_weight * values[li_offset + j] + right_weight * values[ri_offset + j]) / (left_weight + right_weight);
        values[i_offset + j] = v;
      }
      // const tfloat v = (left_weight * values[li] + right_weight * values[ri]) / (left_weight + right_weight);
      // values[i] = v;
      return std::max(depth_left, depth_right) + 1;
    }
}

inline void tree_shap(const unsigned M, const unsigned num_outputs, const unsigned max_depth,
                      const int *children_left, const int *children_right,
                      const int *children_default, const int *features,
                      const tfloat *thresholds, const tfloat *values,
                      const tfloat *node_sample_weight,
                      const tfloat *x, const bool *x_missing,
                      tfloat *out_contribs, int condition,
                      unsigned condition_feature, bool less_than_or_equal) {

  // update the reference value with the expected value of the tree's predictions
  if (condition == 0) {
    for (unsigned j = 0; j < num_outputs; ++j) {
      out_contribs[M * num_outputs + j] += values[j];
    }
  }

  // Preallocate space for the unique path data
  const unsigned maxd = max_depth + 2; // need a bit more space than the max depth
  PathElement *unique_path_data = new PathElement[(maxd * (maxd + 1)) / 2];

  tree_shap_recursive(
    num_outputs, children_left, children_right, children_default, features, thresholds, values,
    node_sample_weight, x, x_missing, out_contribs, 0, 0, unique_path_data,
    1, 1, -1, condition, condition_feature, 1, less_than_or_equal
  );
  delete[] unique_path_data;
}

// inline void tree_shap(const unsigned M, const unsigned num_outputs, const unsigned max_depth,
//                       const int *children_left, const int *children_right,
//                       const int *children_default, const int *features,
//                       const tfloat *thresholds, const tfloat *values,
//                       const tfloat *node_sample_weight,
//                       const tfloat *x, const bool *x_missing,
//                       tfloat *out_contribs, int condition,
//                       unsigned condition_feature) {
//
//   // update the reference value with the expected value of the tree's predictions
//   if (condition == 0) {
//     for (unsigned j = 0; j < num_outputs; ++j) {
//       out_contribs[M * num_outputs + j] += values[j];
//     }
//   }
//
//   // Preallocate space for the unique path data
//   const unsigned maxd = max_depth + 2; // need a bit more space than the max depth
//   PathElement *unique_path_data = new PathElement[(maxd * (maxd + 1)) / 2];
//
//   tree_shap_recursive(
//     num_outputs, children_left, children_right, children_default, features, thresholds, values,
//     node_sample_weight, x, x_missing, out_contribs, 0, 0, unique_path_data,
//     1, 1, -1, condition, condition_feature, 1
//   );
//   delete[] unique_path_data;
// }

// Independent Tree SHAP functions below here
// ------------------------------------------

// https://www.geeksforgeeks.org/space-and-time-efficient-binomial-coefficient/
inline int bin_coeff(int n, int k) { 
    int res = 1; 
    if (k > n - k)
        k = n - k; 
    for (int i = 0; i < k; ++i) { 
        res *= (n - i); 
        res /= (i + 1); 
    } 
    return res; 
} 

inline float calc_weight(const int N, const int M) {
  return(1.0/(N*bin_coeff(N-1,M)));
}

inline void tree_shap_indep(const unsigned max_depth, const unsigned num_feats, 
                            const unsigned num_nodes, const int *children_left, 
                            const int *children_right, const int *features, 
                            const tfloat *thresholds, const tfloat *values, 
                            const tfloat *x, const tfloat *r, tfloat *out_contribs) {
  const bool DEBUG = false;
  ofstream myfile;
  if (DEBUG) {
    myfile.open ("/homes/gws/hughchen/shap/out.txt",fstream::app);
    myfile << "Entering tree_shap_indep\n";
  }
  int node_stack[max_depth];
  int ns_ctr = 0;
  signed short feat_hist[num_feats] = {0};
  float pos_lst[num_nodes] = {0};
  float neg_lst[num_nodes] = {0};
  int node = 0, feat, cl, cr, pnode, pfeat = -1, pcl, pcr;
  tfloat thres, pthres;
  bool from_x, from_r;
  float pos_x = 0, neg_x = 0, pos_r = 0, neg_r = 0;
  unsigned M = 0, N = 0;
  int next_node = -1, from_child = -1;
  
  feat = features[node];
  thres = thresholds[node];
  cr = children_right[node];
  cl = children_left[node];

  if (DEBUG) {
    myfile << "\nNode: " << node << "\n";
    myfile << "x[feat]: " << x[feat] << ", r[feat]: " << r[feat] << "\n";
    myfile << "thres: " << thres << "\n";
  }

  // Check if x and r go the same way
  if ((x[feat] <= thres) && (r[feat] <= thres)) {
    next_node = cl;
  } else if ((x[feat] > thres) && (r[feat] > thres)) {
    next_node = cr;
  }
  
  // If not, go left
  if (next_node == -1) {
    next_node = cl;
    if ((r[feat] <= thres) && (x[feat] > thres)) {
      N = N+1;
      feat_hist[feat] -= 1;
    } else if ((x[feat] <= thres) && (r[feat] > thres)) {
      M = M+1;
      N = N+1;
      feat_hist[feat] += 1;
    }
  }
  node_stack[ns_ctr] = node;
  ns_ctr += 1;
  while (true) {
    node = next_node;
    feat = features[node];
    thres = thresholds[node];
    cr = children_right[node];
    cl = children_left[node];

    // Get parent information
    pnode = -2;
    from_x = false;
    from_r = false;
    // Don't do this for the root
    if (node != 0) {
      pnode = node_stack[ns_ctr-1];
      pfeat = features[pnode];
      pthres = thresholds[pnode];
      pcr = children_right[pnode];       
      pcl = children_left[pnode];
      if (node == pcl) {
        from_x = x[pfeat] <= pthres;
        from_r = r[pfeat] <= pthres;
      } else if (node == pcr) {
        from_x = x[pfeat] > pthres;
        from_r = r[pfeat] > pthres;
      }
    }
    if (DEBUG) {
      myfile << "\nNode: " << node << "\n";
      myfile << "N: " << N << ", M: " << M << "\n";
      myfile << "x[feat]: " << x[feat] << ", r[feat]: " << r[feat] << "\n";
      myfile << "thres: " << thres << "\n";
    }

    // At a leaf
    if ((cl < 0) && (cr < 0)) {
      if (DEBUG) {
        myfile << "At a leaf\n";
      }
      // Currently assuming a single output
      if (N != 0) {
        if (M != 0) {
          pos_lst[node] = values[node]*calc_weight(N,M-1);
        }
        if (M != N) {
          neg_lst[node] = -values[node]*calc_weight(N,M);
        }
      }
      if (DEBUG) {
        myfile << "pos_lst[node]: " << pos_lst[node] << "\n";
        myfile << "neg_lst[node]: " << neg_lst[node] << "\n";
      }
      // Pop from node_stack
      ns_ctr -= 1;
      next_node = node_stack[ns_ctr];
      from_child = node;
      // Unwind
      if (feat_hist[pfeat] > 0) {
        feat_hist[pfeat] -= 1;
      } else if (feat_hist[pfeat] < 0) {
        feat_hist[pfeat] += 1;  
      }   
      if (feat_hist[pfeat] == 0) {
        if (from_x && !from_r) {
          N = N-1;
          M = M-1;
        } else if (!from_x && from_r) {
          N = N-1;
        }
      }
      continue;
    }
   
    // Arriving at node from parent
    if (from_child == -1) {
      if (DEBUG) {
        myfile << "Arriving at node from parent\n";
      }
      node_stack[ns_ctr] = node;
      ns_ctr += 1;
      next_node = -1;
    
      if (DEBUG) {
        myfile << "feat_hist[feat]" << feat_hist[feat] << "\n";
      }
      // Feature is set upstream
      if (feat_hist[feat] > 0) {
        if (x[feat] <= thres) {
          next_node = cl;
        } else {
          next_node = cr;
        }
        feat_hist[feat] += 1;
      } else if (feat_hist[feat] < 0) {
        if (r[feat] <= thres) {
          next_node = cl;
        } else {
          next_node = cr;
        }
        feat_hist[feat] -= 1;
      }

      // x and r go the same way
      if (next_node == -1) {
        if ((x[feat] <= thres) && (r[feat] <= thres)) {
          next_node = cl;
        } else if ((x[feat] > thres) && (r[feat] > thres)) {
          next_node = cr;
        }
      }
    
      // Go down one path
      if (next_node != -1) {
        continue;
      }

      // Go down both paths, but go left first
      next_node = cl;
      if ((r[feat] <= thres) && (x[feat] > thres)) {
        N = N+1;
        feat_hist[feat] -= 1;
      } else if ((x[feat] <= thres) && (r[feat] > thres)) {
        M = M+1;
        N = N+1;
        feat_hist[feat] += 1;
      }
      from_child = -1;
      continue;
    }
      
    // Arriving at node from child
    if (from_child != -1) {
      if (DEBUG) {
        myfile << "Arriving at node from child\n";
      }
      next_node = -1;
      // Check if we should unroll immediately
      if ((x[feat] <= thres) && (r[feat] <= thres)) {
        next_node = pnode;
      } else if ((x[feat] > thres) && (r[feat] > thres)) {
        next_node = pnode;
      }
      if (feat_hist[feat] != 0) {
        next_node = pnode;
      }

      // Came from a single path, so unroll
      if (next_node != -1) { 
        if (DEBUG) {
          myfile << "Came from a single path, so unroll\n";   
        }
        // At the root node
        if (node == 0) {
          break;
        }
        // Update and unroll
        pos_lst[node] = pos_lst[from_child];
        neg_lst[node] = neg_lst[from_child];
        if (DEBUG) {
          myfile << "pos_lst[node]: " << pos_lst[node] << "\n";
          myfile << "neg_lst[node]: " << neg_lst[node] << "\n";
        }
        from_child = node;
        ns_ctr -= 1;

        // Unwind
        if (feat_hist[pfeat] > 0) {
          feat_hist[pfeat] -= 1;
        } else if (feat_hist[pfeat] < 0) {
          feat_hist[pfeat] += 1;  
        }
        if (feat_hist[pfeat] == 0) {
          if (from_x && !from_r) {
            N = N-1;
            M = M-1;
          } else if (!from_x && from_r) {
            N = N-1;
          }
        }
        continue;
      // Go right - Arriving from the left child
      } else if (from_child == cl) {
        if (DEBUG) {
          myfile << "Go right - Arriving from the left child\n";
        }
        node_stack[ns_ctr] = node;
        ns_ctr += 1;
        next_node = cr;
        if ((r[feat] <= thres) && (x[feat] > thres)) {
          M = M+1;
          N = N+1;
          feat_hist[feat] += 1;
        } else if ((x[feat] <= thres) && (r[feat] > thres)) {
          N = N+1;
          feat_hist[feat] -= 1;
        }
        from_child = -1;
        continue;
      // Compute stuff and unroll - Arriving from the right child
      } else if (from_child == cr) {
        if (DEBUG) {
          myfile << "Compute stuff and unroll - Arriving from the right child\n";
        }
        pos_x = 0;
        neg_x = 0;
        pos_r = 0;
        neg_r = 0;
        if ((r[feat] <= thres) && (x[feat] > thres)) {
          pos_x = pos_lst[cr];
          neg_x = neg_lst[cr];
          pos_r = pos_lst[cl];
          neg_r = neg_lst[cl];
        } else if ((x[feat] <= thres) && (r[feat] > thres)) {
          pos_x = pos_lst[cl];
          neg_x = neg_lst[cl];
          pos_r = pos_lst[cr];
          neg_r = neg_lst[cr];
        }
        // out_contribs needs to have been initialized as all zeros
        out_contribs[feat] += pos_x + neg_r;
        pos_lst[node] = pos_x + pos_r;
        neg_lst[node] = neg_x + neg_r;
        if (DEBUG) {
          myfile << "out_contribs[feat]: " << out_contribs[feat] << "\n";
          myfile << "pos_lst[node]: " << pos_lst[node] << "\n";
          myfile << "neg_lst[node]: " << neg_lst[node] << "\n";
        }

        // Check if at root
        if (node == 0) {
          break;
        }
          
        // Pop
        ns_ctr -= 1;
        next_node = node_stack[ns_ctr];
        from_child = node;
          
        // Unwind
        if (feat_hist[pfeat] > 0) {
          feat_hist[pfeat] -= 1;
        } else if (feat_hist[pfeat] < 0) {
          feat_hist[pfeat] += 1;  
        }
        if (feat_hist[pfeat] == 0) {
          if (from_x && !from_r) {
            N = N-1;
            M = M-1;
          } else if (!from_x && from_r) {
            N = N-1;
          }
        }
        continue;
      }
    }
  }
  if (DEBUG) {
    myfile.close();
  }
}

