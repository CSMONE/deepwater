/*!
 * Copyright (c) 2016 by Contributors
 */
#include <vector>
#include <string>
#include <cassert>
#include <map>
#include <stdio.h>
#include <string.h>

#include "include/symbol.h"
#include "include/optimizer.h"
#include "include/initializer.h"
#include "image_train.hpp"

using namespace mxnet::cpp;

#undef MYDEBUG

#ifdef MYDEBUG
static int count = 0;
const double C = 3;
#endif

ImageTrain::ImageTrain(int w, int h, int c, int device, int seed, bool gpu) {
  width = w;
  height = h;
  channels = c;
  learning_rate = 1e-2;
  weight_decay = 1e-6;
  momentum = 0.9;
  clip_gradient = 10;
  is_built = false;
#if MSHADOW_USE_CUDA == 0
  ctx_dev = Context(DeviceType::kCPU, device);
#else
  if (!gpu)
    ctx_dev = Context(DeviceType::kCPU, device);
  else
    ctx_dev = Context(DeviceType::kGPU, device);
#endif
  setSeed(seed);
}

void ImageTrain::setSeed(int seed) {
  CHECK_EQ(MXRandomSeed(seed), 0);
}

void ImageTrain::setClassificationDimensions(int n, int b) {
  batch_size = b;
  num_classes = n;
  preds.resize(num_classes * batch_size);

  if (height>0 || channels>0) { //image classification
    shape = Shape(batch_size, channels, width, height);
  } else { //otherwise
    assert(width>0);
    shape = Shape(batch_size, width);
  } 
  std::cout << "mxnet data input shape: " << shape << std::endl;
  args_map["data"] = NDArray(shape, ctx_dev);
  args_map["softmax_label"] = NDArray(Shape(batch_size), ctx_dev);
  mxnet_sym.InferArgsMap(ctx_dev, &args_map, args_map);

  // update the executor to use the new args_map and aux_map
  exec = std::unique_ptr<Executor>(mxnet_sym.SimpleBind(ctx_dev,
        args_map,
        std::map<std::string, NDArray>(),
        std::map<std::string, OpReqType>(),
        aux_map));
}

void ImageTrain::setOptimizer() {
  opt = std::unique_ptr<Optimizer>(new Optimizer("ccsgd", learning_rate, weight_decay));
  opt->SetParam("momentum", momentum);
  opt->SetParam("rescale_grad", 1.0 / batch_size);
  opt->SetParam("clip_gradient", clip_gradient);
}

void ImageTrain::initializeState() {
  args_map = exec->arg_dict();
  /*Xavier xavier = Xavier(Xavier::gaussian, Xavier::in, 2.34);*/
  Xavier xavier = Xavier(Xavier::uniform, Xavier::avg);
  for (auto &arg : args_map) {
    xavier(arg.first, &arg.second);
  }
  aux_map = exec->aux_dict();
  for (auto &aux : aux_map) {
    xavier(aux.first, &aux.second);
  }
  assert(opt);
  is_built = true;
}

void ImageTrain::buildNet(int n, int b, char * n_name,
    int num_hidden, int *hidden, char ** activations, 
    double input_dropout, double *hidden_dropout) {
  std::string net_name(n_name);
  if (net_name == "inception_bn") {
    mxnet_sym = InceptionSymbol(n);
  } else if (net_name == "vgg") {
    mxnet_sym = VGGSymbol(n);
  } else if (net_name == "lenet") {
    mxnet_sym = LenetSymbol(n);
  } else if (net_name == "alexnet") {
    mxnet_sym = AlexnetSymbol(n);
  } else if (net_name == "googlenet") {
    mxnet_sym = GoogleNetSymbol(n);
  } else if (net_name == "resnet") {
    mxnet_sym = ResNetSymbol(n);
  } else if (net_name == "relu_1024_relu_1024_relu_2048_dropout") {
    mxnet_sym = MLPSymbol({1024,1024,2048}, {"relu","relu","relu"}, n, 0.1, {0.5,0.5,0.5});
  } else if (net_name == "MLP") {
    assert(num_hidden>0);
    std::vector<int> hid(num_hidden);
    std::vector<double> hdrop(num_hidden);
    std::vector<std::string> act(num_hidden);
    for (size_t i=0;i<hid.size();++i) {
      hid[i]=hidden[i];
      act[i]=std::string(activations[i]);
      hdrop[i]=hidden_dropout[i];
    }
    mxnet_sym = MLPSymbol(hid, act, n, input_dropout, hdrop);
  } else if (net_name.find(".json") != std::string::npos || net_name.find(".network") != std::string::npos) {
    loadModel(n_name);
    is_built = false;
  } else {
    std::cerr << "Unsupported network preset " << n_name << std::endl;
    exit(-1);
  }
  //mxnet_sym.Save("/tmp/h2o.json");
  setClassificationDimensions(n, b);
  std::cerr << "Setting the optimizer." << std::endl;
  setOptimizer();
  std::cerr << "Initializing state." << std::endl;
  initializeState();
  std::cerr << "Done creating the model." << std::endl;
}


