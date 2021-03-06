
#ifdef WITH_PYTHON_LAYER
#include "boost/python.hpp"
namespace bp = boost::python;
#endif

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "boost/algorithm/string.hpp"
#include "caffe/caffe.hpp"
#include "caffe/device.hpp"
#include "caffe/util/signal_handler.h"

#ifdef USE_LIBDNN
#include "caffe/layers/libdnn_conv_layer.hpp"
#endif

using caffe::Blob;
using caffe::Caffe;
using caffe::Net;
using caffe::Layer;
using caffe::Solver;
using caffe::shared_ptr;
using caffe::string;
using caffe::Timer;
using caffe::vector;
using caffe::device;
using std::ostringstream;

DEFINE_string(gpu, "",
    "Optional; run in GPU mode on given device IDs separated by ','."
    "Use '-gpu all' to run on all available GPUs. The effective training "
    "batch size is multiplied by the number of devices.");
DEFINE_string(solver, "",
    "The solver definition protocol buffer text file.");
DEFINE_string(model, "",
    "The model definition protocol buffer text file.");
DEFINE_string(phase, "",
    "Optional; network phase (TRAIN or TEST). Only used for 'time'.");
DEFINE_int32(level, 0,
    "Optional; network level.");
DEFINE_string(stage, "",
    "Optional; network stages (not to be confused with phase), "
    "separated by ','.");
DEFINE_string(snapshot, "",
    "Optional; the snapshot solver state to resume training.");
DEFINE_string(weights, "",
    "Optional; the pretrained weights to initialize finetuning, "
    "separated by ','. Cannot be set simultaneously with snapshot.");
DEFINE_int32(iterations, 50,
    "The number of iterations to run.");
DEFINE_string(sigint_effect, "stop",
             "Optional; action to take when a SIGINT signal is received: "
              "snapshot, stop or none.");
DEFINE_string(sighup_effect, "snapshot",
             "Optional; action to take when a SIGHUP signal is received: "
             "snapshot, stop or none.");
DEFINE_bool(lt, false,
    "Optional; enable per layer timings");
DEFINE_bool(detection, false,
    "Optional; Enables detection mAP test for on TEST phase. "
    "By default test is used for classification only.");
DEFINE_string(ap, "11point",
    "Optional; Method to get mAP. Default is VOC2007 method \"11point\"."
    "other methods are \"MaxIntegral\" and \"Integral\".");


// A simple registry for caffe commands.
typedef int (*BrewFunction)();
typedef std::map<caffe::string, BrewFunction> BrewMap;
BrewMap g_brew_map;

#define RegisterBrewFunction(func) \
namespace { \
class __Registerer_##func { \
 public: /* NOLINT */ \
  __Registerer_##func() { \
    g_brew_map[#func] = &func; \
  } \
}; \
__Registerer_##func g_registerer_##func; \
}

int actions() {
  LOG(ERROR) << "Available caffe actions:";
  for (BrewMap::iterator it = g_brew_map.begin();
       it != g_brew_map.end(); ++it) {
    LOG(ERROR) << "\t" << it->first;
  }
  return 0;
}

RegisterBrewFunction(actions);

static BrewFunction GetBrewFunction(const caffe::string& name) {
  if (g_brew_map.count(name)) {
    return g_brew_map[name];
  } else {
    LOG(ERROR) << "Unknown action: " << name;
    return g_brew_map["actions"];  // not reachable, just to suppress old compiler warnings.
  }
}

// Parse GPU ids or use all available devices
static void get_gpus(vector<int>* gpus) {
  if (FLAGS_gpu == "all") {
    int count = 0;
#ifndef CPU_ONLY
    count = Caffe::EnumerateDevices(true);
#else
    NO_GPU;
#endif
    for (int i = 0; i < count; ++i) {
      gpus->push_back(i);
    }
  } else if (FLAGS_gpu.size()) {
    vector<string> strings;
    boost::split(strings, FLAGS_gpu, boost::is_any_of(","));
    for (int i = 0; i < strings.size(); ++i) {
      gpus->push_back(boost::lexical_cast<int>(strings[i]));
    }
  } else {
    CHECK_EQ(gpus->size(), 0);
  }
}

