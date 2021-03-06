//==================================================
// viewpoint_planner_graph.cpp
//
//  Copyright (c) 2016 Benjamin Hepp.
//  Author: Benjamin Hepp
//  Created on: Mar 9, 2017
//==================================================

#include "viewpoint_planner.h"
#include <boost/graph/connected_components.hpp>

ViewpointPlanner::ViewpointEntryIndex ViewpointPlanner::addViewpointEntry(
        const Pose& pose, const bool no_raycast) {
  const bool verbose = true;

  const Viewpoint viewpoint = getVirtualViewpoint(pose);
  // Compute observed voxels and information and discard if too few voxels or too little information.
  // Also discard if too close to too many voxels.
  try {
    if (!no_raycast && !options_.viewpoint_no_raycast) {
      const bool ignore_voxels_with_zero_information = true;
      std::pair<VoxelWithInformationSet, FloatType> raycast_result =
              getRaycastHitVoxelsWithInformationScore(viewpoint, ignore_voxels_with_zero_information);
      VoxelWithInformationSet &voxel_set = raycast_result.first;
      FloatType total_information = raycast_result.second;
      //    if (verbose) {
      //      std::cout << "voxel_set.size()=" << voxel_set.size() << std::endl;
      //    }
      if (voxel_set.size() < options_.viewpoint_min_voxel_count) {
        if (verbose) {
          std::cout << "voxel_set.size() < options_.viewpoint_min_voxel_count" << std::endl;
        }
        return (ViewpointEntryIndex)-1;
      }
      //  if (verbose) {
      //    std::cout << "total_information=" << total_information << std::endl;
      //  }
      if (total_information < options_.viewpoint_min_information) {
        if (verbose) {
          std::cout << "total_information < options_.viewpoint_min_information" << std::endl;
        }
        return (ViewpointEntryIndex)-1;
      }

      size_t too_close_voxel_count = 0;
      for (const VoxelWithInformation &vi : voxel_set) {
        const FloatType squared_distance = (viewpoint.pose().getWorldPosition() -
                                            vi.voxel->getBoundingBox().getCenter()).squaredNorm();
        if (squared_distance < options_.viewpoint_voxel_distance_threshold) {
          ++too_close_voxel_count;
        }
      }
      const FloatType too_close_voxel_ratio = too_close_voxel_count / voxel_set.size();
      if (too_close_voxel_ratio >= options_.viewpoint_max_too_close_voxel_ratio) {
        if (verbose) {
          std::cout << "too_close_voxel_ratio < options_.viewpoint_max_too_close_voxel_ratio" << std::endl;
        }
      }

      const bool ignore_viewpoint_count_grid = true;
      const ViewpointEntryIndex new_viewpoint_index = addViewpointEntry(
              ViewpointEntry(Viewpoint(&virtual_camera_, pose), total_information, std::move(voxel_set)),
              ignore_viewpoint_count_grid);
      viewpoint_exploration_front_.push_back(new_viewpoint_index);
      return new_viewpoint_index;
    }
    else {
      VoxelWithInformationSet voxel_set;
      const FloatType total_information = 0;
      const bool ignore_viewpoint_count_grid = true;
      const ViewpointEntryIndex new_viewpoint_index = addViewpointEntry(
              ViewpointEntry(Viewpoint(&virtual_camera_, pose), total_information, std::move(voxel_set)),
              ignore_viewpoint_count_grid);
      viewpoint_exploration_front_.push_back(new_viewpoint_index);
      return new_viewpoint_index;
    }
  }
    // TODO: Should be an exception for raycast
  catch (const OccupiedTreeType::Error& err) {
    std::cout << "Raycast failed: " << err.what() << std::endl;
  }
  return (ViewpointEntryIndex)-1;
}

