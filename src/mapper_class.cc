/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * 
 * All rights reserved.
 * 
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Standard includes
#include <mapper/mapper_class.h>

/**
 * \ingroup mobility
 */
namespace mapper {

MapperClass::MapperClass() {

}

MapperClass::~MapperClass() {
    // Join all threads
    h_haz_tf_thread_.join();
    h_perch_tf_thread_.join();
    h_body_tf_thread_.join();
    h_octo_thread_.join();
    h_fade_thread_.join();
    h_collision_check_thread_.join();

    for(uint i = 0; i < h_cameras_tf_thread_.size(); i++) {
      h_cameras_tf_thread_[i].join();
    }

    // destroy mutexes and semaphores
    mutexes_.destroy();
    semaphores_.destroy();
}

void MapperClass::Initialize(ros::NodeHandle *nh) {

    // Load parameters
    double map_resolution, memory_time, max_range, min_range;
    double inflate_radius_xy, inflate_radius_z;
    double cam_fov, aspect_ratio;
    double occupancy_threshold, probability_hit, probability_miss;
    double clamping_threshold_max, clamping_threshold_min;
    double traj_resolution, compression_max_dev;
    bool process_pcl_at_startup;
    nh->getParam("map_resolution", map_resolution);
    nh->getParam("max_range", max_range);
    nh->getParam("min_range", min_range);
    nh->getParam("memory_time", memory_time);
    nh->getParam("inflate_radius_xy", inflate_radius_xy);
    nh->getParam("inflate_radius_z", inflate_radius_z);
    nh->getParam("cam_fov", cam_fov);
    nh->getParam("cam_aspect_ratio", aspect_ratio);
    nh->getParam("occupancy_threshold", occupancy_threshold);
    nh->getParam("probability_hit", probability_hit);
    nh->getParam("probability_miss", probability_miss);
    nh->getParam("clamping_threshold_min", clamping_threshold_min);
    nh->getParam("clamping_threshold_max", clamping_threshold_max);
    nh->getParam("traj_compression_max_dev", compression_max_dev);
    nh->getParam("traj_compression_resolution", traj_resolution);
    nh->getParam("tf_update_rate", tf_update_rate_);
    nh->getParam("fading_memory_update_rate", fading_memory_update_rate_);
    // nh->getParam("use_haz_cam", use_haz_cam);
    // nh->getParam("use_perch_cam", use_perch_cam);

    // Get namespace of current node
    nh->getParam("namespace", ns_);

    // Load depth camera names
    std::vector<std::string> depth_cam_names;
    std::string depth_cam_prefix, depth_cam_suffix;
    nh->getParam("depth_cam_names", depth_cam_names);
    nh->getParam("depth_cam_prefix", depth_cam_prefix);
    nh->getParam("depth_cam_suffix", depth_cam_suffix);

    // Load frame ids
    std::string inertial_frame_id;
    std::vector<std::string> cam_frame_id;
    nh->getParam("inertial_frame_id", inertial_frame_id);
    nh->getParam("cam_frame_id", cam_frame_id);

    // Check if number of cameras added match the number of frame_id for each of them
    if (depth_cam_names.size() != cam_frame_id.size()) {
        ROS_ERROR("Number of cameras is different from camera tf frame_id! %d %d",depth_cam_names.size(), cam_frame_id.size());
    }

    // Load service names
    std::string resolution_srv_name, memory_time_srv_name;
    std::string map_inflation_srv_name, reset_map_srv_name, rrg_srv_name;
    std::string save_map_srv_name, load_map_srv_name, process_pcl_srv_name;
    nh->getParam("update_resolution", resolution_srv_name);
    nh->getParam("update_memory_time", memory_time_srv_name);
    nh->getParam("update_inflation_radius", map_inflation_srv_name);
    nh->getParam("reset_map", reset_map_srv_name);
    nh->getParam("save_map", save_map_srv_name);
    nh->getParam("load_map", load_map_srv_name);
    nh->getParam("process_pcl", process_pcl_srv_name);
    nh->getParam("rrg_service", rrg_srv_name);

    // Load publisher names
    std::string obstacle_markers_topic, free_space_markers_topic;
    std::string inflated_obstacle_markers_topic, inflated_free_space_markers_topic;
    std::string frustum_markers_topic, discrrg_srv_rete_trajectory_markers_topic;
    std::string collision_detection_topic, graph_tree_marker_topic;
    nh->getParam("obstacle_markers", obstacle_markers_topic);
    nh->getParam("free_space_markers", free_space_markers_topic);
    nh->getParam("inflated_obstacle_markers", inflated_obstacle_markers_topic);
    nh->getParam("inflated_free_space_markers", inflated_free_space_markers_topic);
    nh->getParam("frustum_markers", frustum_markers_topic);
    nh->getParam("discrete_trajectory_markers", discrete_trajectory_markers_topic);
    nh->getParam("collision_detection", collision_detection_topic);
    nh->getParam("graph_tree_marker_topic", graph_tree_marker_topic);

    // Load current package path
    nh->getParam("local_path", local_path_);

    // Parameter defining whether or not the map processes point clouds at startup
    nh->getParam("process_pcl_at_startup", process_pcl_at_startup);

    // Set mapper to update on startup
    globals_.update_map = process_pcl_at_startup;

    // update tree parameters
    globals_.octomap.SetResolution(map_resolution);
    globals_.octomap.SetMaxRange(max_range);
    globals_.octomap.SetMinRange(min_range);
    globals_.octomap.SetInertialFrame(inertial_frame_id);
    globals_.octomap.SetMemory(memory_time);
    globals_.octomap.SetMapInflation(inflate_radius_xy, inflate_radius_z);
    globals_.octomap.SetCamFrustum(cam_fov, aspect_ratio);
    globals_.octomap.SetOccupancyThreshold(occupancy_threshold);
    globals_.octomap.SetHitMissProbabilities(probability_hit, probability_miss);
    globals_.octomap.SetClampingThresholds(clamping_threshold_min, clamping_threshold_max);

    // update trajectory discretization parameters (used in collision check)
    globals_.sampled_traj.SetMaxDev(compression_max_dev);
    globals_.sampled_traj.SetResolution(traj_resolution);

    // Set tf vector to have as many entries as the number of cameras
    globals_.tf_cameras2world.resize(depth_cam_names.size());
    h_cameras_tf_thread_.resize(depth_cam_names.size());
    cameras_sub_.resize(depth_cam_names.size());

    // threads --------------------------------------------------
    h_octo_thread_ = std::thread(&MapperClass::OctomappingTask, this);
    h_fade_thread_ = std::thread(&MapperClass::FadeTask, this);
    h_collision_check_thread_ = std::thread(&MapperClass::CollisionCheckTask, this);
    // h_keyboard_thread_ = std::thread(&MapperClass::KeyboardTask, this);

    // Camera subscribers and tf threads ----------------------------------------------
    for (uint i = 0; i < depth_cam_names.size(); i++) {
        std::string cam_topic = depth_cam_prefix + depth_cam_names[i] + depth_cam_suffix;
        cameras_sub_[i] = nh->subscribe<sensor_msgs::PointCloud2>
              (cam_topic, 10, boost::bind(&MapperClass::PclCallback, this, _1, i));
        h_cameras_tf_thread_[i] = std::thread(&MapperClass::TfTask, this, inertial_frame_id, cam_frame_id[i], i);
        ROS_INFO("[mapper] Subscribed to camera topic: %s", cameras_sub_[i].getTopic().c_str());
    }

    // Create services ------------------------------------------
    resolution_srv_ = nh->advertiseService(
        resolution_srv_name, &MapperClass::UpdateResolution, this);
    memory_time_srv_ = nh->advertiseService(
        memory_time_srv_name, &MapperClass::UpdateMemoryTime, this);
    map_inflation_srv_ = nh->advertiseService(
        map_inflation_srv_name, &MapperClass::MapInflation, this);
    reset_map_srv_ = nh->advertiseService(
        reset_map_srv_name, &MapperClass::ResetMap, this);
    save_map_srv_ = nh->advertiseService(
        save_map_srv_name, &MapperClass::SaveMap, this);
    load_map_srv_ = nh->advertiseService(rrg_srv_rrg_srv_
        load_map_srv_name, &MapperClass::LoadMap, this);
    process_pcl_srv_ = nh->advertiseService(
        process_pcl_srv_name, &MapperClass::OctomapProcessPCL, this);
    rrg_srv_ = nh->advertiseService(
        rrg_srv_name, &MapperClass::RRGService, this);
    collision_check_srv = nh->advertiseService(
        collision_check_srv_name, &MapperClass::CollisionCheckService, this);
    // if (use_haz_cam) {
    //     std::string cam = TOPIC_HARDWARE_NAME_HAZ_CAM;
    //     haz_sub_ = nh->subscribe(cam_prefix + cam + cam_suffix, 10, &MapperClass::PclCallback, this);
    // }
    // if (use_perch_cam) {
    //     std::string cam = TOPIC_HARDWARE_NAME_PERCH_CAM;
    //     perch_sub_ = nh->subscribe(cam_prefix + cam + cam_suffix, 10, &MapperClass::PclCallback, this);
    // }
    // segment_sub_ = nh->subscribe(TOPIC_GNC_CTL_SEGMENT, 10, &MapperClass::SegmentCallback, this);

    // Publishers -----------------------------------------------
    sentinel_pub_ =
        nh->advertise<geometry_msgs::PointStamped>(collision_detection_topic, 10);
    obstacle_marker_pub_ =
        nh->advertise<visualization_msgs::MarkerArray>(obstacle_markers_topic, 10);
    free_space_marker_pub_ =
        nh->advertise<visualization_msgs::MarkerArray>(free_space_markers_topic, 10);
    inflated_obstacle_marker_pub_ =
        nh->advertise<visualization_msgs::MarkerArray>(inflated_obstacle_markers_topic, 10);
    inflated_free_space_marker_pub_ =
        nh->advertise<visualization_msgs::MarkerArray>(inflated_free_space_markers_topic, 10);
    path_marker_pub_ =
        nh->advertise<visualization_msgs::MarkerArray>(discrete_trajectory_markers_topic, 10);
    cam_frustum_pub_ =
        nh->advertise<visualization_msgs::Marker>(frustum_markers_topic, 10);
    graph_tree_marker_pub_ = 
        nh->advertise<visualization_msgs::Marker>(graph_tree_marker_topic, 10);

    // Notify initialization complete
    ROS_INFO("Initialization complete");
}


// PLUGINLIB_EXPORT_CLASS(mapper::MapperClass, nodelet::Nodelet);

}  // namespace mapper