// Parse phase from flags
caffe::Phase get_phase_from_flags(caffe::Phase default_value) {
  if (FLAGS_phase == "")
    return default_value;
  if (FLAGS_phase == "TRAIN")
    return caffe::TRAIN;
  if (FLAGS_phase == "TEST")
    return caffe::TEST;
  LOG(FATAL) << "phase must be \"TRAIN\" or \"TEST\"";
  return caffe::TRAIN;  // Avoid warning
}

// Parse stages from flags
vector<string> get_stages_from_flags() {
  vector<string> stages;
  boost::split(stages, FLAGS_stage, boost::is_any_of(","));
  return stages;
}

// caffe commands to call by
//     caffe <command> <args>
//
// To add a command, define a function "int command()" and register it with
// RegisterBrewFunction(action);

// Device Query: show diagnostic information for a GPU device, or
// enumerate all devices if none is specified.
int device_query() {
  if (FLAGS_gpu.size() == 0 || FLAGS_gpu == "all") {
    // If no gpu is specified, enumerate all the devices.
    caffe::Caffe::EnumerateDevices();
  } else {
#ifndef CPU_ONLY
    LOG(INFO) << "Querying GPUs " << FLAGS_gpu;
    vector<int> gpus;
    get_gpus(&gpus);
    Caffe::SetDevices(gpus);
    for (int i = 0; i < gpus.size(); ++i) {
      caffe::Caffe::SetDevice(gpus[i]);
      caffe::Caffe::DeviceQuery();
    }
#ifdef USE_GREENTEA
  if (Caffe::GetDefaultDevice()->backend() == caffe::BACKEND_OpenCL) {
    if (gpus.size() > 0 && gpus[0] >= 0) {
      // Explicitly call for OCL + FFT
      caffe::Caffe::TeardownDevice(gpus[0]);
    }
  }
#endif  // USE_GREENTEA
#endif  // !CPU_ONLY
  }
  return 0;
}
RegisterBrewFunction(device_query);

// Load the weights from the specified caffemodel(s) into the train and
// test nets.
void CopyLayers(caffe::Solver<float>* solver, const std::string& model_list) {
  std::vector<std::string> model_names;
  boost::split(model_names, model_list, boost::is_any_of(",") );
  for (int i = 0; i < model_names.size(); ++i) {
    LOG(INFO) << "Finetuning from " << model_names[i];
    solver->net()->CopyTrainedLayersFrom(model_names[i]);
    for (int j = 0; j < solver->test_nets().size(); ++j) {
      solver->test_nets()[j]->CopyTrainedLayersFrom(model_names[i]);
    }
  }
}

// Translate the signal effect the user specified on the command-line to the
// corresponding enumeration.
caffe::SolverAction::Enum GetRequestedAction(
    const std::string& flag_value) {
  if (flag_value == "stop") {
    return caffe::SolverAction::STOP;
  }
  if (flag_value == "snapshot") {
    return caffe::SolverAction::SNAPSHOT;
  }
  if (flag_value == "none") {
    return caffe::SolverAction::NONE;
  }
  LOG(FATAL) << "Invalid signal effect \""<< flag_value << "\" was specified";
  return caffe::SolverAction::NONE;
}