ViewpointPlanner::ViewpointEntryIndex ViewpointPlanner::addViewpointEntryWithoutLock(
        ViewpointEntry&& viewpoint_entry, const bool ignore_viewpoint_count_grid) {
  const ViewpointEntryIndex viewpoint_index = viewpoint_entries_.size();
  const Vector3 viewpoint_position = viewpoint_entry.viewpoint.pose().getWorldPosition();
//  std::cout << "Adding position to ANN index" << std::endl;
  viewpoint_ann_.addPoint(viewpoint_position);
//  std::cout << "Putting viewpoint entry into array" << std::endl;
  if (viewpoint_entries_.capacity() == viewpoint_entries_.size()) {
    std::cout << "Increasing capacity for viewpoint entries" << std::endl;
    const size_t new_viewpoint_entries_capacity = (size_t)std::ceil(1.25 * viewpoint_entries_.size());
    viewpoint_entries_.reserve(new_viewpoint_entries_capacity);
  }
  viewpoint_entries_.emplace_back(std::move(viewpoint_entry));
  stereo_viewpoint_indices_.emplace_back((ViewpointEntryIndex)-1);
  stereo_viewpoint_computed_flags_.push_back(false);
//  std::cout << "Adding node to viewpoint graph" << std::endl;
  viewpoint_graph_.addNode(viewpoint_index);
  viewpoint_graph_components_valid_ = false;
  // TODO
//#if !BH_RELEASE
//  if (!ignore_viewpoint_count_grid && viewpoint_entries_.size() > num_real_viewpoints_) {
//    BH_ASSERT(viewpoint_count_grid_.isInsideGrid(viewpoint_position));
//  }
//#endif
  if (!ignore_viewpoint_count_grid && options_.viewpoint_count_grid_enable) {
    if (viewpoint_count_grid_.isInsideGrid(viewpoint_position)) {
      if (viewpoint_entries_.size() > num_real_viewpoints_) {
        viewpoint_count_grid_(viewpoint_position) += 1;
      }
      // Check if viewpoint sampling distribution should be updated
      const size_t increase_since_last_update =
              viewpoint_entries_.size() - viewpoint_sampling_distribution_update_size_;
      const FloatType relative_increase_since_last_update =
              increase_since_last_update / FloatType(viewpoint_entries_.size());
      if (viewpoint_entries_.size() >= num_real_viewpoints_
          && (viewpoint_entries_.size() == num_real_viewpoints_ + 1
              || relative_increase_since_last_update >= FloatType(0.05))) {
        std::cout << "Updating viewpoint sampling distribution" << std::endl;
        updateViewpointSamplingDistribution();
        viewpoint_sampling_distribution_update_size_ = viewpoint_entries_.size();
      }
    }
  }
  return viewpoint_index;
}

ViewpointPlanner::ViewpointEntryIndex ViewpointPlanner::addViewpointEntryWithoutLock(
        const ViewpointEntry& viewpoint_entry, const bool ignore_viewpoint_count_grid) {
  ViewpointEntry viewpoint_entry_copy = viewpoint_entry;
  return addViewpointEntryWithoutLock(std::move(viewpoint_entry_copy), ignore_viewpoint_count_grid);
}

void ViewpointPlanner::updateViewpointSamplingDistribution() {
  std::vector<FloatType> weights;
  weights.reserve(viewpoint_entries_.size() - num_real_viewpoints_);
  const auto grid_count_lambda = [&](const ViewpointEntry& entry) {
    const Vector3 viewpoint_position = entry.viewpoint.pose().getWorldPosition();
    return viewpoint_count_grid_(viewpoint_position);
  };
  const auto max_it = std::max_element(viewpoint_entries_.begin() + num_real_viewpoints_, viewpoint_entries_.end(),
                                       [&] (const ViewpointEntry& entry1, const ViewpointEntry& entry2) {
    return grid_count_lambda(entry1) < grid_count_lambda(entry2);
  });
  const FloatType max_grid_count = grid_count_lambda(*max_it);
//  FloatType exp_factor = 20;
  FloatType exp_factor = 1;
  if (viewpoint_entries_.size() >= num_real_viewpoints_ + 100) {
    exp_factor = 1;
  }
  for (auto it = viewpoint_entries_.begin() + num_real_viewpoints_; it != viewpoint_entries_.end(); ++it) {
    const FloatType grid_count = grid_count_lambda(*it);
    BH_ASSERT(grid_count > 0);
    const FloatType weight = std::exp(-grid_count * exp_factor / max_grid_count);
    weights.push_back(weight);
  }
  viewpoint_sampling_distribution_ = std::discrete_distribution<size_t>(weights.begin(), weights.end());
  const FloatType grid_exp_factor = 2;
//  // Display non-empty grid cells
  if (viewpoint_entries_.size() >= num_real_viewpoints_ + 100) {
  //  std::cout << "Viewpoint grid:" << std::endl;
    for (size_t ix = 0; ix < viewpoint_count_grid_.getDimX(); ++ix) {
      for (size_t iy = 0; iy < viewpoint_count_grid_.getDimY(); ++iy) {
        for (size_t iz = 0; iz < viewpoint_count_grid_.getDimZ(); ++iz) {
          const size_t index = viewpoint_count_grid_.getIndex(ix, iy, iz);
          const FloatType grid_count = viewpoint_count_grid_(ix, iy, iz);
          const FloatType probability = std::exp(-grid_count * grid_exp_factor / max_grid_count);
          grid_cell_probabilities_[index] = probability;
  //        if (grid_count > 0) {
  //          std::cout << "  grid element (" << ix << ", " << iy << ", " << iz << ") has " << grid_count << " viewpoints" << std::endl;
  //        }
        }
      }
    }
  }
  else {
    std::fill(grid_cell_probabilities_.begin(), grid_cell_probabilities_.end(), 1);
  }
}

