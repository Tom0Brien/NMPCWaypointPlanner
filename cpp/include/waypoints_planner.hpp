#ifndef WAYPOINT_PLANNER_HPP
#define WAYPOINT_PLANNER_HPP

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlopt.hpp>
#include <pcl/conversions.h>  // For converting polygon meshes to point clouds
#include <pcl/filters/crop_box.h>
#include <pcl/filters/frustum_culling.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/vtk_lib_io.h>  // For loading STL files
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <random>  // Added for MPPI noise sampling
#include <vector>

/**
 * @brief Builds a 4x4 homogeneous transform from a position and rpy Euler
 * angles.
 *
 * @tparam Scalar The serror angles (roll, pitch, yaw) in order.
 * @return An IsometryT representing the homogeneous transform.
 */
template <typename Scalar>
Eigen::Transform<Scalar, 3, Eigen::Isometry> stateToIsometry(const Eigen::Matrix<Scalar, 3, 1>& translation,
                                                             const Eigen::Matrix<Scalar, 3, 1>& eulZYX) {
    Eigen::Transform<Scalar, 3, Eigen::Isometry> T = Eigen::Transform<Scalar, 3, Eigen::Isometry>::Identity();
    T.linear() = Eigen::AngleAxis<Scalar>(eulZYX.z(), Eigen::Matrix<Scalar, 3, 1>::UnitZ())
                 * Eigen::AngleAxis<Scalar>(eulZYX.y(), Eigen::Matrix<Scalar, 3, 1>::UnitY())
                 * Eigen::AngleAxis<Scalar>(eulZYX.x(), Eigen::Matrix<Scalar, 3, 1>::UnitX()).toRotationMatrix();
    T.translation() = translation;
    return T;
}

/**
 * @brief Computes the 6D homogeneous error between two transforms.
 *
 * The error vector consists of 6 elements where the first 3 represent the
 * translational error and the last 3 represent the rotational error.
 *
 * @tparam Scalar The scalar type.
 * @param H1 The first homogeneous transform.
 * @param H2 The second homogeneous transform.
 * @return A 6x1 Eigen vector representing the error.
 */
template <typename Scalar>
Eigen::Matrix<Scalar, 6, 1> homogeneousError(const Eigen::Transform<Scalar, 3, Eigen::Isometry>& H1,
                                             const Eigen::Transform<Scalar, 3, Eigen::Isometry>& H2) {
    Eigen::Matrix<Scalar, 6, 1> e = Eigen::Matrix<Scalar, 6, 1>::Zero();

    // Translational error
    e.head(3) = H1.translation() - H2.translation();

    // Orientation error
    Eigen::Matrix<Scalar, 3, 3> Re = H1.rotation() * H2.rotation().transpose();
    Scalar t                       = Re.trace();
    Eigen::Matrix<Scalar, 3, 1> eps(Re(2, 1) - Re(1, 2), Re(0, 2) - Re(2, 0), Re(1, 0) - Re(0, 1));
    Scalar eps_norm = eps.norm();
    if (t > -0.99 || eps_norm > 1e-10) {
        if (eps_norm < 1e-3)
            e.tail(3) = (Scalar(0.75) - t / Scalar(12)) * eps;
        else
            e.tail(3) = (std::atan2(eps_norm, t - Scalar(1)) / eps_norm) * eps;
    }
    else {
        e.tail(3) = M_PI_2 * (Re.diagonal().array() + Scalar(1));
    }
    return e;
}

template <typename T,
          typename Scalar                                                                 = typename T::Scalar,
          std::enable_if_t<((T::RowsAtCompileTime == 3) && (T::ColsAtCompileTime == 3))>* = nullptr>
inline Eigen::Matrix<Scalar, 3, 1> mat_to_rpy_intrinsic(const T& mat) {
    // Eigen euler angles and with better range
    return Eigen::Matrix<Scalar, 3, 1>(
        // Roll
        std::atan2(mat(2, 1), mat(2, 2)),
        // Pitch
        std::atan2(-mat(2, 0), std::sqrt(mat(2, 1) * mat(2, 1) + mat(2, 2) * mat(2, 2))),
        // Yaw
        std::atan2(mat(1, 0), mat(0, 0)));
}


/**
 * @brief Helper function to compute the number of points visible from a given
 * pose under the new coordinate mapping (X forward, Y left, Z up).
 *
 * @param cloud      The (downsampled) obstacle cloud.
 * @param fov_degs   Field of view in degrees (horizontal & vertical).
 * @param near_plane Near plane distance for frustum culling.
 * @param far_plane  Far plane distance for frustum culling.
 * @param pose       The pose in world coordinates (where we consider X forward,
 * Y left, Z up).
 * @return The number of points in the cloud that lie within the camera frustum.
 */