// Train / Finetune a model.
int train() {
  CHECK_GT(FLAGS_solver.size(), 0) << "Need a solver definition to train.";
  CHECK(!FLAGS_snapshot.size() || !FLAGS_weights.size())
      << "Give a snapshot to resume training or weights to finetune "
      "but not both.";
  vector<string> stages = get_stages_from_flags();

  caffe::SolverParameter solver_param;
  caffe::ReadSolverParamsFromTextFileOrDie(FLAGS_solver, &solver_param);

  solver_param.mutable_train_state()->set_level(FLAGS_level);
  for (int i = 0; i < stages.size(); i++) {
    solver_param.mutable_train_state()->add_stage(stages[i]);
  }

  // If the gpus flag is not provided, allow the mode and device to be set
  // in the solver prototxt.
  if (FLAGS_gpu.size() == 0
      && solver_param.has_solver_mode()
      && solver_param.solver_mode() == caffe::SolverParameter_SolverMode_GPU) {
      if (solver_param.has_device_id()) {
          FLAGS_gpu = "" +
              boost::lexical_cast<string>(solver_param.device_id());
      } else {  // Set default GPU if unspecified
          FLAGS_gpu = "" + boost::lexical_cast<string>(0);
      }
  }

  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() == 0) {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  } else {
#ifndef CPU_ONLY
    // Load all devices that will be used
    Caffe::SetDevices(gpus);

    ostringstream s;
    for (int_tp i = 0; i < gpus.size(); ++i) {
      s << (i ? ", " : "") << gpus[i];
    }
    LOG(INFO) << "Using GPUs " << s.str();
    solver_param.set_device_id(gpus[0]);
    // Initialize the first device
    Caffe::SetDevice(gpus[0]);
    Caffe::set_mode(Caffe::GPU);
    Caffe::set_solver_count(gpus.size());
#endif  // !CPU_ONLY
  }

  caffe::SignalHandler signal_handler(
        GetRequestedAction(FLAGS_sigint_effect),
        GetRequestedAction(FLAGS_sighup_effect));

  shared_ptr<caffe::Solver<float> >
      solver(caffe::SolverRegistry<float>::CreateSolver(solver_param));

  solver->SetActionFunction(signal_handler.GetActionFunction());

  if (FLAGS_snapshot.size()) {
    LOG(INFO) << "Resuming from " << FLAGS_snapshot;
    solver->Restore(FLAGS_snapshot.c_str());
  } else if (FLAGS_weights.size()) {
    CopyLayers(solver.get(), FLAGS_weights);
  }

  LOG(INFO) << "Starting Optimization";
  if (gpus.size() > 1) {
#ifdef USE_CUDA
#ifdef USE_NCCL
    caffe::NCCL<float> nccl(solver);
    nccl.Run(gpus, FLAGS_snapshot.size() > 0 ? FLAGS_snapshot.c_str() : NULL);
#else
    LOG(FATAL) << "Multi-GPU execution not available - rebuild with USE_NCCL";
#endif  // USE_NCCL
#endif  // USE_CUDA
  } else {
    solver->Solve();
  }
  LOG(INFO) << "Optimization Done.";

#ifdef USE_GREENTEA
  if (Caffe::GetDefaultDevice()->backend() == caffe::BACKEND_OpenCL) {
    if (gpus.size() > 0 && gpus[0] >= 0) {
      // Explicitly call for OCL + FFT
      caffe::Caffe::TeardownDevice(gpus[0]);
    }
  }
#endif
  return 0;
}
RegisterBrewFunction(train);

int test_detection(Net<float>& caffe_net) {
  std::map<int, std::map<int,
    std::vector<std::pair<float, int> > > > all_true_pos;
  std::map<int, std::map<int,
    std::vector<std::pair<float, int> > > > all_false_pos;
  std::map<int, std::map<int, int> > all_num_pos;

  for (int i = 0; i < FLAGS_iterations; ++i) {
    float iter_loss;
    const vector<Blob<float>*>& result = caffe_net.Forward(&iter_loss);

    for (int j = 0; j < result.size(); ++j) {
      const float* result_vec = result[j]->cpu_data();

      int num_det = result[j]->height();
      for (int k = 0; k < num_det; ++k) {
        int item_id = static_cast<int>(result_vec[k * 5]);
        int label = static_cast<int>(result_vec[k * 5 + 1]);
        if (item_id == -1) {
          // Special row of storing number of positives for a label.
          if (all_num_pos[j].find(label) == all_num_pos[j].end()) {
            all_num_pos[j][label] = static_cast<int>(result_vec[k * 5 + 2]);
          } else {
            all_num_pos[j][label] += static_cast<int>(result_vec[k * 5 + 2]);
          }
        } else {
          // Normal row storing detection status.
          float score = result_vec[k * 5 + 2];
          int tp = static_cast<int>(result_vec[k * 5 + 3]);
          int fp = static_cast<int>(result_vec[k * 5 + 4]);
          if (tp == 0 && fp == 0) {
            // Ignore such case. It happens when a detection bbox is matched to
            // a difficult gt bbox and we don't evaluate on difficult gt bbox.
            continue;
          }
          all_true_pos[j][label].push_back(std::make_pair(score, tp));
          all_false_pos[j][label].push_back(std::make_pair(score, fp));
        }
      }
    }
  }

  for (int i = 0; i < all_true_pos.size(); ++i) {
    if (all_true_pos.find(i) == all_true_pos.end()) {
      LOG(FATAL) << "Missing output_blob true_pos: " << i;
    }
    const std::map<int, std::vector<std::pair<float, int> > >& true_pos =
        all_true_pos.find(i)->second;
    if (all_false_pos.find(i) == all_false_pos.end()) {
      LOG(FATAL) << "Missing output_blob false_pos: " << i;
    }
    const std::map<int, std::vector<std::pair<float, int> > >& false_pos =
        all_false_pos.find(i)->second;
    if (all_num_pos.find(i) == all_num_pos.end()) {
      LOG(FATAL) << "Missing output_blob num_pos: " << i;
    }
    const std::map<int, int>& num_pos = all_num_pos.find(i)->second;
    std::map<int, float> APs;
    float mAP = 0.;
    // Sort true_pos and false_pos with descend scores.
    for (std::map<int, int>::const_iterator it = num_pos.begin();
         it != num_pos.end(); ++it) {
      int label = it->first;
      int label_num_pos = it->second;
      if (true_pos.find(label) == true_pos.end()) {
        LOG(WARNING) << "Missing true_pos for label: " << label;
        continue;
      }
      const std::vector<std::pair<float, int> >& label_true_pos =
          true_pos.find(label)->second;
      if (false_pos.find(label) == false_pos.end()) {
        LOG(WARNING) << "Missing false_pos for label: " << label;
        continue;
      }
      const std::vector<std::pair<float, int> >& label_false_pos =
          false_pos.find(label)->second;
      std::vector<float> prec, rec;
      caffe::ComputeAP(label_true_pos, label_num_pos, label_false_pos,
                FLAGS_ap, &prec, &rec, &(APs[label]));
      mAP += APs[label];
    }
    mAP /= num_pos.size();
    const int output_blob_index = caffe_net.output_blob_indices()[i];
    const string& output_name = caffe_net.blob_names()[output_blob_index];
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mAP;
  }

  return 0;
}