ViewpointPlanner::ViewpointEntryIndex ViewpointPlanner::addViewpointEntry(
        ViewpointEntry&& viewpoint_entry, const bool ignore_viewpoint_count_grid) {
  std::unique_lock<std::mutex> lock(mutex_);
  const ViewpointEntryIndex viewpoint_index = addViewpointEntryWithoutLock(
          std::move(viewpoint_entry), ignore_viewpoint_count_grid);
  lock.unlock();
  return viewpoint_index;
}

ViewpointPlanner::ViewpointEntryIndex ViewpointPlanner::addViewpointEntry(
        const ViewpointEntry& viewpoint_entry, const bool ignore_viewpoint_count_grid) {
  ViewpointEntry viewpoint_entry_copy = viewpoint_entry;
  return addViewpointEntry(std::move(viewpoint_entry_copy), ignore_viewpoint_count_grid);
}

const std::pair<std::vector<std::size_t>, std::size_t>& ViewpointPlanner::getConnectedComponents() const {
  if (!viewpoint_graph_components_valid_) {
    std::vector<std::size_t> component(viewpoint_graph_.numVertices());
    std::size_t num_components = boost::connected_components(viewpoint_graph_.boostGraph(), &component.front());
    viewpoint_graph_components_.first = std::move(component);
    viewpoint_graph_components_.second = num_components;
  }
  return viewpoint_graph_components_;
}