template <typename Scalar>
pcl::PointCloud<pcl::PointXYZ>::Ptr getFrustrumCloud(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
                                                     Scalar fov_degs,
                                                     Scalar near_plane,
                                                     Scalar far_plane,
                                                     const Eigen::Transform<Scalar, 3, Eigen::Isometry>& pose) {
    if (!cloud || cloud->points.empty()) {
        return pcl::PointCloud<pcl::PointXYZ>::Ptr();
    }

    // Create a FrustumCulling filter
    pcl::FrustumCulling<pcl::PointXYZ> fc;
    fc.setInputCloud(cloud);
    fc.setHorizontalFOV(static_cast<float>(fov_degs));
    fc.setVerticalFOV(static_cast<float>(fov_degs));
    fc.setNearPlaneDistance(static_cast<float>(near_plane));
    fc.setFarPlaneDistance(static_cast<float>(far_plane));

    // Rotate camera so that PCL "camera" aligns with X forward, Y left, Z up.
    Eigen::Matrix4f camera_pose = pose.matrix().template cast<float>();
    Eigen::Matrix4f cam2robot;
    // cam2robot << 1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1;
    cam2robot << 0, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1;  // X right, Y down, Z forward
    camera_pose = camera_pose * cam2robot;

    // Apply the transform in PCL
    fc.setCameraPose(camera_pose);

    // Filter the points that lie within this frustum
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_out(new pcl::PointCloud<pcl::PointXYZ>());
    fc.filter(*cloud_out);

    return cloud_out;
}

/**
 * @brief Returns how many obstacle points lie within a user-defined box in
 *        the camera/end-effector coordinate frame.
 *
 * @param cloud        The obstacle cloud.
 * @param pose         The camera/EE pose in world coordinates.
 * @param box_min      The minimum corner of the box in camera coords.
 * @param box_max      The maximum corner of the box in camera coords.
 * @return             Number of points inside that box.
 */
template <typename Scalar>
std::size_t getPointsInBox(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
                           const Eigen::Transform<Scalar, 3, Eigen::Isometry>& pose,
                           const Eigen::Vector4f& box_min,
                           const Eigen::Vector4f& box_max) {
    if (!cloud || cloud->empty()) {
        return 0;
    }

    // Create a CropBox filter
    pcl::CropBox<pcl::PointXYZ> crop_filter;
    crop_filter.setInputCloud(cloud);

    // We want the box to be defined in the "camera frame."
    // By providing the *inverse* of the camera's world pose,
    // PCL will transform each point into the camera frame before checking.
    crop_filter.setTransform(pose.inverse().template cast<float>());

    // Define the bounding box extents (in camera coords).
    crop_filter.setMin(box_min);
    crop_filter.setMax(box_max);

    // Filter the cloud
    pcl::PointCloud<pcl::PointXYZ>::Ptr in_box(new pcl::PointCloud<pcl::PointXYZ>());
    crop_filter.filter(*in_box);

    return in_box->size();
}

/**
 * @brief PlannerMpc class implementing an NLMPC-style trajectory planner.
 *
 * This class is templated on the state dimension (StateDim), action dimension
 * (ActionDim), horizon (HorizonDim), and the scalar type (Scalar). For the
 * simple integrator used here, it is assumed that StateDim == ActionDim, and
 * that the first 3 entries of the state vector represent the position while the
 * last 3 represent the Euler angles.
 *
 * @tparam StateDim The dimension of the state vector.
 * @tparam ActionDim The dimension of the action (control) vector.
 * @tparam HorizonDim The planning horizon.
 * @tparam Scalar The scalar type (default is double).
 */
template <int StateDim, int ActionDim, int HorizonDim, typename Scalar = double>
class PlannerMpc {
public:
    /// Type alias for the isometry using the specified scalar type.
    using IsometryT = Eigen::Transform<Scalar, 3, Eigen::Isometry>;

    /// Initial pose.
    IsometryT H_0 = IsometryT::Identity();
    /// Goal pose.
    IsometryT H_goal = IsometryT::Identity();

    /// Positional tracking cost weight.
    Scalar w_p = Scalar(100.0);
    /// Orientation tracking cost weight.
    Scalar w_q = Scalar(10.0);
    /// Terminal positional cost weight.
    Scalar w_p_term = Scalar(1e3);
    /// Terminal orientation cost weight.
    Scalar w_q_term = Scalar(1e3);

    /// Look at goal cost weight.
    Scalar w_look_at_goal = Scalar(10.0);

    /// Tuning parameter to control the saturation rate.
    Scalar alpha_visibility = Scalar(0.2);
    /// Field of view (in degrees) for visibility checking.
    Scalar visibility_fov = Scalar(60.0);
    /// Min plane distances for the visibility frustum.
    Scalar visibility_min_range = Scalar(0.0);
    /// Max plane distance for the visibility frustum.
    Scalar visibility_max_range = Scalar(0.5);

    /// Percentage of points that must be visible.
    double min_visible_ratio = 0.5;