// Test: score a model.
int test() {
  CHECK_GT(FLAGS_model.size(), 0) << "Need a model definition to score.";
  CHECK_GT(FLAGS_weights.size(), 0) << "Need model weights to score.";
  vector<string> stages = get_stages_from_flags();

  // Set device id and mode
  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() != 0) {
#ifndef CPU_ONLY
    LOG(INFO) << "Use GPU with device ID " << gpus[0];
    Caffe::SetDevices(gpus);
    Caffe::set_mode(Caffe::GPU);
    Caffe::SetDevice(gpus[0]);
#endif  // !CPU_ONLY
  } else {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  }
  // Instantiate the caffe net.
  Net<float> caffe_net(FLAGS_model, caffe::TEST,
                       Caffe::GetDefaultDevice(), FLAGS_level, &stages);
  caffe_net.CopyTrainedLayersFrom(FLAGS_weights);
  LOG(INFO) << "Running for " << FLAGS_iterations << " iterations.";

  if (FLAGS_detection) {
    test_detection(caffe_net);
  } else {
    vector<int> test_score_output_id;
    vector<float> test_score;
    float loss = 0;
    for (int_tp i = 0; i < FLAGS_iterations; ++i) {
      float iter_loss;
      const vector<Blob<float>*>& result =
          caffe_net.Forward(&iter_loss);
      loss += iter_loss;
      int_tp idx = 0;
      for (int_tp j = 0; j < result.size(); ++j) {
        const float* result_vec = result[j]->cpu_data();
        for (int_tp k = 0; k < result[j]->count(); ++k, ++idx) {
          const float score = result_vec[k];
          if (i == 0) {
            test_score.push_back(score);
            test_score_output_id.push_back(j);
          } else {
            test_score[idx] += score;
          }
          const std::string& output_name = caffe_net.blob_names()[
              caffe_net.output_blob_indices()[j]];
          LOG(INFO) << "Batch " << i << ", " << output_name << " = " << score;
        }
      }
    }
    loss /= FLAGS_iterations;
    LOG(INFO) << "Loss: " << loss;
    for (int_tp i = 0; i < test_score.size(); ++i) {
      const std::string& output_name = caffe_net.blob_names()[
          caffe_net.output_blob_indices()[test_score_output_id[i]]];
      const float loss_weight = caffe_net.blob_loss_weights()[
          caffe_net.output_blob_indices()[test_score_output_id[i]]];
      std::ostringstream loss_msg_stream;
      const float mean_score = test_score[i] / FLAGS_iterations;
      if (loss_weight) {
        loss_msg_stream << " (* " << loss_weight
                        << " = " << loss_weight * mean_score << " loss)";
      }
      LOG(INFO) << output_name << " = " << mean_score << loss_msg_stream.str();
    }
  }