bool ViewpointPlanner::generateNextViewpointEntry() {
  const bool verbose = true;

  // If we have failed to sample the second viewpoint too many times we clear the viewpoint graph and start over again.
  const std::size_t max_failed_second_viewpoint_entry_samples = 50;
  if (viewpoint_entries_.size() == num_real_viewpoints_ + 1
      && num_of_failed_viewpoint_entry_samples_ >= max_failed_second_viewpoint_entry_samples) {
    std::cout << "WARNING: Too many failed attempts to sample the second viewpoints. Resetting viewpoint graph." << std::endl;
    reset();
  }

  // Sample viewpoint and add it to the viewpoint graph

  bool found_sample;
  Pose sampled_pose;
  ViewpointEntryIndex reference_viewpoint_index = (ViewpointEntryIndex)-1;

//  if (verbose) {
//    std::cout << "Trying to sample viewpoint" << std::endl;
//  }
  if (viewpoint_entries_.size() <= num_real_viewpoints_) {
    if (options_.isSet("drone_start_position")) {
      const Pose drone_start_pose = Pose::createFromImageToWorldTransformation(options_.drone_start_position, Quaternion::Identity());
      std::cout << "Sampling around start position" << std::endl;
      std::tie(found_sample, sampled_pose) = sampleSurroundingPose(drone_start_pose);
    }
    else {
      reference_viewpoint_index = random_.sampleUniformIntExclusive(viewpoint_entries_.size());
      const Pose& reference_pose = viewpoint_entries_[reference_viewpoint_index].viewpoint.pose();
      std::cout << "Sampling around previous viewpoint position" << std::endl;
      std::tie(found_sample, sampled_pose) = sampleSurroundingPose(reference_pose);
    }
  }
  else {
    const bool sample_without_reference = random_.sampleBernoulli(options_.viewpoint_sample_without_reference_probability);
    if (sample_without_reference) {
      std::cout << "Sampling uniformly in pose sample bounding box" << std::endl;
      std::tie(found_sample, sampled_pose) = samplePose(pose_sample_bbox_, drone_bbox_);
    }
    else {
      std::cout << "Sampling around existing viewpoint" << std::endl;
      const auto viewpoint_it = sampleViewpointByGridCounts(
          viewpoint_entries_.cbegin() + num_real_viewpoints_, viewpoint_entries_.cend());
      if (viewpoint_it == viewpoint_entries_.cend()) {
        std::cout << "Could not sample viewpoint from grid" << std::endl;
        found_sample = false;
      }
      else {
        const Pose& reference_pose = viewpoint_it->viewpoint.pose();
        std::tie(found_sample, sampled_pose) = sampleSurroundingPose(reference_pose);
        if (found_sample) {
          if (options_.viewpoint_count_grid_enable) {
            const size_t grid_index = viewpoint_count_grid_.getIndex(
                    viewpoint_count_grid_.getGridIndices(sampled_pose.getWorldPosition()));
            const FloatType prob = grid_cell_probabilities_[grid_index];
            const FloatType u = random_.sampleUniform();
            if (u > prob) {
              std::cout << "Sample was rejected because of viewpoint count grid" << std::endl;
              found_sample = false;
            }
          }
        }
        else {
          std::cout << "Could not sample surrounding pose" << std::endl;
        }
      }
    }
  }
  if (!found_sample) {
//    if (verbose) {
//      std::cout << "Failed to sample pose." << i << std::endl;
//    }
    ++num_of_failed_viewpoint_entry_samples_;
    return false;
  }

  const Viewpoint viewpoint = getVirtualViewpoint(sampled_pose);

  // Check distance to other viewpoints and discard if too close
  const std::size_t dist_knn = options_.viewpoint_discard_dist_knn;
  const FloatType dist_thres_square = options_.viewpoint_discard_dist_thres_square;
  const std::size_t dist_count_thres = options_.viewpoint_discard_dist_count_thres;
  const FloatType dist_real_thres_square = options_.viewpoint_discard_dist_real_thres_square;
  static std::vector<ViewpointANN::IndexType> knn_indices;
  static std::vector<ViewpointANN::DistanceType> knn_distances;
  knn_indices.resize(dist_knn);
  knn_distances.resize(dist_knn);
  viewpoint_ann_.knnSearch(sampled_pose.getWorldPosition(), dist_knn, &knn_indices, &knn_distances);
  std::size_t too_close_count = 0;
//  for (ViewpointANN::IndexType viewpoint_index : knn_indices) {
//    const ViewpointEntry& other_viewpoint = viewpoint_entries_[viewpoint_index];
//    FloatType dist_square = (pose.getWorldPosition() - other_viewpoint.viewpoint.pose().getWorldPosition()).squaredNorm();
  for (std::size_t i = 0; i < knn_distances.size(); ++i) {
    const ViewpointANN::DistanceType dist_square = knn_distances[i];
    const ViewpointEntryIndex viewpoint_index = knn_indices[i];
    if (viewpoint_index < num_real_viewpoints_ && dist_square < dist_real_thres_square) {
      if (verbose) {
        std::cout << "Rejected: Too close to real viewpoint" << std::endl;
      }
      ++num_of_failed_viewpoint_entry_samples_;
      return false;
    }
//    std::cout << "dist_square=" << dist_square << std::endl;
    if (dist_square < dist_thres_square) {
      ++too_close_count;
//      if (verbose) {
//        std::cout << "dist_square=" << dist_square << std::endl;
//      }
      if (too_close_count > dist_count_thres) {
        break;
      }
    }
  }

//  // Test code for approximate nearest neighbor search
//  static std::vector<ViewpointANN::IndexType> knn_indices2;
//  static std::vector<ViewpointANN::DistanceType> knn_distances2;
//  knn_indices2.resize(dist_knn);
//  knn_distances2.resize(dist_knn);
//  viewpoint_ann_.knnSearchExact(pose.getWorldPosition(), dist_knn, &knn_indices2, &knn_distances2);
//  std::size_t too_close_count2 = 0;
//  for (ViewpointANN::IndexType viewpoint_index : knn_indices2) {
//    const ViewpointEntry& other_viewpoint = viewpoint_entries_[viewpoint_index];
//    FloatType dist_square = (pose.getWorldPosition() - other_viewpoint.viewpoint.pose().getWorldPosition()).squaredNorm();
////    std::cout << "dist_square=" << dist_square << std::endl;
//    if (dist_square < dist_thres_square) {
//      ++too_close_count2;
////      if (verbose) {
////        std::cout << "dist_square=" << dist_square << std::endl;
////      }
//      if (too_close_count2 > dist_count_thres) {
//        break;
//      }
//    }
//  }
//  // This is not necessarily true because of approximate nearest neighbor search but for testing we can quickly see if there is a bug.
//  if (too_close_count != too_close_count2) {
//    std::cerr << "TEST ERROR: too_close_count != too_close_count2" << std::endl;
//    std::cerr << "too_close_count=" << too_close_count << ", too_close_count2=" << too_close_count2 << std::endl;
//  }
//  if (too_close_count > too_close_count2) {
//    std::cout << "too_close_count > too_close_count2" << std::endl;
//    for (std::size_t i = 0; i < knn_indices.size(); ++i) {
//      std::cout << "knn_indices[" << i << "]=" << knn_indices[i] << std::endl;
//      std::cout << "knn_distances[" << i << "]=" << knn_distances[i] << std::endl;
//      std::cout << "getPoint(" << i << ")=" << viewpoint_ann_.getPoint(i) << std::endl;
//      std::cout << "viewpoint_entry[" << knn_indices[i] << "]=" << viewpoint_entries_[knn_indices[i]].viewpoint.pose().getWorldPosition().transpose() << std::endl;
//      std::cout << "knn_indices2[" << i << "]=" << knn_indices2[i] << std::endl;
//      std::cout << "knn_distances2[" << i << "]=" << knn_distances2[i] << std::endl;
//      std::cout << "viewpoint_entry[" << knn_indices2[i] << "]=" << viewpoint_entries_[knn_indices2[i]].viewpoint.pose().getWorldPosition().transpose() << std::endl;
//    }
//  }
//  BH_ASSERT(too_close_count <= too_close_count2);
//  // End of testing code
//  if (verbose) {
//    std::cout << "too_close_count=" << too_close_count << std::endl;
//  }

  if (too_close_count > dist_count_thres) {
//    if (verbose) {
//      std::cout << "Rejected: Too close to other viewpoints" << std::endl;
//    }
    ++num_of_failed_viewpoint_entry_samples_;
    return false;
  }

  // TODO
//  bool found_neighbour = false;
//  bool found_motion = false;
//  SE3Motion se3_motion;
//  if (reference_viewpoint_index == (ViewpointEntryIndex)-1 && viewpoint_entries_.size() > num_real_viewpoints_) {
//    // Make sure we can find at least one neighbour that is reachable and matchable. Otherwise, discard.
//    const std::size_t knn = options_.viewpoint_sample_motion_and_matching_knn;
//    static std::vector<ViewpointANN::IndexType> knn_indices;
//    static std::vector<ViewpointANN::DistanceType> knn_distances;
//    knn_indices.resize(knn);
//    knn_distances.resize(knn);
//    viewpoint_ann_.knnSearch(sampled_pose.getWorldPosition(), knn, &knn_indices, &knn_distances);
//    for (std::size_t i = 0; i < knn_distances.size(); ++i) {
//      const ViewpointANN::DistanceType dist_square = knn_distances[i];
//      const ViewpointEntryIndex other_viewpoint_index = knn_indices[i];
//      if (other_viewpoint_index < num_real_viewpoints_) {
//        continue;
//      }
//      const Viewpoint& other_viewpoint = viewpoint_entries_[other_viewpoint_index].viewpoint;
////      const std::unordered_map<Point3DId, ViewpointPlanner::Vector3>& visible_points = computeVisibleSparsePoints(viewpoint);
////      const std::unordered_map<Point3DId, ViewpointPlanner::Vector3>& other_visible_points = getCachedVisibleSparsePoints(other_viewpoint_index);
////      const bool matchable = isSparseMatchable(
////              viewpoint, other_viewpoint,
////              visible_points, other_visible_points);
////      if (!matchable) {
////        continue;
////      }
////      const std::unordered_set<size_t>& visible_voxels = getRaycastHitVoxelsSet(viewpoint);
//      const std::unordered_set<size_t>& visible_voxels = getVisibleVoxels(viewpoint);
//      const std::unordered_set<size_t>& other_visible_voxels = getCachedVisibleVoxels(other_viewpoint_index);
//      const bool matchable = isSparseMatchable2(
//              viewpoint, other_viewpoint,
//              visible_voxels, other_visible_voxels);
//      if (!matchable) {
//        continue;
//      }
//      const Pose& other_pose = other_viewpoint.pose();
//      SE3Motion local_se3_motion;
//      bool local_found_motion = false;
//      std::tie(local_se3_motion, local_found_motion) = motion_planner_.findMotion(other_pose, sampled_pose);
//      if (!local_found_motion) {
//        continue;
//      }
//      reference_viewpoint_index = other_viewpoint_index;
//      se3_motion = local_se3_motion;
//      found_motion = true;
//      found_neighbour = true;
//      break;
//    }
//    if (!found_neighbour) {
//      std::cout << "Unable to find motion to new sampled pose" << std::endl;
//      ++num_of_failed_viewpoint_entry_samples_;
//      return false;
//    }
//    FloatType motion_distance = 0;
//    if (se3_motion.poses.size() >= 2) {
//      for (auto pose_it = se3_motion.poses.begin() + 1; pose_it != se3_motion.poses.end(); ++pose_it) {
//        motion_distance += (pose_it->getWorldPosition() - (pose_it - 1)->getWorldPosition()).norm();
//      }
//    }
//#if !BH_RELEASE
//    BH_ASSERT(bh::isApproxEqual(se3_motion.distance, motion_distance, FloatType(1e-2)));
//#endif
//  }
//  else if (reference_viewpoint_index != (ViewpointEntryIndex)-1
//      && reference_viewpoint_index >= num_real_viewpoints_) {
//    // Make sure that reference viewpoint is reachable and matchable. Otherwise, discard.
//    const Viewpoint& other_viewpoint = viewpoint_entries_[reference_viewpoint_index].viewpoint;
////    const std::unordered_map<Point3DId, ViewpointPlanner::Vector3>& visible_points = computeVisibleSparsePoints(viewpoint);
////    const std::unordered_map<Point3DId, ViewpointPlanner::Vector3>& other_visible_points = getCachedVisibleSparsePoints(reference_viewpoint_index);
////    const bool matchable = isSparseMatchable(
////            viewpoint, other_viewpoint,
////            visible_points, other_visible_points);
////    if (!matchable) {
////      std::cout << "Unable to match sample reference from new sampled viewpoint" << std::endl;
////      ++num_of_failed_viewpoint_entry_samples_;
////      return false;
////    }
//    const Pose& other_pose = other_viewpoint.pose();
//    std::tie(se3_motion, found_motion) = motion_planner_.findMotion(other_pose, sampled_pose);
//    if (!found_motion) {
//      std::cout << "Unable to find motion from sample reference to new sampled pose" << std::endl;
//      ++num_of_failed_viewpoint_entry_samples_;
//      return false;
//    }
////    const std::unordered_set<size_t>& visible_voxels = getRaycastHitVoxelsSet(viewpoint);
//    const std::unordered_set<size_t>& visible_voxels = getVisibleVoxels(viewpoint);
//    const std::unordered_set<size_t>& other_visible_voxels = getCachedVisibleVoxels(reference_viewpoint_index);
//    const bool matchable = isSparseMatchable2(
//            viewpoint, other_viewpoint,
//            visible_voxels, other_visible_voxels);
//    if (!matchable) {
//      std::cout << "Unable to match sample reference from new sampled viewpoint" << std::endl;
//      ++num_of_failed_viewpoint_entry_samples_;
//      return false;
//    }
//  }

  // Compute observed voxels and information and discard if too few voxels or too little information.
  // Also discard if too close to too many voxels.
  try {
    const bool ignore_voxels_with_zero_information = true;
    std::pair<VoxelWithInformationSet, FloatType> raycast_result =
        getRaycastHitVoxelsWithInformationScore(viewpoint, ignore_voxels_with_zero_information);
    VoxelWithInformationSet& voxel_set = raycast_result.first;
    FloatType total_information = raycast_result.second;
  //    if (verbose) {
  //      std::cout << "voxel_set.size()=" << voxel_set.size() << std::endl;
  //    }
    if (voxel_set.size() < options_.viewpoint_min_voxel_count) {
      if (verbose) {
        std::cout << "voxel_set.size() < options_.viewpoint_min_voxel_count" << std::endl;
      }
      ++num_of_failed_viewpoint_entry_samples_;
      return false;
    }
  //  if (verbose) {
  //    std::cout << "total_information=" << total_information << std::endl;
  //  }
    if (total_information < options_.viewpoint_min_information) {
      if (verbose) {
        std::cout << "total_information < options_.viewpoint_min_information" << std::endl;
      }
      ++num_of_failed_viewpoint_entry_samples_;
      return false;
    }

    size_t too_close_voxel_count = 0;
    for (const VoxelWithInformation& vi : voxel_set) {
      const FloatType squared_distance = (viewpoint.pose().getWorldPosition() - vi.voxel->getBoundingBox().getCenter()).squaredNorm();
      if (squared_distance < options_.viewpoint_voxel_distance_threshold) {
        ++too_close_voxel_count;
      }
    }
    const FloatType too_close_voxel_ratio = too_close_voxel_count / voxel_set.size();
    if (too_close_voxel_ratio >= options_.viewpoint_max_too_close_voxel_ratio) {
      if (verbose) {
        std::cout << "too_close_voxel_ratio < options_.viewpoint_max_too_close_voxel_ratio" << std::endl;
      }
      ++num_of_failed_viewpoint_entry_samples_;
    }

  //  if (verbose) {
  //    std::cout << "pose=" << pose << ", voxel_set=" << voxel_set.size() << ", total_information=" << total_information << std::endl;
  //  }

    addViewpointEntry(
        ViewpointEntry(Viewpoint(&virtual_camera_, sampled_pose), total_information, std::move(voxel_set)));

//    if (found_motion) {
//      ViewpointMotion motion({ reference_viewpoint_index, new_viewpoint_index }, { se3_motion });
//      BH_ASSERT(motion.se3Motions().front().poses.front() == viewpoint_entries_[reference_viewpoint_index].viewpoint.pose());
//      BH_ASSERT(motion.se3Motions().back().poses.back() == viewpoint_entries_[new_viewpoint_index].viewpoint.pose());
//      addViewpointMotion(std::move(motion));
//    }

    num_of_failed_viewpoint_entry_samples_ = 0;
    return true;
  }

  // TODO: Should be a exception for raycast
  catch (const OccupiedTreeType::Error& err) {
    std::cout << "Raycast failed: " << err.what() << std::endl;
    return false;
  }
}