void ImageTrain::loadModel(char * model_path) {
  std::cerr << "Loading the model." << std::endl;
  mxnet_sym = Symbol::Load(std::string(model_path));
  std::cerr << "Done loading the model." << std::endl;
  is_built = true;
}

const char * ImageTrain::toJson() {
  std::string tmp = mxnet_sym.ToJSON();
  return strdup(tmp.c_str()); //this leaks, but at least doesn't corrupt
}

void ImageTrain::saveModel(char * model_path) {
  if (!is_built) {
    std::cerr << "Network not built!" << std::endl;
  } else {
    std::cerr << "Saving the model." << std::endl;
    mxnet_sym.Save(std::string(model_path));
    std::cerr << "Done saving the model." << std::endl;
  }
}


void ImageTrain::loadParam(char * param_path) {
  std::cerr << "Loading the model parameters." << std::endl;
  NDArray::WaitAll();
  std::map<std::string, NDArray> parameters;
  NDArray::Load(std::string(param_path), nullptr, &parameters);

  // only restored named symbols (both aux and arg)
  size_t args = 0;
  size_t aux = 0;
  for (const auto &k : parameters) {
    if (k.first.substr(0, 4) == "arg:") {
      args++;
      auto name = k.first.substr(4, k.first.size() - 4);
      args_map[name] = k.second.Copy(ctx_dev);
      k.second.WaitToRead();

#ifdef MYDEBUG
      // immediately bring the values back to the CPU and check
      std::vector<mx_float> vals(k.second.Size());
      MXNDArraySyncCopyToCPU(k.second.GetHandle(), &vals[0], vals.size());
      for (size_t j = 0; j < vals.size(); ++j) {
        if (j == 0 && vals[j] != C) {
          std::cerr << "load mismatch arg: pos " << j << ": "
                    << vals[j] << " != " << C << std::endl;
        }
      }
#endif
    }
    if (k.first.substr(0, 4) == "aux:") {
      aux++;
      auto name = k.first.substr(4, k.first.size() - 4);
      aux_map[name] = k.second.Copy(ctx_dev);
      k.second.WaitToRead();

#ifdef MYDEBUG
      // immediately bring the values back to the CPU and check
      std::vector<mx_float> vals(k.second.Size());
      MXNDArraySyncCopyToCPU(k.second.GetHandle(), &vals[0], vals.size());
      for (size_t j = 0; j < vals.size(); ++j) {
        if (j == 0 && vals[j] != C) {
          std::cerr << "load mismatch aux: pos " << j << ": "
                    << vals[j] << " != " << C << std::endl;
        }
      }
#endif
    }
  }
  if (args+2 != args_map.size()) {  // all but "data" and "softmax_label"
    std::cerr << "Expected to fill up " << args_map.size() - 2
              << " arg arrays, but only filled " << args << std::endl;
    exit(-1);
  }
  if (aux != aux_map.size()) {
    std::cerr << "Expected to fill up " << aux_map.size()
              << " aux arrays, but only filled " << aux << std::endl;
    exit(-1);
  }
  // update the executor to use the new args_map and aux_map
  exec = std::unique_ptr<Executor>(mxnet_sym.SimpleBind(ctx_dev,
        args_map,
        std::map<std::string, NDArray>(),
        std::map<std::string, OpReqType>(),
        aux_map));

#ifdef MYDEBUG
  // verify arg
  for (const auto &t : args_map) {
    if (t.first == "data" || t.first == "softmax_label")
      continue;
    std::vector<mx_float> vals(t.second.Size());
    MXNDArraySyncCopyToCPU(t.second.GetHandle(), &vals[0], vals.size());
    // second save: check that we are saving the correct values
    for (size_t j = 0; j < vals.size(); ++j) {
      if (j == 0 && vals[j] != C) {
        std::cerr << "load2 arg mismatch for " << t.first << ": pos "
                  << j << ": " << vals[j] << " != " << C << std::endl;
      }
    }
  }

  // verify aux
  for (const auto &t : aux_map) {
    if (t.first == "data" || t.first == "softmax_label")
      continue;
    std::vector<mx_float> vals(t.second.Size());
    MXNDArraySyncCopyToCPU(t.second.GetHandle(), &vals[0], vals.size());
    // second save: check that we are saving the correct values
    for (size_t j = 0; j < vals.size(); ++j) {
      if (j == 0 && vals[j] != C) {
        std::cerr << "load2 aux mismatch for " << t.first << ": pos "
                  << j << ": " << vals[j] << " != " << C << std::endl;
      }
    }
  }
#endif

  NDArray::WaitAll();
  std::cerr << "Done loading the model parameters." << std::endl;
}