#ifdef USE_GREENTEA
  if (Caffe::GetDefaultDevice()->backend() == caffe::BACKEND_OpenCL) {
    if (gpus.size() > 0 && gpus[0] >= 0) {
      // Explicitly call for OCL + FFT
      caffe::Caffe::TeardownDevice(gpus[0]);
    }
  }
#endif

  return 0;
}
RegisterBrewFunction(test);


// Time: benchmark the execution time of a model.
int time() {
  CHECK_GT(FLAGS_model.size(), 0) << "Need a model definition to time.";
  caffe::Phase phase = get_phase_from_flags(caffe::TRAIN);
  vector<string> stages = get_stages_from_flags();

  // Set device id and mode
  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() != 0) {
#ifndef CPU_ONLY
    LOG(INFO) << "Use GPU with device ID " << gpus[0];
    Caffe::SetDevices(gpus);
    Caffe::set_mode(Caffe::GPU);
    Caffe::SetDevice(gpus[0]);
#endif  // !CPU_ONLY
  } else {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  }
  // Instantiate the caffe net.
  Net<float> caffe_net(FLAGS_model, phase,
                       Caffe::GetDefaultDevice(), FLAGS_level, &stages);

  // Do a clean forward and backward pass, so that memory allocation are done
  // and future iterations will be more stable.
  LOG(INFO) << "Performing Forward";
  // Note that for the speed benchmark, we will assume that the network does
  // not take any input blobs.
  float initial_loss;
  caffe_net.Forward(&initial_loss);
  LOG(INFO) << "Initial loss: " << initial_loss;
  if (phase == caffe::TRAIN) {
    LOG(INFO) << "Performing Backward";
    caffe_net.Backward();
  }

  const vector<shared_ptr<Layer<float> > >& layers = caffe_net.layers();
  const vector<vector<Blob<float>*> >& bottom_vecs = caffe_net.bottom_vecs();
  const vector<vector<Blob<float>*> >& top_vecs = caffe_net.top_vecs();
  const vector<vector<bool> >& bottom_need_backward =
      caffe_net.bottom_need_backward();
  LOG(INFO) << "*** Benchmark begins ***";
  LOG(INFO) << "Testing for " << FLAGS_iterations << " iterations.";
  Timer total_timer;
  total_timer.Start();
  Timer forward_timer;
  std::vector<double> forward_time_per_layer(layers.size(), 0.0);
  std::vector<double> backward_time_per_layer(layers.size(), 0.0);
  double forward_time = 0.0;
  double backward_time = 0.0;

  for (int_tp j = 0; j < FLAGS_iterations; ++j) {
    Timer iter_timer;
    std::vector<Timer> layer_timers(layers.size());
    iter_timer.Start();
    forward_timer.Start();
    for (int_tp i = 0; i < layers.size(); ++i) {
      if (FLAGS_lt) {
        layer_timers[i].Start();
      }
      layers[i]->Forward(bottom_vecs[i], top_vecs[i]);
      if (FLAGS_lt) {
        layer_timers[i].Stop();
      }
    }
    Caffe::Synchronize(Caffe::GetDefaultDevice()->id());
    forward_time += forward_timer.MicroSeconds();
    if (FLAGS_lt) {
      for (int_tp i = 0; i < layers.size(); ++i) {
        forward_time_per_layer[i] += layer_timers[i].MicroSeconds();
      }
    }
    if (phase == caffe::TRAIN) {
      for (int_tp i = layers.size() - 1; i >= 0; --i) {
        if (FLAGS_lt) {
          layer_timers[i].Start();
        }
        layers[i]->Backward(top_vecs[i], bottom_need_backward[i],
                            bottom_vecs[i]);
        if (FLAGS_lt) {
          layer_timers[i].Stop();
        }
      }
      Caffe::Synchronize(Caffe::GetDefaultDevice()->id());
      if (FLAGS_lt) {
        for (int_tp i = 0; i < layers.size(); ++i) {
          backward_time_per_layer[i] += layer_timers[i].MicroSeconds();
          backward_time += backward_time_per_layer[i];
        }
      }
    }
    LOG(INFO) << "Iteration: " << j + 1 << " forward-backward time: "
      << iter_timer.MilliSeconds() << " ms.";
  }

  if (FLAGS_lt) {
    LOG(INFO) << "Average time per layer: ";
    for (int_tp i = 0; i < layers.size(); ++i) {
      const caffe::string& layername = layers[i]->layer_param().name();
      LOG(INFO) << std::setfill(' ') << std::setw(10) << layername <<
        "\tforward: " << forward_time_per_layer[i] / 1000 /
        FLAGS_iterations << " ms.";
      LOG(INFO) << std::setfill(' ') << std::setw(10) << layername  <<
        "\tbackward: " << backward_time_per_layer[i] / 1000 /
        FLAGS_iterations << " ms.";
    }
  }
  total_timer.Stop();
  LOG(INFO) << "Average Forward pass: " << forward_time / 1000 /
    FLAGS_iterations << " ms.";
  LOG(INFO) << "Average Backward pass: " << backward_time / 1000 /
    FLAGS_iterations << " ms.";
  LOG(INFO) << "Average Forward-Backward: " << total_timer.MilliSeconds() /
    FLAGS_iterations << " ms.";
  LOG(INFO) << "Total Time: " << total_timer.MilliSeconds() << " ms.";
  LOG(INFO) << "*** Benchmark ends ***";