bool ViewpointPlanner::tryToAddViewpointEntry(const Pose& pose, const bool no_raycast) {
  const bool verbose = true;
  const bool valid = isValidObjectPosition(pose.getWorldPosition(), drone_bbox_);
  if (!valid) {
    return false;
  }
  FloatType exploration_step = computeExplorationStep(pose.getWorldPosition());
  // Check distance to other viewpoints and discard if too close
  const std::size_t dist_knn = options_.viewpoint_discard_dist_knn;
//  const FloatType dist_thres_square = options_.viewpoint_discard_dist_thres_square;
//  const std::size_t dist_count_thres = options_.viewpoint_discard_dist_count_thres;
//  const FloatType dist_real_thres_square = options_.viewpoint_discard_dist_real_thres_square;
  static std::vector<ViewpointANN::IndexType> knn_indices;
  static std::vector<ViewpointANN::DistanceType> knn_distances;
  knn_indices.resize(dist_knn);
  knn_distances.resize(dist_knn);
  viewpoint_ann_.knnSearch(pose.getWorldPosition(), dist_knn, &knn_indices, &knn_distances);
  //  for (ViewpointANN::IndexType viewpoint_index : knn_indices) {
  //    const ViewpointEntry& other_viewpoint = viewpoint_entries_[viewpoint_index];
  //    FloatType dist_square = (pose.getWorldPosition() - other_viewpoint.viewpoint.pose().getWorldPosition()).squaredNorm();
  bool discard = false;
  for (std::size_t i = 0; i < knn_distances.size(); ++i) {
    const ViewpointANN::DistanceType dist_square = knn_distances[i];
    const ViewpointEntryIndex viewpoint_index = knn_indices[i];
    const ViewpointEntry& viewpoint_entry = viewpoint_entries_[viewpoint_index];
    exploration_step = std::min(exploration_step, computeExplorationStep(viewpoint_entry.viewpoint.pose().getWorldPosition()));
    const FloatType exploration_step_square_threshold
            = FloatType(0.6) * exploration_step * exploration_step;
    const FloatType angular_dist = pose.getAngularDistanceTo(viewpoint_entries_[viewpoint_index].viewpoint.pose());
    const FloatType exploration_angular_dist_threshold
            = options_.viewpoint_exploration_angular_dist_threshold_degrees * FloatType(M_PI) / FloatType(180.0);
    if (dist_square < exploration_step_square_threshold && angular_dist < exploration_angular_dist_threshold) {
      if (verbose) {
        std::cout << "dist_square=" << dist_square << ", angular_dist=" << angular_dist << std::endl;
      }
      discard = true;
      break;
    }
  }
  if (discard) {
    return false;
  }

  const Viewpoint viewpoint = getVirtualViewpoint(pose);

  // Compute observed voxels and information and discard if too few voxels or too little information.
  // Also discard if too close to too many voxels.
  try {
    if (!no_raycast && !options_.viewpoint_no_raycast) {
      const bool ignore_voxels_with_zero_information = true;
      std::pair<VoxelWithInformationSet, FloatType> raycast_result =
              getRaycastHitVoxelsWithInformationScore(viewpoint, ignore_voxels_with_zero_information);
      VoxelWithInformationSet &voxel_set = raycast_result.first;
      FloatType total_information = raycast_result.second;
      //    if (verbose) {
      //      std::cout << "voxel_set.size()=" << voxel_set.size() << std::endl;
      //    }
      if (voxel_set.size() < options_.viewpoint_min_voxel_count) {
        if (verbose) {
          std::cout << "voxel_set.size() < options_.viewpoint_min_voxel_count" << std::endl;
        }
        return false;
      }
      //  if (verbose) {
      //    std::cout << "total_information=" << total_information << std::endl;
      //  }
      if (total_information < options_.viewpoint_min_information) {
        if (verbose) {
          std::cout << "total_information < options_.viewpoint_min_information" << std::endl;
        }
        return false;
      }

      size_t too_close_voxel_count = 0;
      for (const VoxelWithInformation &vi : voxel_set) {
        const FloatType squared_distance = (viewpoint.pose().getWorldPosition() -
                                            vi.voxel->getBoundingBox().getCenter()).squaredNorm();
        if (squared_distance < options_.viewpoint_voxel_distance_threshold) {
          ++too_close_voxel_count;
        }
      }
      const FloatType too_close_voxel_ratio = too_close_voxel_count / voxel_set.size();
      if (too_close_voxel_ratio >= options_.viewpoint_max_too_close_voxel_ratio) {
        if (verbose) {
          std::cout << "too_close_voxel_ratio < options_.viewpoint_max_too_close_voxel_ratio" << std::endl;
        }
      }

      const bool ignore_viewpoint_count_grid = true;
      const ViewpointEntryIndex new_viewpoint_index = addViewpointEntry(
              ViewpointEntry(Viewpoint(&virtual_camera_, pose), total_information, std::move(voxel_set)),
              ignore_viewpoint_count_grid);
      viewpoint_exploration_front_.push_back(new_viewpoint_index);
    }
    else {
      VoxelWithInformationSet voxel_set;
      const FloatType total_information = 0;
      const bool ignore_viewpoint_count_grid = true;
      const ViewpointEntryIndex new_viewpoint_index = addViewpointEntry(
              ViewpointEntry(Viewpoint(&virtual_camera_, pose), total_information, std::move(voxel_set)),
              ignore_viewpoint_count_grid);
      viewpoint_exploration_front_.push_back(new_viewpoint_index);
    }

    return true;
  }

    // TODO: Should be a exception for raycast
  catch (const OccupiedTreeType::Error& err) {
    std::cout << "Raycast failed: " << err.what() << std::endl;
    return false;
  }
  return true;
}