void ImageTrain::saveParam(char * param_path) {
  std::cerr << "Saving the model parameters." << std::endl;
  args_map = exec->arg_dict();
  aux_map = exec->aux_dict();

  std::vector<NDArrayHandle> args;
  std::vector<std::string> keys;
  for (const auto &t : args_map) {
    if (t.first == "data" || t.first == "softmax_label")
      continue;
#ifdef MYDEBUG
    if (!count) (NDArray)t.second = C;
#endif
    const_cast<NDArray&>(t.second).WaitToWrite();
    t.second.WaitToRead();
    args.push_back(t.second.GetHandle());
    keys.push_back("arg:" + t.first);
  }
  for (const auto &t : aux_map) {
    if (t.first == "data" || t.first == "softmax_label")
      continue;
#ifdef MYDEBUG
    if (!count) (NDArray)t.second = C;
#endif
    const_cast<NDArray&>(t.second).WaitToWrite();
    t.second.WaitToRead();
    args.push_back(t.second.GetHandle());
    keys.push_back("aux:" + t.first);
  }

  const char * c_keys[args.size()];
  for (size_t i = 0; i < args.size(); i++) {
    c_keys[i] = keys[i].c_str();  // these stay valid since keys[i] won't be modified or destroyed
  }
#ifdef MYDEBUG
  count++;
#endif

  CHECK_EQ(MXNDArraySave(param_path, args.size(), args.data(), c_keys), 0);
  std::cerr << "Done saving the model parameters." << std::endl;
}

std::vector<float> ImageTrain::train(float * data, float * label) {
  return execute(data, label, true);
}

std::vector<float> ImageTrain::predict(float * data, float * label) {
  std::cout << "This has been deprecated." << std::endl;
  return execute(data, label, false);
}

std::vector<float> ImageTrain::predict(float * data) {
  return execute(data, NULL, false);
}

std::vector<float> ImageTrain::execute(float * data, float * label, bool is_train) {
  if (!is_built) {
    std::cerr << "Network hasn't been built. "
        << "Please run buildNet() or loadModel() first." << std::endl;
    exit(0);
  }

  NDArray data_n = NDArray(data, shape, ctx_dev);
  data_n.CopyTo(&args_map["data"]);

  NDArray label_n;
  if (is_train) {
    label_n = NDArray(label, Shape(batch_size), ctx_dev);
    label_n.CopyTo(&args_map["softmax_label"]);
  }

  NDArray::WaitAll();

  exec->Forward(is_train);
  // train or predict?
  if (is_train) {
    exec->Backward();
    exec->UpdateAll(opt.get(), learning_rate, weight_decay);
  }

  NDArray::WaitAll();

  // for debugging
  if (is_train && false) {
    Accuracy train_acc;
    train_acc.Update(label_n, exec->outputs[0]);
    std::cerr << "Training Accuracy for this mini-batch: " << train_acc.Get() << std::endl;
  }

  // get probs for prediction
  exec->outputs[0].SyncCopyToCPU(&preds, batch_size * num_classes);

  return preds;
}

std::vector<float> ImageTrain::loadMeanImage(const char * fname) {
  mx_uint out_size, out_name_size;
  NDArrayHandle *out_arr;
  const char **out_names;
  CHECK_EQ(MXNDArrayLoad(fname, &out_size, &out_arr, &out_name_size,
                         &out_names), 0);
  CHECK_EQ(out_name_size, out_size);
  mxnet::cpp::NDArray nd_res = mxnet::cpp::NDArray(out_arr[0]);
  size_t nd_size = nd_res.Size();
  std::vector<float> res(nd_size);
  nd_res.SyncCopyToCPU(&res, nd_size);
  return res;
}

std::vector<float> ImageTrain::extractLayer(float * data, const char * output_key) {

  // find the output symbol
  Symbol net = mxnet_sym.GetInternals();
  bool found = false;
  for(const auto & layer_name:net.ListOutputs()){
    //std::cerr << layer_name << std::endl;
    if (strcmp(layer_name.c_str(), output_key)==0) {
      found = true;
      break;
    }
  }
  if (!found) {
    std::cerr << "Layer " << output_key << " not found. These are the layers available:\n";
    for(const auto & layer_name:net.ListOutputs()){
      std::cerr << layer_name << std::endl;
    }
    return std::vector<float>(0);
  }

  // extract the output layer
  Symbol output_layer = mxnet_sym.GetInternals()[output_key];

  // update the executor to use the new args_map and aux_map
  exec = std::unique_ptr<Executor>(output_layer.SimpleBind(ctx_dev,
        args_map,
        std::map<std::string, NDArray>(),
        std::map<std::string, OpReqType>(),
        aux_map));

  // forward propagate the input
  NDArray data_n = NDArray(data, shape, ctx_dev);
  data_n.CopyTo(&args_map["data"]);
  exec->Forward(false);

  // extract the output of this executor (i.e., the requested symbol)
  NDArray::WaitAll();
  std::vector<float> res;
  exec->outputs[0].SyncCopyToCPU(&res);
  return res;
}

const char * ImageTrain::listAllLayers() {
  std::string res;
  Symbol net = mxnet_sym.GetInternals();
  for(const auto & layer_name:net.ListOutputs()){
    res += layer_name + "\n";
  }
  return strdup(res.c_str()); // this leaks, but at least doesn't corrupt
}