#ifdef USE_GREENTEA
  if (Caffe::GetDefaultDevice()->backend() == caffe::BACKEND_OpenCL) {
    if (gpus.size() > 0 && gpus[0] >= 0) {
      // Explicitly call for OCL + FFT
      caffe::Caffe::TeardownDevice(gpus[0]);
    }
  }
#endif
  return 0;
}
RegisterBrewFunction(time);


int autotune() {
  CHECK_GT(FLAGS_model.size(), 0) << "Need a model definition to time.";

  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() == 0) {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  } else {
#ifndef CPU_ONLY
    // Load all devices that will be used
    Caffe::SetDevices(gpus);

    ostringstream s;
    for (int_tp i = 0; i < gpus.size(); ++i) {
      s << (i ? ", " : "") << gpus[i];
    }
    LOG(INFO) << "Using GPUs " << s.str();
    // Initialize the first device
    Caffe::SetDevice(gpus[0]);
    Caffe::set_mode(Caffe::GPU);
    Caffe::set_solver_count(gpus.size());
#endif  // !CPU_ONLY
  }

  caffe::SignalHandler signal_handler(
        GetRequestedAction(FLAGS_sigint_effect),
        GetRequestedAction(FLAGS_sighup_effect));

  Net<float> net(FLAGS_model, caffe::TRAIN, Caffe::GetDefaultDevice());

  for (int i = 0; i < net.layers().size(); ++i) {
#ifdef USE_LIBDNN
    shared_ptr<caffe::LibDNNConvolutionLayer<float> > layer =
        boost::dynamic_pointer_cast<caffe::LibDNNConvolutionLayer<float> >
                (net.layers()[i]);
    if (layer.get() != nullptr) {
      float* top_data = net.top_vecs()[i][0]->mutable_gpu_data();
      float* top_diff = net.top_vecs()[i][0]->mutable_gpu_diff();
      float* bottom_data = net.top_vecs()[i][0]->mutable_gpu_data();
      float* bottom_diff = net.top_vecs()[i][0]->mutable_gpu_diff();
      int_tp batch_size = net.top_vecs()[i][0]->shape(0);
      layer->Tune(top_data, top_diff, bottom_data, bottom_diff, batch_size);
    }
#endif  // USE_LIBDNN
  }
  return 0;
}
RegisterBrewFunction(autotune);




int main(int argc, char** argv) {
  // Print output to stderr (while still logging).
  FLAGS_alsologtostderr = 1;
  // Set version
  gflags::SetVersionString(AS_STRING(CAFFE_VERSION));
  // Usage message.
  gflags::SetUsageMessage("command line brew\n"
      "usage: caffe <command> <args>\n\n"
      "commands:\n"
      "  train           train or finetune a model\n"
      "  test            score a model\n"
      "  device_query    show GPU diagnostic information\n"
      "  time            benchmark model execution time"
      "  autotune        autotune a model");
  // Run tool or show usage.
  caffe::GlobalInit(&argc, &argv);
  if (argc == 2) {
#ifdef WITH_PYTHON_LAYER
    try {
#endif
      return GetBrewFunction(caffe::string(argv[1]))() != 0;
#ifdef WITH_PYTHON_LAYER
    } catch (bp::error_already_set) {
      PyErr_Print();
      return 1;
    }
#endif
  } else {
    gflags::ShowUsageWithFlagsRestrict(argv[0], "tools/caffe");
  }
  return 0;
}
