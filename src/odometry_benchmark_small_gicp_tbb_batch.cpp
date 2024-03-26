#include <small_gicp/benchmark/benchmark_odom.hpp>

#include <tbb/tbb.h>
#include <small_gicp/factors/gicp_factor.hpp>
#include <small_gicp/points/point_cloud.hpp>
#include <small_gicp/ann/kdtree.hpp>
#include <small_gicp/util/normal_estimation.hpp>
#include <small_gicp/registration/registration.hpp>

namespace small_gicp {

class BatchedSmallGICPOnlineOdometryEstimationTBB : public OdometryEstimation {
public:
  struct InputFrame {
    using Ptr = std::shared_ptr<InputFrame>;
    size_t id;
    PointCloud::Ptr points;
    KdTree<PointCloud>::Ptr kdtree;
    Eigen::Isometry3d T_last_current;
    Stopwatch sw;
  };

  struct InputFramePair {
    InputFrame::Ptr target;
    InputFrame::Ptr source;
  };

  BatchedSmallGICPOnlineOdometryEstimationTBB(const OdometryEstimationParams& params)
  : OdometryEstimation(params),
    control(tbb::global_control::max_allowed_parallelism, params.num_threads),
    throughput(0.0) {}

  ~BatchedSmallGICPOnlineOdometryEstimationTBB() { guik::async_destroy(); }

  std::vector<Eigen::Isometry3d> estimate(std::vector<PointCloud::Ptr>& points) override {
    auto async_viewer = guik::async_viewer();

    std::vector<Eigen::Isometry3d> traj;
    traj.reserve(points.size());

    tbb::flow::graph graph;
    tbb::flow::broadcast_node<InputFrame::Ptr> input_node(graph);
    tbb::flow::function_node<InputFrame::Ptr, InputFrame::Ptr> preprocess_node(graph, tbb::flow::unlimited, [&](const InputFrame::Ptr& input) {
      input->sw.start();
      input->points = voxelgrid_sampling(*input->points, params.downsample_resolution);
      input->kdtree = std::make_shared<KdTree<PointCloud>>(input->points);
      estimate_covariances(*input->points, *input->kdtree, 10);
      return input;
    });
    tbb::flow::sequencer_node<InputFrame::Ptr> postpre_sequencer_node(graph, [](const InputFrame::Ptr& input) { return input->id; });
    tbb::flow::function_node<InputFrame::Ptr, InputFramePair> pairing_node(graph, 1, [&](const InputFrame::Ptr& input) {
      static InputFrame::Ptr last_frame;
      InputFramePair pair = {last_frame, input};
      last_frame = input;
      return pair;
    });
    tbb::flow::function_node<InputFramePair, InputFrame::Ptr> registration_node(graph, tbb::flow::unlimited, [&](const InputFramePair& pair) {
      if (pair.target == nullptr) {
        pair.source->T_last_current.setIdentity();
        return pair.source;
      }

      Registration<GICPFactor, SerialReduction> registration;
      registration.rejector.max_dist_sq = params.max_correspondence_distance * params.max_correspondence_distance;
      const auto result = registration.align(*pair.target->points, *pair.source->points, *pair.target->kdtree, Eigen::Isometry3d::Identity());
      pair.source->T_last_current = result.T_target_source;
      return pair.source;
    });
    tbb::flow::sequencer_node<InputFrame::Ptr> postreg_sequencer_node(graph, [](const InputFrame::Ptr& input) { return input->id; });
    tbb::flow::function_node<InputFrame::Ptr> viewer_node(graph, 1, [&](const InputFrame::Ptr& input) {
      if (traj.empty()) {
        traj.emplace_back(input->T_last_current);
      } else {
        traj.emplace_back(traj.back() * input->T_last_current);
      }
      const Eigen::Isometry3d T = traj.back();

      input->sw.stop();
      reg_times.push(input->sw.msec());

      static auto t0 = input->sw.t1;
      const double elapsed_msec = std::chrono::duration_cast<std::chrono::nanoseconds>(input->sw.t2 - t0).count() / 1e6;
      throughput = elapsed_msec / (input->id + 1);

      if (input->id && input->id % 1024 == 0) {
        report();
      }

      async_viewer->update_points(guik::anon(), input->points->points, guik::Rainbow(T));
      async_viewer->update_points("points", input->points->points, guik::FlatOrange(T).set_point_scale(2.0f));
    });

    tbb::flow::make_edge(input_node, preprocess_node);
    tbb::flow::make_edge(preprocess_node, postpre_sequencer_node);
    tbb::flow::make_edge(postpre_sequencer_node, pairing_node);
    tbb::flow::make_edge(pairing_node, registration_node);
    tbb::flow::make_edge(registration_node, postreg_sequencer_node);
    tbb::flow::make_edge(postreg_sequencer_node, viewer_node);

    Stopwatch sw;
    sw.start();
    for (size_t i = 0; i < points.size(); i++) {
      auto frame = InputFrame::Ptr(new InputFrame);
      frame->id = i;
      frame->points = points[i];
      if (!input_node.try_put(frame)) {
        std::cerr << "failed to input!!" << std::endl;
      }
    }
    graph.wait_for_all();

    sw.stop();
    throughput = sw.msec() / points.size();

    return traj;
  }

  void report() override {  //
    std::cout << "registration_time_stats=" << reg_times.str() << " [msec/scan]  total_throughput=" << throughput << " [msec/scan]" << std::endl;
  }

private:
  tbb::global_control control;

  double throughput;
  Summarizer reg_times;
};

static auto small_gicp_tbb_batch_registry =
  register_odometry("small_gicp_tbb_batch", [](const OdometryEstimationParams& params) { return std::make_shared<BatchedSmallGICPOnlineOdometryEstimationTBB>(params); });

}  // namespace small_gicp