bool ViewpointPlanner::tryToAddViewpointEntries(const Vector3& position, const bool no_raycast) {
  const bool valid = isValidObjectPosition(position, drone_bbox_);
  if (!valid) {
    return false;
  }
  bool success = false;
  for (size_t i = 0; i < options_.viewpoint_exploration_num_orientations; ++i) {
    Pose::Quaternion sampled_orientation = sampleBiasedOrientation(position, data_->roi_bbox_);
    const Pose sampled_pose = Pose::createFromImageToWorldTransformation(position, sampled_orientation);
    const bool local_success = tryToAddViewpointEntry(sampled_pose, no_raycast);
    success = success || local_success;
  }
  return success;
}

ViewpointPlanner::FloatType ViewpointPlanner::computeExplorationStep(const Vector3& position) const {
  FloatType exploration_step = options_.viewpoint_exploration_step;
  if (getExplorationBbox().isOutside(position)) {
    const FloatType distance_to_bbox = getExplorationBbox().distanceTo(position);
    const FloatType dilation_distance = distance_to_bbox - options_.viewpoint_exploration_dilation_start_bbox_distance;
    if (dilation_distance > 0) {
      if (options_.viewpoint_exploration_dilation_squared) {
        exploration_step += options_.viewpoint_exploration_dilation_speed * dilation_distance * dilation_distance;
      }
      else {
        exploration_step += options_.viewpoint_exploration_dilation_speed * dilation_distance;
      }
    }
  }
  if (exploration_step > options_.viewpoint_exploration_step_max) {
    exploration_step = options_.viewpoint_exploration_step_max;
  }
  return exploration_step;
}