    /// Minimum number of visible points required.
    int min_visible_points = 0.0;

    /// Point in world to look at while moving.
    Eigen::Vector3d look_at_goal = Eigen::Vector3d::Zero();

    /// Look at goal distance from camera.
    Scalar look_at_goal_distance = Scalar(0.11);

    /// Obstacle avoidance cost weight.
    Scalar w_obs = Scalar(5.0);

    /// Obstacle cloud for avoidance
    pcl::PointCloud<pcl::PointXYZ>::ConstPtr obstacle_cloud;

    /// End-effector mesh cloud for collision checking.
    pcl::PointCloud<pcl::PointXYZ>::Ptr ee_mesh_cloud =
        pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());

    /// KD-tree for obstacle queries.
    std::shared_ptr<pcl::KdTreeFLANN<pcl::PointXYZ>> kd_tree;

    /// Safety margin for collision avoidance.
    Scalar collision_margin = Scalar(0.05);

    // Box dimensions for collision checking (min and max in camera/end-effector frame).
    Eigen::Vector4f box_min = Eigen::Vector4f(-0.08, -0.08, -0.08, 1);
    Eigen::Vector4f box_max = Eigen::Vector4f(0.08, 0.08, 0.08, 1);

    /// Control bounds for position.
    Scalar dp_min = Scalar(-0.1);
    Scalar dp_max = Scalar(0.1);
    /// Control bounds for orientation.
    Scalar dtheta_min = Scalar(-0.1);
    Scalar dtheta_max = Scalar(0.1);

    int num_samples      = 2048;          // Number of candidate trajectories to sample.
    Scalar mppi_lambda   = Scalar(1.0);   // Temperature parameter.
    Scalar noise_std_pos = Scalar(0.01);  // Standard deviation for position noise.
    Scalar noise_std_ori = Scalar(0.05);  // Standard deviation for orientation noise.
    // ********************************

    /// Maintained control sequence for warm-starting (size: ActionDim * HorizonDim).
    std::vector<Scalar> U;

    /// Convergence criteria for waypoint generation: position tolerance (1 cm).
    double position_tolerance = 1e-2;
    /// Convergence criteria for waypoint generation: orientation tolerance (~0.057 deg).
    double orientation_tolerance = 1e-2;
    /// Maximum number of iterations for waypoint generation.
    int max_iterations = 20;

    double fusion_position_tolerance    = 1e-2;
    double fusion_orientation_tolerance = 0.1;

    pcl::PointCloud<pcl::PointXYZ>::Ptr collision_debug_cloud =
        pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());

    /**
     * @brief Default constructor.
     */
    PlannerMpc() = default;

    /**
     * @brief Default destructor.
     */
    ~PlannerMpc() = default;

    /**
     * @brief Sets a new warm-start control sequence.
     *
     * @param U_init The initial control sequence.
     */
    void setAction(const std::vector<Scalar>& U_init) {
        if (static_cast<int>(U_init.size()) == ActionDim * HorizonDim) {
            U = U_init;
        }
        else {
            std::cerr << "[PlannerMpc::setAction] Mismatched size! Expected " << (ActionDim * HorizonDim) << ".\n";
        }
    }

    /**
     * @brief Rollouts the trajectory using the given control sequence.
     *
     * Converts a std::vector<Scalar> control sequence (length = ActionDim * HorizonDim)
     * into a dynamic Eigen representation and integrates the trajectory starting
     * from H_0. Each state is represented as an Eigen::Matrix<Scalar, StateDim, 1>.
     *
     * @param U_in The control sequence.
     * @return A vector of state vectors representing the trajectory.
     */
    std::vector<Eigen::Matrix<Scalar, StateDim, 1>> rollout(const std::vector<Scalar>& U_in) {
        std::vector<Eigen::Matrix<Scalar, StateDim, 1>> trajectory(HorizonDim + 1);

        // Convert U_in into an Eigen matrix (ActionDim x HorizonDim)
        Eigen::Matrix<Scalar, ActionDim, HorizonDim> U_mat;
        for (int k = 0; k < HorizonDim; ++k) {
            for (int i = 0; i < ActionDim; ++i) {
                U_mat(i, k) = U_in[ActionDim * k + i];
            }
        }
        // Simple integrator: next state = current state + control.
        trajectory[0] << H_0.translation(), mat_to_rpy_intrinsic(H_0.rotation());
        for (int k = 0; k < HorizonDim; ++k) {
            trajectory[k + 1] = trajectory[k] + U_mat.col(k);
        }
        return trajectory;
    }

    /**
     * @brief Computes the obstacle cost based on the distance from a query pose.
     *
     * The translation component of the pose is used to query the obstacle kd-tree.
     *
     * @param pose The pose (as an isometry) at which to evaluate the obstacle cost.
     * @return The computed obstacle cost.
     */
    Scalar obstacleCost(const IsometryT& pose) {
        if (kd_tree && kd_tree->getInputCloud() && !kd_tree->getInputCloud()->points.empty()) {
            pcl::PointXYZ query_pt;
            Eigen::Matrix<Scalar, 3, 1> p = pose.translation();
            query_pt.x                    = static_cast<float>(p(0));
            query_pt.y                    = static_cast<float>(p(1));
            query_pt.z                    = static_cast<float>(p(2));
            std::vector<int> nn_index(1);
            std::vector<float> nn_dist2(1);
            int found = kd_tree->nearestKSearch(query_pt, 1, nn_index, nn_dist2);
            if (found > 0) {
                Scalar nearest_dist = std::sqrt(nn_dist2[0]);
                if (nearest_dist < collision_margin) {
                    Scalar diff = (Scalar(1.0) / nearest_dist) - (Scalar(1.0) / collision_margin);
                    return Scalar(0.5) * w_obs * diff * diff;
                }
            }
        }
        return Scalar(0.0);
    }

    /**
     * @brief Computes a detailed collision cost using the stored end-effector mesh.
     *
     * Each point of the mesh is transformed into the world frame using the given pose.
     *
     * @param pose The current end-effector pose in world coordinates.
     * @return A scalar cost representing the collision penalty.
     */
    Scalar meshCollisionCost(const IsometryT& pose) {
        Scalar total_cost = 0;
        pcl::PointXYZ query_pt;

        // Ensure we have a valid mesh cloud.
        if (!ee_mesh_cloud || ee_mesh_cloud->empty())
            return total_cost;

        // For each point on the end-effector mesh...
        for (const auto& pt : ee_mesh_cloud->points) {
            // Transform the point into the world frame.
            Eigen::Vector3f pt_vec(pt.x, pt.y, pt.z);
            Eigen::Vector3f transformed_pt =
                pose.rotation().template cast<float>() * pt_vec + pose.translation().template cast<float>();

            query_pt.x = transformed_pt(0);
            query_pt.y = transformed_pt(1);
            query_pt.z = transformed_pt(2);

            // Query the obstacle kd-tree for the nearest neighbor.
            std::vector<int> nn_index(1);
            std::vector<float> nn_dist2(1);
            int found = kd_tree->nearestKSearch(query_pt, 1, nn_index, nn_dist2);
            if (found > 0) {
                Scalar nearest_dist = std::sqrt(nn_dist2[0]);
                if (nearest_dist < collision_margin) {
                    Scalar cost = (Scalar(1) / (2 * collision_margin)) * (nearest_dist - collision_margin)
                                  * (nearest_dist - collision_margin);
                    total_cost += w_obs * cost;
                }
            }
        }
        return total_cost;
    }

    /**
     * @brief Returns a collision cost if any obstacle points lie in a
     *        bounding box around the camera/end-effector.
     *
     * @param pose Current camera/end-effector pose in the world frame.
     * @return Collision cost (0 if empty, >0 if some points found).
     */
    Scalar boxCollisionCost(const IsometryT& pose) {
        // Count the number of points inside the box.
        std::size_t count = getPointsInBox<Scalar>(obstacle_cloud, pose, box_min, box_max);
        Scalar n_points   = static_cast<Scalar>(count);

        // Simple linear penalty based on the number of points.
        Scalar cost = w_obs * n_points;
        return cost;
    }

    Scalar poseCost(const IsometryT& pose, Scalar wp, Scalar wq) {
        // 1) Pose cost
        auto e           = homogeneousError(pose, H_goal);
        Scalar cost_pose = wp * e.head(3).squaredNorm() + wq * e.tail(3).squaredNorm();

        // 2) Look at goal cost: angle between the camera's +Z axis and (look_at_goal - cameraPos)

        Eigen::Vector3d look_at_goal =
            H_goal.translation() + H_goal.rotation() * Eigen::Vector3d(0, 0, look_at_goal_distance);
        Scalar cost_rot                      = Scalar(0);
        Eigen::Matrix<Scalar, 3, 1> camera_z = pose.linear().col(2);
        // Typically this is already unit-length if pose is orthonormal.

        // Vector pointing from camera to look_at_goal
        Eigen::Matrix<Scalar, 3, 1> dir = look_at_goal - pose.translation();
        Scalar dist                     = dir.norm();
        if (dist > Scalar(1e-8)) {
            dir /= dist;  // normalize
            // Dot product (clamp to [-1,1] for acos)
            Scalar c = camera_z.dot(dir);
            if (c > Scalar(1))
                c = Scalar(1);
            if (c < Scalar(-1))
                c = Scalar(-1);

            Scalar angle = std::acos(c);
            cost_rot     = w_look_at_goal * angle * angle;
        }

        return cost_pose + cost_rot;
    }

    Scalar visibilityCost(const IsometryT& pose) {
        // If no visibility cloud is provided, or it's empty, no visibility constraint can be enforced.
        if (!obstacle_cloud || obstacle_cloud->points.empty()) {
            return Scalar(0.0);
        }

        // Count how many points are in the camera frustum.
        std::size_t visible =
            getFrustrumCloud(obstacle_cloud, visibility_fov, visibility_min_range, visibility_max_range, pose)->size();

        // Convert to Scalar for safety in math:
        Scalar v = static_cast<Scalar>(visible);

        // Soft constraint: if v is below min_visible_points, apply an exponential penalty.
        // The tuning parameter 'alpha' determines how steep the cost grows.
        Scalar delta = min_visible_points - v;
        if (delta > 0) {
            return std::exp(alpha_visibility * delta) - 1.0;
        }
        else {
            return 0.0;
        }
    }

    /**
     * @brief Computes the total cost along the trajectory induced by the control sequence.
     *
     * This function assumes that the trajectory is represented as a vector of state vectors,
     * where each state encodes position and orientation (in Euler angles). Each state is first
     * converted to an isometry using stateToIsometry before being passed to the cost functions.
     *
     * @param x    The control sequence.
     * @param grad The gradient of the cost (if required).
     * @return The total cost.
     */
    Scalar cost(const std::vector<Scalar>& x, std::vector<Scalar>& grad) {
        auto traj         = rollout(x);
        Scalar total_cost = 0;
        for (int k = 0; k <= HorizonDim; ++k) {
            // Convert the k-th state (position and Euler angles) into an isometry.
            IsometryT pose         = stateToIsometry<Scalar>(traj[k].template head<3>(), traj[k].template tail<3>());
            double mesh_cost       = meshCollisionCost(pose);
            double pose_cost       = poseCost(pose, w_p, w_q);
            double visibility_cost = visibilityCost(pose);
            total_cost += pose_cost + mesh_cost + visibility_cost;
        }
        // Terminal cost
        IsometryT pose_N =
            stateToIsometry<Scalar>(traj[HorizonDim].template head<3>(), traj[HorizonDim].template tail<3>());
        total_cost += poseCost(pose_N, w_p_term, w_q_term);
        return total_cost;
    }

    /**
     * @brief Static cost wrapper for NLopt callback.
     *
     * @param x The control sequence.
     * @param grad The gradient of the cost.
     * @param data Pointer to the PlannerMpc instance.
     * @return The cost computed by the PlannerMpc instance.
     */
    static Scalar costWrapper(const std::vector<Scalar>& x, std::vector<Scalar>& grad, void* data) {
        PlannerMpc* planner_ptr = reinterpret_cast<PlannerMpc*>(data);
        return planner_ptr->cost(x, grad);
    }

    /**
     * @brief Solves the MPC problem using NLopt and returns the optimized control
     * sequence.
     *
     * @param H0_in The initial pose.
     * @return The optimized control sequence.
     */
    std::vector<Scalar> getAction(const IsometryT& H0_in) {
        H_0 = H0_in;
        if (HorizonDim <= 0) {
            std::cerr << "[PlannerMpc::getAction] HorizonDim <= 0.\n";
            return {};
        }
        if (static_cast<int>(U.size()) != ActionDim * HorizonDim)
            U.assign(ActionDim * HorizonDim, Scalar(0));

        int dim = ActionDim * HorizonDim;
        nlopt::opt opt(nlopt::LN_COBYLA, dim);
        opt.set_min_objective(costWrapper, this);

        // Bounds
        std::vector<Scalar> lb(dim), ub(dim);
        for (int k = 0; k < HorizonDim; ++k) {
            // position deltas
            for (int i = 0; i < 3; ++i) {
                lb[ActionDim * k + i] = dp_min;
                ub[ActionDim * k + i] = dp_max;
            }
            // orientation deltas
            for (int i = 3; i < 6; ++i) {
                lb[ActionDim * k + i] = dtheta_min;
                ub[ActionDim * k + i] = dtheta_max;
            }
        }
        opt.set_lower_bounds(lb);
        opt.set_upper_bounds(ub);
        opt.set_xtol_rel(1e-6);
        opt.set_maxeval(200);

        std::vector<Scalar> U_opt = U;  // warm start
        Scalar minf               = 0;
        try {
            auto result = opt.optimize(U_opt, minf);
            std::cout << "[PlannerMpc::getAction] Converged. Cost = " << minf << " (nlopt code: " << result << ")\n";
        }
        catch (std::exception& e) {
            std::cerr << "[PlannerMpc::getAction] NLopt failed: " << e.what() << std::endl;
        }

        // Check final pose error
        auto traj  = rollout(U_opt);
        auto p_N   = traj[HorizonDim].template head<3>();
        auto eul_N = traj[HorizonDim].template tail<3>();
        auto H_N   = stateToIsometry<Scalar>(p_N, eul_N);
        auto err   = homogeneousError(H_N, H_goal);
        std::cout << "[PlannerMpc::getAction] Final pos error: " << err.head(3).norm()
                  << ", ori error: " << err.tail(3).norm() << "\n";

        // Recede horizon
        if (HorizonDim > 1) {
            for (int k = 0; k < HorizonDim - 1; ++k)
                for (int i = 0; i < ActionDim; ++i)
                    U[ActionDim * k + i] = U_opt[ActionDim * (k + 1) + i];
            std::fill(U.end() - ActionDim, U.end(), Scalar(0));
        }
        else {
            std::fill(U.begin(), U.end(), Scalar(0));
        }

        return U_opt;
    }

    std::vector<Scalar> getActionMPPI(const IsometryT& H0_in) {
        H_0 = H0_in;
        if (HorizonDim <= 0) {
            std::cerr << "[PlannerMpc::getAction] HorizonDim <= 0.\n";
            return {};
        }
        if (static_cast<int>(U.size()) != ActionDim * HorizonDim)
            U.assign(ActionDim * HorizonDim, Scalar(0));

        int dim = ActionDim * HorizonDim;

        // MPPI parameters (using member variables):
        int N         = num_samples;
        Scalar lambda = mppi_lambda;

        // Set up noise standard deviations per control dimension.
        // (For the simple integrator, assume first 3 entries are position and the next 3 are orientation.)
        std::vector<Scalar> noise_std(ActionDim);
        for (int i = 0; i < 3; ++i)
            noise_std[i] = noise_std_pos;
        for (int i = 3; i < ActionDim; ++i)
            noise_std[i] = noise_std_ori;

        // Prepare containers for candidates and costs.
        std::vector<std::vector<Scalar>> candidates(N, std::vector<Scalar>(dim, 0));
        std::vector<Scalar> candidate_costs(N, 0);

// Parallelize the candidate sampling and evaluation.
#pragma omp parallel for
        for (int i = 0; i < N; ++i) {
            // Each thread creates its own random number generator.
            std::random_device rd;
            std::mt19937 gen(rd() + i);
            // For each candidate, sample a control sequence.
            for (int k = 0; k < HorizonDim; ++k) {
                for (int j = 0; j < ActionDim; ++j) {
                    int idx      = k * ActionDim + j;
                    Scalar noise = 0;
                    // Create local normal distribution based on control type.
                    if (j < 3) {  // Position noise.
                        std::normal_distribution<Scalar> dist(0.0, noise_std_pos);
                        noise = dist(gen) * std::exp(-Scalar(k));
                    }
                    else {  // Orientation noise.
                        std::normal_distribution<Scalar> dist(0.0, noise_std_ori);
                        noise = dist(gen) * std::exp(-Scalar(k));
                    }
                    candidates[i][idx] = U[idx] + noise;
                    // Clip the candidate values to respect control bounds.
                    if (j < 3)  // position bounds
                        candidates[i][idx] = std::max(dp_min, std::min(dp_max, candidates[i][idx]));
                    else  // orientation bounds
                        candidates[i][idx] = std::max(dtheta_min, std::min(dtheta_max, candidates[i][idx]));
                }
            }
            std::vector<Scalar> grad;  // Unused here.
            candidate_costs[i] = cost(candidates[i], grad);
        }

        // Compute weights based on cost.
        Scalar min_cost = *std::min_element(candidate_costs.begin(), candidate_costs.end());
        std::vector<Scalar> weights(N, 0);
        Scalar weight_sum = 0;
        for (int i = 0; i < N; ++i) {
            weights[i] = std::exp(-(candidate_costs[i] - min_cost) / lambda);
            weight_sum += weights[i];
        }
        for (int i = 0; i < N; ++i)
            weights[i] /= weight_sum;

        // Update the control sequence by taking the weighted average.
        std::vector<Scalar> U_opt(dim, 0);
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < dim; ++j) {
                U_opt[j] += weights[i] * candidates[i][j];
            }
        }

        // Recede the horizon: shift the control sequence one step ahead.
        if (HorizonDim > 1) {
            for (int k = 0; k < HorizonDim - 1; ++k)
                for (int j = 0; j < ActionDim; ++j)
                    U[k * ActionDim + j] = U_opt[(k + 1) * ActionDim + j];
            // Initialize the last time step with zeros.
            for (int j = 0; j < ActionDim; ++j)
                U[(HorizonDim - 1) * ActionDim + j] = 0;
        }
        else {
            U = U_opt;
        }
        return U_opt;
    }

    /**
     * @brief Post-process the generated waypoints and fuse consecutive waypoints that are close together.
     *
     * Two poses are considered “close” if both their translational difference (Euclidean norm)
     * and rotational difference (norm of the 3-vector from homogeneousError) are below the provided tolerances.
     * In this simple implementation, if two consecutive poses are within tolerance,
     * the translation is averaged and the rotation of the first pose is retained.
     *
     * @param waypoints The input vector of waypoints.
     * @param pos_tol Positional tolerance for fusion.
     * @param ori_tol Orientation tolerance for fusion.
     * @return A vector of fused waypoints.
     */
    std::vector<IsometryT> fuseWaypoints(const std::vector<IsometryT>& waypoints) {
        std::vector<IsometryT> fused;
        if (waypoints.empty())
            return fused;

        fused.push_back(waypoints.front());
        for (size_t i = 1; i < waypoints.size() - 1; ++i) {
            // Compute the error between the last fused pose and the current one.
            Eigen::Matrix<Scalar, 6, 1> diff = homogeneousError(fused.back(), waypoints[i]);
            Scalar pos_diff                  = diff.head(3).norm();
            Scalar ori_diff                  = diff.tail(3).norm();

            if (pos_diff < fusion_position_tolerance && ori_diff < fusion_orientation_tolerance) {
                // If the poses are close, update the last fused waypoint by averaging the translation.
                // fused.back().translation() = (fused.back().translation() + waypoints[i].translation()) /
                // Scalar(2);
                // For rotation: if the difference is very small, simply keep the existing rotation.
                std::cout << "[PlannerMpc::fuseWaypoints] Fused waypoints at index " << i << std::endl;
            }
            else {
                fused.push_back(waypoints[i]);
            }
        }
        // Always keep the last waypoint.
        fused.push_back(waypoints.back());
        return fused;
    }

    /**
     * @brief Generates waypoints by running the MPC loop from the initial pose to
     * the goal pose, while also computing time statistics and visibility metrics.
     *
     * Runs the MPC loop starting from the initial pose until the convergence
     * criteria or the maximum number of iterations is reached. Returns a vector
     * of waypoints representing the trajectory.
     *
     * @param init The initial pose.
     * @param goal The goal pose.
     * @return A vector of IsometryT waypoints representing the planned trajectory.
     */
    std::vector<IsometryT> generateWaypoints(const IsometryT& init, const IsometryT& goal) {
        // Start timer
        auto start_time = std::chrono::high_resolution_clock::now();

        H_0    = init;
        H_goal = goal;

        min_visible_points = static_cast<int>(min_visible_ratio * obstacle_cloud->points.size());
        std::cout << "[PlannerMpc::generateWaypoints] Minimum visible points: " << min_visible_points << std::endl;

        std::vector<IsometryT> waypoints{H_0};
        int iter = 0;
        for (iter = 0; iter < max_iterations; ++iter) {
            auto U_opt                      = getAction(H_0);
            auto states                     = rollout(U_opt);
            auto next_s                     = states[1];  // receding-horizon step
            Eigen::Matrix<Scalar, 3, 1> p   = next_s.head(3);
            Eigen::Matrix<Scalar, 3, 1> eul = next_s.tail(3);

            IsometryT H_next = stateToIsometry(p, eul);
            auto err         = homogeneousError(H_next, H_goal);
            double pos_err   = err.head(3).norm();
            double ori_err   = err.tail(3).norm();

            std::cout << "[PlannerMpc::generateWaypoints] Iter " << (iter + 1) << " -> pos_err=" << pos_err
                      << ", ori_err=" << ori_err << "\n";

            if ((pos_err < position_tolerance && ori_err < orientation_tolerance) || iter == max_iterations - 1) {
                waypoints.back() = H_goal;  // Snap final
                std::cout << "[PlannerMpc::generateWaypoints] Converged in " << (iter + 1) << " iterations.\n";
                break;
            }
            else {
                H_0 = H_next;
                waypoints.push_back(H_0);
            }
        }

        // End timer
        auto end_time = std::chrono::high_resolution_clock::now();
        auto planning_duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        // Compute visibility metric across all generated waypoints
        double sum_visible = 0.0;
        if (obstacle_cloud && !obstacle_cloud->points.empty()) {
            for (const auto& wp : waypoints) {
                std::size_t visible_count =
                    getFrustrumCloud(obstacle_cloud, visibility_fov, visibility_min_range, visibility_max_range, wp)
                        ->size();
                sum_visible += static_cast<double>(visible_count);
            }
        }
        double avg_visible_per_waypoint =
            (waypoints.empty()) ? 0.0 : (sum_visible / static_cast<double>(waypoints.size()));

        // Print out the results
        std::cout << "[PlannerMpc::generateWaypoints] "
                  << "Planning took " << planning_duration_ms << " ms. "
                  << "Number of waypoints: " << waypoints.size() << std::endl;
        std::cout << "[PlannerMpc::generateWaypoints] "
                  << "Average visible points per waypoint: " << avg_visible_per_waypoint << std::endl;
        // Fuse waypoints that are close together.
        waypoints = fuseWaypoints(waypoints);

        return waypoints;
    }

    /**
     * @brief Updates the end-effector collision box dimensions and detailed mesh from an STL file.
     *
     * This method loads an STL file from the provided filepath, applies the given rigid transform to the
     * model vertices, computes the axis-aligned bounding box (with an optional margin), downsamples the
     * point cloud, and stores the resulting mesh for detailed collision checking.
     *
     * @param stl_filepath The file path to the end effector STL file.
     * @param Hce          The rigid transform to apply to the STL model.
     * @param margin       An optional collision margin to expand the box (default is 0).
     */
    void updateEndEffectorFromSTL(const std::string& stl_filepath,
                                  const Eigen::Transform<Scalar, 3, Eigen::Isometry>& Hce,
                                  Scalar margin = Scalar(0)) {
        // Load the STL file.
        pcl::PolygonMesh mesh;
        if (pcl::io::loadPolygonFileSTL(stl_filepath, mesh) == 0) {
            std::cerr << "[PlannerMpc::updateEndEffectorFromSTL] Failed to load STL file: " << stl_filepath
                      << std::endl;
            return;
        }

        // Convert the mesh to a point cloud.
        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>());
        pcl::fromPCLPointCloud2(mesh.cloud, *cloud);
        if (cloud->empty()) {
            std::cerr << "[PlannerMpc::updateEndEffectorFromSTL] STL file contains no points: " << stl_filepath
                      << std::endl;
            return;
        }

        // --- Apply scaling and re-centering ---
        // For example, set the desired scale factor:
        float cutter_scale = 1.0f;
        // Compute the look_at_goal of the cloud.
        Eigen::Vector4f centroid(0, 0, 0, 0);
        for (const auto& pt : cloud->points) {
            centroid += Eigen::Vector4f(pt.x, pt.y, pt.z, 1.0f);
        }
        centroid /= static_cast<float>(cloud->points.size());
        // Translate points so that the centroid is at the origin and apply scaling.
        for (auto& pt : cloud->points) {
            pt.x = cutter_scale * (pt.x - centroid.x());
            pt.y = cutter_scale * (pt.y - centroid.y());
            pt.z = cutter_scale * (pt.z - centroid.z());
        }
        // --- End scaling and re-centering ---

        // Cast the provided transform to float for computation.
        Eigen::Matrix4f Hce_f = Hce.matrix().template cast<float>();

        // Compute the axis-aligned bounding box (AABB) for collision checking.
        Eigen::Vector4f min_pt(std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               1.0f);
        Eigen::Vector4f max_pt(-std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               1.0f);
        for (const auto& pt : cloud->points) {
            Eigen::Vector4f p(pt.x, pt.y, pt.z, 1.0f);
            Eigen::Vector4f p_transformed = Hce_f * p;
            min_pt                        = min_pt.cwiseMin(p_transformed);
            max_pt                        = max_pt.cwiseMax(p_transformed);
        }

        // Apply the collision margin to expand the box.
        min_pt[0] -= margin;
        min_pt[1] -= margin;
        min_pt[2] -= margin;
        max_pt[0] += margin;
        max_pt[1] += margin;
        max_pt[2] += margin;

        // Update internal box dimensions.
        box_min = min_pt;
        box_max = max_pt;
        std::cout << "[PlannerMpc::updateEndEffectorFromSTL] Updated box dimensions:\n"
                  << "  box_min: " << box_min.transpose() << "\n"
                  << "  box_max: " << box_max.transpose() << std::endl;

        // Downsample the point cloud using a VoxelGrid filter.
        pcl::VoxelGrid<pcl::PointXYZ> voxel_filter;
        voxel_filter.setInputCloud(cloud);
        float leaf_size = 0.02f;  // Adjust the voxel (leaf) size as needed.
        voxel_filter.setLeafSize(leaf_size, leaf_size, leaf_size);
        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_downsampled(new pcl::PointCloud<pcl::PointXYZ>());
        voxel_filter.filter(*cloud_downsampled);
        std::cout << "[PlannerMpc::updateEndEffectorFromSTL] Downsampled point cloud from " << cloud->points.size()
                  << " to " << cloud_downsampled->points.size() << " points.\n";

        // Clear the existing stored end-effector mesh cloud.
        ee_mesh_cloud->clear();

        // Transform each downsampled point and store it.
        for (const auto& pt : cloud_downsampled->points) {
            pcl::PointXYZ pt_transformed;
            Eigen::Vector4f p(pt.x, pt.y, pt.z, 1.0f);
            Eigen::Vector4f p_trans = Hce_f * p;
            pt_transformed.x        = p_trans[0];
            pt_transformed.y        = p_trans[1];
            pt_transformed.z        = p_trans[2];
            ee_mesh_cloud->points.push_back(pt_transformed);
        }

        std::cout << "[PlannerMpc::updateEndEffectorFromSTL] Stored " << ee_mesh_cloud->points.size()
                  << " mesh points for collision checking.\n";
    }
};

#endif  // WAYPOINT_PLANNER_HPP