bool ViewpointPlanner::generateNextViewpointEntry2() {
  const bool verbose = true;

  // Sample viewpoint and add it to the viewpoint graph

  bool found_sample;
  Pose sampled_pose;

  if (viewpoint_entries_.size() <= num_real_viewpoints_ && viewpoint_exploration_front_.empty()) {
    for (size_t i = 0; i < viewpoint_entries_.size(); ++i) {
      viewpoint_exploration_front_.push_back(i);
    }
  }

  const bool sample_without_reference = random_.sampleBernoulli(options_.viewpoint_sample_without_reference_probability);
  if (sample_without_reference || viewpoint_exploration_front_.empty()) {
    std::cout << "Sampling uniformly in pose sample bounding box" << std::endl;
    std::tie(found_sample, sampled_pose) = samplePose(pose_sample_bbox_, drone_bbox_);
  }
  else {
    std::cout << "Sampling around existing viewpoint" << std::endl;
    std::cout << "Size of exploration front: " << viewpoint_exploration_front_.size() << std::endl;
    const auto exploration_it = random_.sampleDiscrete(viewpoint_exploration_front_.begin(), viewpoint_exploration_front_.end());
    const ViewpointEntryIndex exploration_index = *exploration_it;
    // Swap exploration_it with last element and afterwards remove last element
    using std::swap;
    swap(*exploration_it, viewpoint_exploration_front_.back());
    viewpoint_exploration_front_.pop_back();
//    const size_t front_index = random_.sampleUniformIntExclusive(0, viewpoint_exploration_front_.size());
//    const ViewpointEntryIndex exploration_index = viewpoint_exploration_front_[front_index];
//    viewpoint_exploration_front_.erase(viewpoint_exploration_front_)
//    const ViewpointEntryIndex exploration_index = viewpoint_exploration_front_.front();
//    viewpoint_exploration_front_.pop_front();
    const Vector3 exploration_position = viewpoint_entries_[exploration_index].viewpoint.pose().getWorldPosition();
    const FloatType exploration_step = computeExplorationStep(exploration_position);
    const bool success1 = tryToAddViewpointEntries(exploration_position - exploration_step * Vector3::UnitX());
    const bool success2 = tryToAddViewpointEntries(exploration_position + exploration_step * Vector3::UnitX());
    const bool success3 = tryToAddViewpointEntries(exploration_position - exploration_step * Vector3::UnitY());
    const bool success4 = tryToAddViewpointEntries(exploration_position + exploration_step * Vector3::UnitY());
    const bool success5 = tryToAddViewpointEntries(exploration_position - exploration_step * Vector3::UnitZ());
    const bool success6 = tryToAddViewpointEntries(exploration_position + exploration_step * Vector3::UnitZ());
    const bool success = success1 || success2 || success3 || success4 || success5 || success6;
    return success;
  }
  if (!found_sample) {
//    if (verbose) {
//      std::cout << "Failed to sample pose." << i << std::endl;
//    }
    return false;
  }

  const bool success = tryToAddViewpointEntry(sampled_pose);
  return success;
}
