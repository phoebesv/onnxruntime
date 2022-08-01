// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/allocation_planner.h"
#include <list>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <ctime>
#include <iomanip>
#include "core/common/exceptions.h"
#include "core/common/inlined_containers.h"
#include "core/platform/env.h"
#include "core/framework/data_types.h"
#include "core/framework/execution_context.h"
#include "core/framework/kernel_def_builder.h"
#include "core/framework/mldata_type_utils.h"
#include "core/framework/op_kernel.h"
#include "core/framework/session_state.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/utils.h"
#include "core/framework/op_kernel_context_internal.h"
#include "core/framework/sequential_executor.h"

using namespace onnxruntime::common;
using namespace ONNX_NAMESPACE;
namespace onnxruntime {

namespace NestedSubgraphInfoDetails {

// Used to compose a unique key to identify a nested subgraph
// relative to a current graph level (which in turn is identified using a "base")
std::string ComposeNestedSubgraphInfoKeyHelper(const std::string& base,
                                               size_t graph_depth,
                                               NodeIndex node_index,
                                               const std::string& attr_name) {
  std::ostringstream ss;

  // key = base + graph depth + current graph node index + attr name corresponding to the subgraph
  ss << base;
  ss << graph_depth;
  ss << node_index;
  ss << attr_name;

  return ss.str();
}

}  // namespace NestedSubgraphInfoDetails

std::ostream& operator<<(std::ostream& out, AllocKind alloc_kind) {
  switch (alloc_kind) {
    case AllocKind::kAllocate:
      out << "Allocate";
      break;
    case AllocKind::kAllocateStatically:
      out << "AllocateStatically";
      break;
    case AllocKind::kPreExisting:
      out << "PreExisting";
      break;
    case AllocKind::kReuse:
      out << "Reuse";
      break;
    case AllocKind::kAllocateOutput:
      out << "AllocateOutput";
      break;
    case AllocKind::kShare:
      out << "Share";
      break;
    case AllocKind::kAllocatedExternally:
      out << "AllocatedExternally";
      break;
    case AllocKind::kNotSet:
      out << "NotSet";
      break;
  }
  return out;
}

// Output details of an execution plan:
std::ostream& operator<<(std::ostream& out, std::pair<const SequentialExecutionPlan*, const SessionState*> planinfo) {
  const SequentialExecutionPlan& plan = *planinfo.first;
  const SessionState& session_state = *planinfo.second;

  const auto& name_idx_map = session_state.GetOrtValueNameIdxMap();
  InlinedHashMap<int, std::string_view> index_to_name;
  index_to_name.reserve(name_idx_map.Size());

  out << "Allocation Plan:\n";
  out << "(ort_value_idx) output_name : <allocation plan>\n";
  auto plan_size = plan.allocation_plan.size();

  for (auto& name_index : name_idx_map) {
    auto index = name_index.second;
    index_to_name[index] = name_index.first;
    out << "(" << index << ") " << name_index.first << " : ";
    if (0 <= index && static_cast<size_t>(index) < plan_size) {
      auto& elt_plan = plan.allocation_plan[index];
      out << elt_plan.alloc_kind;
      if (elt_plan.alloc_kind == AllocKind::kReuse) out << " " << elt_plan.reused_buffer;

      auto& loc = elt_plan.location;
      out << ", " << loc.ToString();
    } else {
      out << "Index out-of-range!";
    }

    out << std::endl;
  }

  out << "\nExecution Plan:\n";
  for (size_t i = 0; i < plan.execution_plan.size(); ++i) {
    auto& execution_plan = plan.execution_plan[i];
    out << " Start logic stream : " << i << "on execution provider: " << execution_plan->ep_->Type() << std::endl;
    for (auto& step : execution_plan->steps_) {
      out << step->Dump() << std::endl;
    }
    out << "End logic stream : " << i << std::endl;
  }

  return out;
}

static const KernelCreateInfo& GetKernelCreateInfo(
    const KernelCreateInfoMap& kernel_create_info_map,
    NodeIndex node_index) {
  auto entry = kernel_create_info_map.find(node_index);
  ORT_ENFORCE(entry != kernel_create_info_map.cend(),
              "SessionState should have saved the KernelCreateInfo prior to this running. NodeIndex:", node_index);

  return *entry->second;
}

class BarrierStep : public SequentialExecutionPlan::ExecutionStep {
public:
  BarrierStep(size_t id) : SequentialExecutionPlan::ExecutionStep(),
      barrier_id{id},
      func_([](size_t barrier_idx, void* ctx, size_t /*stream_idx*/, bool& continue_flag) {
        ExecutionContext* execution_context = reinterpret_cast<ExecutionContext*>(ctx);
        continue_flag = execution_context->DecCountDownBarrier(barrier_idx);
        return Status::OK();
      }) {
  }

  StepCommandFn GetStepFun() override {
    return std::bind(func_, barrier_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
  }

  std::string Dump() const override {
    std::stringstream ss;
    ss << "Set a barrier with id: " << barrier_id << ", count: " << 2 << ". ";
    return ss.str();
  }

private:
 size_t barrier_id{0};
 std::function<Status(size_t barrier_idx, void* ctx, size_t stream_idx, bool& continue_flag)> func_;
};

class WaitOnEPStep : public SequentialExecutionPlan::ExecutionStep {
 public:
  WaitOnEPStep(WaitNotificationFn handle, NotificationIndex idx) : SequentialExecutionPlan::ExecutionStep(),
                                                                   wait_handle(handle),
                                                                   notification_idx(idx),
                                                                   func_(
                                                                       [](WaitNotificationFn wait_handle,
                                                                          NotificationIndex notification_idx,
                                                                          void* ctx, size_t stream_idx, bool& continue_flag) {
                                                                         ExecutionContext* execution_context = reinterpret_cast<ExecutionContext*>(ctx);
                                                                         wait_handle(*execution_context->GetDeviceStream(stream_idx), *execution_context->GetNotification(notification_idx));
                                                                         // update streams clock status
                                                                         if (execution_context->GetDeviceStream(stream_idx)) {
                                                                           execution_context->GetDeviceStream(stream_idx)->UpdateStreamClock(execution_context->GetNotification(notification_idx)->stream_clock_);
                                                                         }
                                                                         LOGS(execution_context->GetLogger(), INFO) << "stream " << stream_idx << " wait on Notification with id: " << notification_idx;
                                                                         continue_flag = true;
                                                                         return Status::OK();
                                                                       }) {}

  StepCommandFn GetStepFun() override {
    return std::bind(func_, wait_handle, notification_idx, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
  }

  std::string Dump() const override {
    std::stringstream ss;
    ss << "WaitOnEPStep: wait on notification with id: " << notification_idx << ". ";
    return ss.str();
  }

 private:
  WaitNotificationFn wait_handle;
  NotificationIndex notification_idx;
  std::function<Status(WaitNotificationFn wait_handle,
                       NotificationIndex notification_idx,
                       void* ctx, size_t stream_idx, bool& continue_flag)>
      func_;
};

class LaunchKernelStep : public SequentialExecutionPlan::ExecutionStep {
 public:
  LaunchKernelStep(NodeIndex index) : SequentialExecutionPlan::ExecutionStep(),
                        node_index{index},
                        func_([](NodeIndex node_idx, void* ctx, size_t stream_idx, bool& continue_flag) {
                                        auto* execution_context = reinterpret_cast<ExecutionContext*>(ctx);
                                        if (!continue_flag) {
                                          LOGS(execution_context->GetLogger(), WARNING) << "Exiting due to terminate flag being set to true.";
                                          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Exiting due to terminate flag being set to true.");
                                        }
                                        onnxruntime::Status status = ExecuteKernel(*execution_context, node_idx, stream_idx);
                                        continue_flag = status.IsOK();
                                        return status;
                        }) {
  }

  StepCommandFn GetStepFun() override {
    return std::bind(func_, node_index, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
  }

  std::string Dump() const override {
    std::stringstream ss;
    ss << "Launch kernel with node id: " << node_index << ". ";
    return ss.str();
  }

 private:
  NodeIndex node_index{0};
  std::function<Status(NodeIndex index, void* ctx, size_t stream_idx, bool& continue_flag)> func_;
};

class ActivateNotificationStep : public SequentialExecutionPlan::ExecutionStep {
 public:
  ActivateNotificationStep(NotificationIndex notification_index) : SequentialExecutionPlan::ExecutionStep(),
                                      notification_idx(notification_index),
                                      func_([](NotificationIndex notification_index, void* ctx, size_t stream_idx, bool& continue_flag) {
                                        ExecutionContext* execution_context = reinterpret_cast<ExecutionContext*>(ctx);
                                        if (execution_context->GetNotification(notification_index)) {
                                          execution_context->GetNotification(notification_index)->ActivateAndUpdate();
                                        }
                                        LOGS(execution_context->GetLogger(), INFO) << "stream " << stream_idx << " activate notification with index " << notification_index;
                                        continue_flag = true;
                                        return Status::OK();
                                      }) {
  }

  StepCommandFn GetStepFun() override {
    return std::bind(func_, notification_idx, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
  }

  virtual std::string Dump() const override {
    std::stringstream ss;
    ss << "ActivateNotificationStep: activate notification with id: " << notification_idx << ". ";
    return ss.str();
  }

 private:
  NotificationIndex notification_idx;
  std::function<Status(NotificationIndex notification_idx, void* ctx, size_t stream_idx, bool& continue_flag)> func_;
};

class TriggerDownstreamStep : public SequentialExecutionPlan::ExecutionStep {
 public:
  TriggerDownstreamStep(NotificationIndex notification_index) : SequentialExecutionPlan::ExecutionStep(),
                                                                notification_idx(notification_index),
                                                                func_([](NotificationIndex notification_index, void* ctx, size_t /*stream_idx*/, bool& continue_flag) {
                                                                  ExecutionContext* execution_context = reinterpret_cast<ExecutionContext*>(ctx);
                                                                  ScheduleDownstream(*execution_context, notification_index, /*single thread mode*/ false);
                                                                  continue_flag = true;
                                                                  return Status::OK();
                                                                }) {
  }

  StepCommandFn GetStepFun() override {
    return std::bind(func_, notification_idx, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
  }

  virtual std::string Dump() const override {
    std::stringstream ss;
    ss << "TriggerDownstreamStep: trigger downstream of notification: " << notification_idx << ". ";
    return ss.str();
  }

 private:
  NotificationIndex notification_idx;
  std::function<Status(NotificationIndex notification_idx, void* ctx, size_t stream_idx, bool& continue_flag)> func_;
};


class PlannerImpl {
 public:
  PlannerImpl(const Node* parent_node, const onnxruntime::GraphViewer& graph_viewer,
              gsl::span<const NodeArg* const> outer_scope_node_args, const ExecutionProviders& providers,
              const KernelCreateInfoMap& kernel_create_info_map,
              const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps,
              const InlinedHashMap<OrtValueName, OrtMemoryInfo>& outer_scope_node_arg_to_location_map,
              const OrtValueNameIdxMap& ort_value_name_idx_map,
              const ISequentialPlannerContext& context, SequentialExecutionPlan& plan)
      : context_(&context),
        plan_(plan),
        parent_node_(parent_node),
        graph_viewer_(graph_viewer),
        outer_scope_node_args_(outer_scope_node_args),
        execution_providers_(providers),
        kernel_create_info_map_(kernel_create_info_map),
        subgraphs_kernel_create_info_maps_(subgraphs_kernel_create_info_maps),
        outer_scope_node_arg_to_location_map_(outer_scope_node_arg_to_location_map),
        ort_value_name_idx_map_(ort_value_name_idx_map) {}

  Status CreatePlan(const ExecutionProviders& execution_providers,
                    const IStreamCommandHandleRegistry& stream_handle_registry,
                    /*const ProviderStreamMap& provider_stream_map,
                    const OpStreamMap& op_stream_map,*/
                    const std::string& partition_config_file,
                    const logging::Logger& logger);

 private:
  const ISequentialPlannerContext* context_;
  SequentialExecutionPlan& plan_;

  const Node* parent_node_;
  const onnxruntime::GraphViewer& graph_viewer_;
  gsl::span<const NodeArg* const> outer_scope_node_args_;
  const ExecutionProviders& execution_providers_;

  const KernelCreateInfoMap& kernel_create_info_map_;
  const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps_;

  const InlinedHashMap<OrtValueName, OrtMemoryInfo>& outer_scope_node_arg_to_location_map_;

  const OrtValueNameIdxMap& ort_value_name_idx_map_;

  size_t num_logic_streams_{0};
  std::vector<std::vector<NodeIndex>> stream_nodes_;
  std::vector<size_t> node_stream_map_;
  // dependence_graph_ keeps the dependencies combining model graph and logic streams
  // e.g. dependence_graph_[downstream_node] = [upstream_node_0, upstream_node_1, upstream_node_2 ...]
  // upstream_node_0 and upstream_node_1 are the immmediate upstream nodes of downstream_node
  // upstream_node_2 is the immediate nodes ahead of downstream_node is the same logic stream
  InlinedHashMap<onnxruntime::NodeIndex, InlinedHashSet<onnxruntime::NodeIndex>> dependence_graph_;
  std::unordered_map<onnxruntime::OrtValueIndex, std::unordered_set<onnxruntime::NodeIndex>> value_consumer_map_;
  std::unordered_map<onnxruntime::OrtValueIndex, onnxruntime::NodeIndex> value_node_map_;

  // OrtValueInfo: Auxiliary information about an OrtValue used only during plan-generation:
  struct OrtValueInfo {
    const onnxruntime::NodeArg* p_def_site;  // the (unique) NodeArg corresponding to the MLValue
    int usecount = 0;                        // static reference-count

    // This is initialized to -1 to ensure that if ProcessDef is somehow not called, planning
    // will fail more cleanly.  This is also used as a temporary workaround to detect the
    // case that the DML provider has removed initilizers from the graph during partitioning.
    // Removing initializers is a temporary measure needed to limit the number of copies of
    // tensors in GPU memory.
    OrtValueIndex reused_buffer_index = -1;  // index of original buffer to reuse
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    OrtValueIndex inplace_reused_buffer_index = -1;  // index of original buffer to reuse inplace
#endif
  };

  // ort_value_info_ is indexed by an OrtValueIndex
  std::vector<OrtValueInfo> ort_value_info_;

  // FreeBufferInfo is used to track information about ml-values whose buffers are
  // free to be reused.
  struct FreeBufferInfo {
    OrtValueIndex ml_value;
    // deallocate_point is an index into the execution-plan; thus, ml_value becomes free after
    // this step in the execution-plan is completed.
    size_t deallocate_point;
    FreeBufferInfo(OrtValueIndex ort_value, size_t dealloc_point)
        : ml_value(ort_value), deallocate_point(dealloc_point) {}
  };
  // freelist_ : a list of ml-values whose buffers are free to be reused, sorted by when
  // they became free (more recently freed earlier in the list).
  std::list<FreeBufferInfo> freelist_;

  OrtValueIndex Index(const OrtValueName& name) {
    OrtValueIndex result;
    auto status = ort_value_name_idx_map_.GetIdx(name, result);
    ORT_ENFORCE(status.IsOK(), status.ErrorMessage());
    return result;
  }

  int& UseCount(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size(), "invalid value index: ", n, " against size ", ort_value_info_.size());
    return ort_value_info_[n].usecount;
  }
  int& UseCount(const OrtValueName& name) { return UseCount(Index(name)); }

  int DecrementUseCount(OrtValueIndex n) {
    int& use_count = --UseCount(n);
    assert(use_count >= 0);
    return use_count;
  }

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  OrtValueIndex& InplaceBuffer(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].inplace_reused_buffer_index;
  }
#endif

  OrtValueIndex& Buffer(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].reused_buffer_index;
  }

  AllocPlanPerValue& AllocPlan(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < plan_.allocation_plan.size());
    return plan_.allocation_plan[static_cast<size_t>(n)];
  }

  AllocPlanPerValue& AllocPlan(const OrtValueName& name) { return AllocPlan(Index(name)); }

  // Initialize state for a given ml-value at its definition site:
  void ProcessDef(OrtValueIndex id, const onnxruntime::NodeArg* p_def_site) {
    ORT_ENFORCE(id >= 0 && static_cast<size_t>(id) < ort_value_info_.size());
    OrtValueInfo& info = ort_value_info_[id];
    info.usecount = 0;
    info.reused_buffer_index = id;  // initially, no reuse; the ml-value uses its own buffer
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    info.inplace_reused_buffer_index = id;  // initially, no reuse; the ml-value uses its own buffer
#endif

    info.p_def_site = p_def_site;
  }

  // Reuse/Alias/Share between two OrtValue indexes
  void Reuse(OrtValueIndex reused, OrtValueIndex reused_for, AllocKind alloc_kind) {
    ORT_ENFORCE(reused != reused_for);
    // find original buffer underlying ml-value we want to reuse:
    OrtValueIndex original = Buffer(reused);
    // record that the new buffer will reuse that original buffer
    Buffer(reused_for) = original;
    // adjust original buffer's usecount
    UseCount(original) += UseCount(reused_for);

    // update allocation plan (for use at execution-time)
    auto& symplan = AllocPlan(reused_for);
    symplan.alloc_kind = alloc_kind;
    symplan.reused_buffer = original;
  }

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  void InplaceReuse(OrtValueIndex reused, OrtValueIndex reused_for) {
    ORT_ENFORCE(reused != reused_for);
    OrtValueIndex original = InplaceBuffer(reused);
    InplaceBuffer(reused_for) = original;
    AllocPlan(reused_for).inplace_reuse = original;
  }
#endif

  // Find if there exists some input tensor that we can use in-place for output_arg_num-th input in the node.
  bool FindReusableInput(const onnxruntime::Node& node, int output_arg_num, OrtValueIndex* reusable_input,
                         bool* is_strided_tensor) {
    *is_strided_tensor = false;
#ifdef ENABLE_TRAINING
    // Inputs of Yields are essentially the outputs for FW partial subgraph
    // Thses tensors will be pass back to pytorch, thus cannot share the buffer with other tensors

    // Unhandled corner case:
    // If FW output tensor is consumed by BW graph, and pytorch performs an inplace operation on th returned tensor,
    // we will run into a buffer corruption problem.
    // One potential fix is returning a copy of output tensor, if it has downstream dependency
    auto p_next_node = node.OutputNodesBegin();
    if (p_next_node != node.OutputNodesEnd() && p_next_node->OpType() == "YieldOp") {
      return false;
    }
#endif  // ENABLE_TRAINING

    auto p_output_arg = node.OutputDefs()[output_arg_num];
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());

    if (ci.kernel_def == nullptr) {
      return false;
    }

    const auto& alias_map = ci.kernel_def->Alias();
    auto input_args = node.InputDefs();
    for (auto& pair : alias_map) {
      if (pair.second == output_arg_num) {
        // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for reshape)
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            *reusable_input = Index(p_input_arg->Name());
            return true;
          }
        }
      }
    }

    const auto& variadic_alias_offsets = ci.kernel_def->VariadicAlias();
    if (variadic_alias_offsets.has_value()) {
      int input_offset = variadic_alias_offsets->first;
      int output_offset = variadic_alias_offsets->second;
      // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for AllReduce)
      int alias_input_index = output_arg_num - output_offset + input_offset;
      if (alias_input_index >= 0 && static_cast<size_t>(alias_input_index) < input_args.size()) {
        auto p_input_arg = input_args[alias_input_index];
        if (p_input_arg->Exists()) {
          *reusable_input = Index(p_input_arg->Name());
          return true;
        }
      }
    }

    const auto& inplace_map = ci.kernel_def->MayInplace();
    for (auto& pair : inplace_map) {
      if (pair.second == output_arg_num) {
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            auto input_arg_index = Index(p_input_arg->Name());
            auto original = Buffer(input_arg_index);
            if (1 == UseCount(original)) {
              if (SameSize(*p_input_arg, *p_output_arg)) {
                // we can reuse this input since it is its last use and permitted for in-place update
                *reusable_input = input_arg_index;  // or original; both should be okay
                return true;
              }
            }
          }
        }
      }
    }

#ifdef ENABLE_TRAINING
    // If any output of the kernel can support strided tensor, and all its consumers' inputs also support
    // strided tensors at the corresponding position, this output will generate a strided tensor
    // and share the data from the corresponding input specified in MayStridedOutputsMap.
    const auto& may_strided_outputs_map = ci.kernel_def->MayStridedOutput();
    for (auto& pair : may_strided_outputs_map) {
      if (pair.second == output_arg_num && pair.first >= 0 && static_cast<size_t>(pair.first) < input_args.size() &&
          input_args[pair.first]->Exists()) {
        bool can_strided = true;
        for (auto it = node.OutputNodesBegin(); it != node.OutputNodesEnd(); ++it) {
          const KernelCreateInfo& output_node_ci = GetKernelCreateInfo(kernel_create_info_map_, it->Index());
          if (!output_node_ci.kernel_def) {
            can_strided = false;
            break;
          }
          const auto& may_strided_inputs = output_node_ci.kernel_def->MayStridedInput();
          for (size_t i = 0; i < it->InputDefs().size(); ++i) {
            if (it->InputDefs()[i] == p_output_arg && std::find(may_strided_inputs.begin(), may_strided_inputs.end(),
                                                                static_cast<int>(i)) == may_strided_inputs.end()) {
              can_strided = false;
              break;
            }
          }
          if (!can_strided) {
            break;
          }
        }
        if (can_strided) {
          *reusable_input = Index(input_args[pair.first]->Name());
          *is_strided_tensor = true;
          return true;
        }
      }
    }
#endif

    return false;
  }

  static bool SameShape(const TensorShapeProto& shape1, const TensorShapeProto& shape2) {
    // TODO: This should probably be defined to be the equality operator on TensorShapeProto.
    namespace on = ONNX_NAMESPACE;
    int rank1 = shape1.dim_size();
    if (shape2.dim_size() != rank1) return false;
    for (int i = 0; i < rank1; i++) {
      const auto& val1 = shape1.dim(i);
      const auto& val2 = shape2.dim(i);
      if (utils::HasDimValue(val1) && utils::HasDimValue(val2) &&
          (val1.dim_value() == val2.dim_value()))
        continue;  // same known dimension
      if (utils::HasDimParam(val1) && utils::HasDimParam(val2)) {
        const auto& val1_param = val1.dim_param();
        if (val1_param == val2.dim_param() && !val1_param.empty())
          continue;  // same unknown dimension
      }
      return false;
    }
    return true;
  }

  /*! \brief Given a tensor-type, return the size of an element of the tensor.
   */
  static size_t GetElementSize(const DataType& tensor_type) {
    const TypeProto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(tensor_type);
    MLDataType ml_data_type = DataTypeImpl::TypeFromProto(type_proto);
    const TensorTypeBase* tensor_type_base = ml_data_type->AsTensorType();
    ORT_ENFORCE(nullptr != tensor_type_base);
    MLDataType elt_type = tensor_type_base->GetElementType();
    return elt_type->Size();
  }

  static bool SameSize(const TensorShapeProto& shape1, const onnxruntime::NodeArg& arg1,
                       const TensorShapeProto& shape2, const onnxruntime::NodeArg& arg2) {
    const auto& ptype1 = arg1.Type();
    const auto& ptype2 = arg2.Type();
    auto type1_size = GetElementSize(ptype1);
    auto type2_size = GetElementSize(ptype2);
    bool is_type1_string = arg1.TypeAsProto()->tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto_DataType_STRING;
    bool is_type2_string = arg2.TypeAsProto()->tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto_DataType_STRING;

    // sizeof(std::string) = sizeof(double) on gcc 4.8.x on CentOS. This causes the allocation planner to reuse
    // a tensor of type double. This won't work for string tensors since they need to be placement new'ed.
    // If either of the tensors is a string, don't treat them the same. Moreover, reusing a string tensor for a string
    // tensor without releasing the previous memory can cause memory leaks; hence we don't allow reuse across string
    // tensors as well.
    return !(is_type1_string || is_type2_string) && (type1_size == type2_size) && SameShape(shape1, shape2);

    /* TODO: we can improve this if the concrete shapes are known for both as below.
       Unclear whether this is worthwhile though.
    if (KnownSize(p_shape1) && KnownSize(p_shape2)) {
      // Comparison of statically-known size
      auto size1 = NumElements(p_shape1) * EltSize(ptype1);
      auto size2 = NumElements(p_shape2) * EltSize(ptype2);
      return size1 == size2;
    } else {
      // Comparison of statically-unknown size buffers
      return SameElementSize(ptype1, ptype2) && SameShape(shape1, shape2);
    }
    */
  }

  bool SameSize(const onnxruntime::NodeArg& arg1, const onnxruntime::NodeArg& arg2) {
    if ((!arg1.Exists()) || (!arg2.Exists())) return false;
    auto p_shape1 = context_->GetShape(arg1);
    auto p_shape2 = context_->GetShape(arg2);
    // If the shapes are unknown, we conservatively assume they may be of different size.
    if ((nullptr == p_shape1) || (nullptr == p_shape2)) return false;
    return SameSize(*p_shape1, arg1, *p_shape2, arg2);
  }

  // Find if freelist contains a buffer of the same size as output_arg
  bool FindReusableTensor(const onnxruntime::NodeArg& output_arg, OrtValueIndex* reusable_tensor) {
    if (!context_->GetEnableMemoryReuse()) {
      return false;
    }
    auto p_required_buffer_shape = context_->GetShape(output_arg);
    if (nullptr == p_required_buffer_shape || p_required_buffer_shape->dim_size() == 0) return false;
    auto& required_memory_info = AllocPlan(output_arg.Name()).location;

    for (auto it = freelist_.begin(); it != freelist_.end(); ++it) {
      size_t reusable = static_cast<size_t>(it->ml_value);
      const onnxruntime::NodeArg* p_node_arg = ort_value_info_.at(reusable).p_def_site;
      if (!p_node_arg) {
        // TODO this should be an error case, needs more investigation
        continue;
      }

#if !defined(DISABLE_OPTIONAL_TYPE)
      // Make sure optional types are not up for re-use as we aren't quite
      // sure if the re-used tensor will be a None or otherwise. This cannot
      // be determined statically.
      if (IsOptionalType(*p_node_arg)) {
        continue;
      }
#endif

      auto& available_memory_info = AllocPlan(p_node_arg->Name()).location;
      if (!(available_memory_info == required_memory_info)) continue;
      auto p_available_buffer_shape = context_->GetShape(*p_node_arg);
      if (nullptr != p_available_buffer_shape) {
        if (SameSize(*p_available_buffer_shape, *p_node_arg,
                     *p_required_buffer_shape, output_arg)) {
          *reusable_tensor = it->ml_value;
          freelist_.erase(it);
          return true;
        }
      }
    }
    return false;
  }

  void Initialize(size_t /*num_graph_nodes*/, size_t num_ml_values) {
    // All ml-value indices must be in range 0 .. num_ml_values-1
    ort_value_info_.resize(num_ml_values);

    // Initialize execution plan:
    plan_.execution_plan.reserve(num_logic_streams_);

    // Initialize allocation plan:
    plan_.allocation_plan.resize(num_ml_values);
  }

  bool HasExternalOutputs(const Node& node) const {
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());
    if (ci.kernel_def == nullptr) {
      return false;
    }

    return ci.kernel_def->HasExternalOutputs();
  }

  Status ComputePlanForInputsAndWeights() {
    auto setup_preexisting = [this](const NodeArg* node_arg) {
      auto input_index = Index(node_arg->Name());
      AllocPlanPerValue& thisplan = AllocPlan(input_index);
      thisplan.alloc_kind = AllocKind::kPreExisting;
      thisplan.value_type = utils::GetMLDataType(*node_arg);
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
      size_t max_pc = plan_.execution_plan.size();
      thisplan.life_interval = std::pair<size_t, size_t>(0, max_pc);
#endif
    };

    // inputs of the graph:
    // An input ml-value's data is owned by the caller (of InferenceSession::Run())
    // It must be allocated by the caller, and will not be reused during inference.
    for (auto graph_input : graph_viewer_.GetInputs()) {
      setup_preexisting(graph_input);
    }

    // outer scope node args are treated the same as graph inputs
    for (auto outer_scope_node_arg : outer_scope_node_args_) {
      setup_preexisting(outer_scope_node_arg);
    }

    // set AllocationInfo for each weight
    return GeneratePlanForWeights();
  }

  Status ComputeReuseCount() {
    // Note: for every ml-value, its definition must appear before all its uses in a topological sort of a valid model
    using GraphInputsSet = InlinedHashSet<std::string_view>;
    const auto& graph_inputs_nodes = graph_viewer_.GetInputsIncludingInitializers();
    GraphInputsSet graph_inputs;
    graph_inputs.reserve(graph_inputs_nodes.size());
    for (auto& graph_input : graph_inputs_nodes) {
      graph_inputs.insert(graph_input->Name());
    }

    for (auto graph_input : graph_viewer_.GetInputs()) {
      OrtValueIndex index = Index(graph_input->Name());
      UseCount(index)++;  // Models caller's usage post-inference; ensures it will not be reused.
    }

    for (auto node_arg : outer_scope_node_args_) {
      OrtValueIndex index = Index(node_arg->Name());
      UseCount(index)++;  // ensure will not be re-used as this graph does not own the buffer
    }

    // All initializers should be treated as input
    for (const auto& pair : graph_viewer_.GetAllInitializedTensors()) {
      const auto& initializer_name = pair.first;
      UseCount(initializer_name)++;
    }

    for (auto& stream_execution_order : stream_nodes_) {
      for (NodeIndex node_index : stream_execution_order) {
        auto pnode = graph_viewer_.GetNode(node_index);
        if (pnode == nullptr) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", node_index);
        }

        auto process_input = [this](const NodeArg& input, size_t /*arg_idx*/) {
          const auto& name = input.Name();
          UseCount(name)++;
          return Status::OK();
        };

        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->InputDefs(), process_input));

        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->ImplicitInputDefs(), process_input));

        auto outputs = pnode->OutputDefs();
        auto num_outputs = outputs.size();
        bool has_external_outputs = HasExternalOutputs(*pnode);
        for (size_t i = 0; i < num_outputs; ++i) {
          auto* node_output = outputs[i];
          if (!node_output->Exists()) continue;
          OrtValueIndex index = Index(node_output->Name());
          // Ensures external outputs will not be reused.
          UseCount(index) += (has_external_outputs ? 2 : 1);
        }
      }
    }

    for (auto graph_output : graph_viewer_.GetOutputs()) {
      UseCount(graph_output->Name())++;  // Models caller's usage post-inference; ensures it will not be reused.
    }
    return Status::OK();
  }

  Status ComputeValueLocation() {
    // Note: for every ml-value, its definition must appear before all its uses in a topological sort of a valid model
    using GraphInputsSet = InlinedHashSet<std::string_view>;
    const auto& graph_inputs_nodes = graph_viewer_.GetInputsIncludingInitializers();
    GraphInputsSet graph_inputs;
    graph_inputs.reserve(graph_inputs_nodes.size());
    for (auto& graph_input : graph_inputs_nodes) {
      graph_inputs.insert(graph_input->Name());
    }

    for (auto graph_input : graph_viewer_.GetInputs()) {
      OrtValueIndex index = Index(graph_input->Name());
      ProcessDef(index, graph_input);
    }

    for (auto node_arg : outer_scope_node_args_) {
      OrtValueIndex index = Index(node_arg->Name());
      ProcessDef(index, node_arg);
    }

    // All initializers should be treated as input
    for (const auto& pair : graph_viewer_.GetAllInitializedTensors()) {
      const auto& initializer_name = pair.first;
      OrtValueIndex index = Index(initializer_name);
      ProcessDef(index, graph_viewer_.GetNodeArg(pair.first));
    }

    InlinedHashSet<OrtValueIndex> set_node_arg_has_explicit_consumer;

    InlinedHashMap<OrtValueIndex, const IExecutionProvider*> map_implicitly_consumed_node_arg_to_ep;
    InlinedHashSet<OrtValueIndex> set_implicitly_consumed_node_arg_has_heterogenous_ep_consumers;

    for (auto& stream_execution_order : stream_nodes_) {
      for (NodeIndex node_index : stream_execution_order) {
        auto pnode = graph_viewer_.GetNode(node_index);
        if (pnode == nullptr) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", node_index);
        }

        // Identify where each output of this node should be allocated.
        // This is determined by the OpKernel bound to the node.
        const KernelCreateInfo& kernel_create_info = GetKernelCreateInfo(kernel_create_info_map_, pnode->Index());

        const auto* p_kernel_def = kernel_create_info.kernel_def.get();

        ORT_ENFORCE(p_kernel_def, "Should not have entry in kernel create info with nullptr for kernel_def");

        auto exec_provider = execution_providers_.Get(*pnode);
        if (exec_provider == nullptr) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the execution provider ",
                                 pnode->GetExecutionProviderType());
        }

        bool is_implicit_input = false;

        // Add location information if applicable for the provided input def
        auto process_input = [&graph_inputs, &exec_provider, &p_kernel_def, &is_implicit_input,
                              &set_node_arg_has_explicit_consumer,
                              &map_implicitly_consumed_node_arg_to_ep,
                              &set_implicitly_consumed_node_arg_has_heterogenous_ep_consumers,
                              this](const NodeArg& input, size_t arg_idx) {
          const auto& name = input.Name();

          bool is_graph_input = (graph_inputs.find(name) != graph_inputs.cend());
          bool is_outer_scope_arg = std::find_if(outer_scope_node_args_.cbegin(), outer_scope_node_args_.cend(),
                                                 [&name](const NodeArg* value) {
                                                   return value && value->Name() == name;
                                                 }) != outer_scope_node_args_.cend();
          bool is_subgraph = (parent_node_ != nullptr);

          // If it's a graph input or outer scope node arg, set its plan.
          // NOTE: Copy nodes should have already been added if a graph input is fed as input
          // to nodes assigned to different providers.

          if (is_graph_input || is_outer_scope_arg) {
            OrtValueIndex index = Index(name);

            if (!is_implicit_input) {
              OrtMemType mem_type = p_kernel_def->InputMemoryType(arg_idx);
              plan_.SetLocation(static_cast<size_t>(index), exec_provider->GetAllocator(0, mem_type)->Info());
              set_node_arg_has_explicit_consumer.insert(index);
            } else {  // implicit input
              // Only process an implicit input if there are explicit consumers at this graph level
              // If there is an explicit consumer, the location MUST be where it is consumed
              // and not where it is located in the outer scope.
              // It is okay if we process a node consuming this arg as an implicit input
              // ahead of a node that is an explicit consumer, because we will just reset
              // this location in the 'if' branch above.

              // CASE 1: We see an implicit input without explicit consumers in a subgraph (pass-through subgraph inputs),
              // then set its location to be its corresponding location in the outer scope.
              // This is so that the subgraph copying mechanism doesn't trigger an unnecessary copy and any copying
              // decisions are deferred till there is an explicit consumer of the subgraph input in nested subgraphs.
              if (is_subgraph && set_node_arg_has_explicit_consumer.count(index) == 0) {
                auto iter = outer_scope_node_arg_to_location_map_.find(name);
                bool found_in_outer_scope_location_map = (iter != outer_scope_node_arg_to_location_map_.end());

                if (!is_graph_input) {
                  // Failing this enforce for an implicit subgraph input points to an internal error somewhere.
                  // For certain older opsets (Scan-8), we may not have added explicit subgraph inputs
                  // to the outer scope location map. See explanation in IsNodeWhereNodeInputsAreSameAsExplicitSubgraphInputs()
                  // called in FinalizeSessionStateImpl() in SessionState.
                  ORT_ENFORCE(found_in_outer_scope_location_map,
                              "There is no location for this node arg in the outer scope location map");
                }

                if (found_in_outer_scope_location_map) {
                  plan_.SetLocation(static_cast<size_t>(index), iter->second);
                }
              } else if (set_node_arg_has_explicit_consumer.count(index) == 0) {
                // CASE 2: We see an implicit input without explicit consumers in the main graph,
                // then set its location to be the device corresponding to the EP that the subgraph
                // holding node has been partitioned to.

                // The "ideal" solution is to set the location of its first "explicit" usage which may occur
                // in any nested subgraph of the node, but that is potentially too costly to
                // get at this stage (TODO: Investigate feasibility of this, see TODO in FinalizeSessionStateImpl() around this)

                // Instead, we take a "less than ideal" route which is to set the location to be the device
                // corresponding to the EP that the node is partitioned to. The hypothesis is that it is "most likely"
                // that the implicit input will eventually be consumed on that device in a nested subgraph.

                // The previous behavior was to default to CPU which will cause unnecessary copies when
                // (1) The user invokes Run() with an OrtValue backed by non-CPU memory (eg CUDA) and
                // the node in the subgraph that consumes the subgraph's implicit input is on a non-CPU device
                // in the subgraph
                // (2) The user tries to IOBind implicitly consumed graph inputs (GH Issue 11254) and
                // the node in the subgraph that consumes the subgraph's implicit input is on
                // a non-CPU device in the subgraph

                // Even if the user provides an input on CPU and the node in the subgraph that consumes the subgraph's
                // implicit input is on a non-CPU device, instead of the subgraph copying mechanism taking it to the device,
                // all we will do is "front-load" this copy in utils::CopyInputsAcrossDevices() with this approach.

                // NOTE 1: The only case this will be sub-optimal is when a node containing a subgraph is partitioned to a
                // non-CPU EP and the user provides an input (or tries to IOBind the input) AND it will eventually be
                // explicitly consumed on CPU - this scenario should be very rare and we forgo performance in this case
                // (the subgraph copying mechanism will make the copy to CPU eventually) in favor of optimizing for the
                // common case (which is that we expect the implicit input to be consumed on the non-CPU device corresponding
                // to the non-CPU EP).

                // NOTE 2: If the implicit input is consumed by multiple nodes (as implicit inputs in all of them) and
                // all of them are partitioned to the same EP, then we go ahead with the above stated logic.
                // If there are multiple EPs involved, we default the location to just CPU as there is ambiguity involved
                // as to which non-CPU device is "most optimal" for the implicit input.

                if (set_implicitly_consumed_node_arg_has_heterogenous_ep_consumers.count(index) == 0) {
                  auto already_seen_ep_for_node_arg = map_implicitly_consumed_node_arg_to_ep.find(index);

                  if (already_seen_ep_for_node_arg == map_implicitly_consumed_node_arg_to_ep.end()) {
                    // First time we are encountering this implicitly consumed input at this graph level (or)
                    plan_.SetLocation(static_cast<size_t>(index), exec_provider->GetAllocator(0, OrtMemType::OrtMemTypeDefault)->Info());
                    map_implicitly_consumed_node_arg_to_ep.insert({index, exec_provider});
                  } else if (already_seen_ep_for_node_arg->second == exec_provider) {
                    // The EP that we previously seen for this implicit input is the same one as the current EP
                    // we have seen
                    plan_.SetLocation(static_cast<size_t>(index), exec_provider->GetAllocator(0, OrtMemType::OrtMemTypeDefault)->Info());
                  } else {
                    // Default the location to CPU
                    plan_.SetLocation(static_cast<size_t>(index),
                                      execution_providers_.Get(CPU)->GetAllocator(0, OrtMemType::OrtMemTypeDefault)->Info());
                    set_implicitly_consumed_node_arg_has_heterogenous_ep_consumers.insert(index);
                  }
                }
              }
            }
          }

          return Status::OK();
        };

        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->InputDefs(), process_input));

        is_implicit_input = true;
        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->ImplicitInputDefs(), process_input));

        auto outputs = pnode->OutputDefs();
        auto num_outputs = outputs.size();
        //bool has_external_outputs = HasExternalOutputs(*pnode);
        for (size_t i = 0; i < num_outputs; ++i) {
          auto* node_output = outputs[i];
          if (!node_output->Exists()) continue;
          OrtValueIndex index = Index(node_output->Name());
          ProcessDef(index, node_output);
          auto allocator = exec_provider->GetAllocator(0, p_kernel_def->OutputMemoryType(i));
          ORT_ENFORCE(allocator);
          plan_.SetLocation(static_cast<size_t>(index),
                            allocator->Info());
        }
      }
    }

    return Status::OK();
  }

  OrtMemoryInfo GetLocationForNodeInput(size_t input_index, const Node& node,
                                        const KernelCreateInfoMap& kernel_create_info_map) {
    auto* p_provider = execution_providers_.Get(node);
    ORT_ENFORCE(p_provider);

    const KernelCreateInfo& kernel_create_info = GetKernelCreateInfo(kernel_create_info_map, node.Index());

    if (utils::IsInputOnCpu(node, &kernel_create_info, input_index))
      // weights are not output from any node, so it's OK to put its location on CPU provider
      return execution_providers_.GetDefaultCpuMemoryInfo();
    return p_provider->GetAllocator(0, OrtMemTypeDefault)->Info();
  }

  void GeneratePlanForWeightsHelper(const GraphViewer& graph_viewer,
                                    const InitializedTensorSet& weights,
                                    const KernelCreateInfoMap& kernel_create_info_map,
                                    const std::string& subgraph_kernel_create_info_map_key_base,
                                    size_t graph_depth,
                                    /*out*/ std::vector<std::vector<OrtMemoryInfo>>& locations) {
    // Iterate over nodes in current level firstly to record location of usages
    // in current graph
    for (const auto& node : graph_viewer.Nodes()) {
      const auto& input_node_args = node.InputDefs();
      size_t num_node_inputs = input_node_args.size();

      for (size_t node_input_index = 0; node_input_index < num_node_inputs; ++node_input_index) {
        auto input_node_arg = input_node_args[node_input_index];

        // Skip processing missing optional inputs
        if (!input_node_arg->Exists()) {
          continue;
        }

        auto& def_name = input_node_arg->Name();

        // This node input doesn't correspond to any of the weights
        if (!weights.count(def_name)) {
          continue;
        }

        // While processing subgraphs, if we don't see an entry in the implicit
        // inputs of the node containing the subgraph, it is a shadow value.
        auto is_shadow_value_in_subgraph = [](const Node& subgraph_parent_node,
                                              const std::string& def_name) -> bool {
          bool is_shadow_value_in_subgraph = true;
          for (const auto& implicit_input : subgraph_parent_node.ImplicitInputDefs()) {
            if (implicit_input->Name() == def_name) {
              is_shadow_value_in_subgraph = false;
              break;
            }
          }

          return is_shadow_value_in_subgraph;
        };

        // Skip processing shadow values in subgraphs
        if (graph_depth > 0) {
          // We are processing a subgraph if we enter this
          const auto* parent_node = graph_viewer.ParentNode();

          // Skip processing if it is a shadow value
          if (is_shadow_value_in_subgraph(*parent_node, def_name)) {
            continue;
          }
        }

        auto wt_index = Index(def_name);
        // TODO: Identify error cases where-in an initializer is used on different
        // devices within the same graph level.
        // If we ever encounter that, it means that there is a severe bug in Memcpy
        // transformer and the model will crash while running. The Memcpy transformer
        // is supposed to duplicate initializers being used on different devices within
        // the same graph level and hence we should never see an initializer being used
        // on different devices here.
        // The same initializer being used on different devices across graph levels
        // (subgraphs) is okay and utils::CopyInputsAcrossDevices() will take it to
        // the right device before subgraph execution.
        locations[wt_index].emplace_back(
            GetLocationForNodeInput(node_input_index, node, kernel_create_info_map));
      }
    }

    // Iterate over nodes in current graph with subgraphs and recurse.
    for (const auto& node : graph_viewer.Nodes()) {
      // If the node has subgraphs (i.e.) control flow nodes,
      // walk the nodes in those subgraphs as well to best determine
      // the location for the OrtValue corresponding to the weights
      // (i.e.) do a recursion
      if (node.ContainsSubgraph()) {
        // A node may contain multiple subgraphs - so iterate through all of them
        for (auto& name_to_subgraph : node.GetAttributeNameToSubgraphMap()) {
          GraphViewer subgraph_viewer(*name_to_subgraph.second);

          const auto& local_subgraph_kernel_create_info_map_key =
              NestedSubgraphInfoDetails::ComposeNestedSubgraphInfoKeyHelper(subgraph_kernel_create_info_map_key_base,
                                                                            graph_depth, node.Index(), name_to_subgraph.first);

          auto specific_subgraph_kernel_create_info_map = subgraphs_kernel_create_info_maps_.find(local_subgraph_kernel_create_info_map_key);
          ORT_ENFORCE(specific_subgraph_kernel_create_info_map != subgraphs_kernel_create_info_maps_.end());

          GeneratePlanForWeightsHelper(subgraph_viewer,
                                       weights,
                                       specific_subgraph_kernel_create_info_map->second,
                                       local_subgraph_kernel_create_info_map_key,
                                       graph_depth + 1,
                                       locations);
        }
      }
    }
  }

  Status GeneratePlanForWeights() {
    // TODO: Move away from usage of vector of `OrtMemoryInfo`s per weight (initializer)
    // We do not need to maintain a vector of locations that a weight is used in.
    // We only need to know the location of its first usage according to the nodes
    // iteration rule in GeneratePlanForWeightsHelper() because:
    // (1) If the initializer is used in the graph level it is introduced in, then it can
    // only be used on one device as the Memcpy transformer will duplicate the initializer
    // (with a different name) in case it is used on multiple devices.
    // If the initializer is also additionally used in one of the subgraphs, we rely
    // on the utils::CopyInputsAcrossDevices() to copy it over to the appropriate device
    // before the subgraphs are executed.
    // (2) If the initializer is NOT used in the level it is introduced in and only used
    // in subgraphs, even then knowing its first usage location is enough as it can't be
    // used on different devices within the same graph level (see (1) for reason), and for
    // nested subgraphs, we can rely on the utils::CopyInputsAcrossDevices() to copy it
    // over to the appropriate device before the subgraphs are executed.
    std::vector<std::vector<OrtMemoryInfo>> locations(plan_.allocation_plan.size());

    GeneratePlanForWeightsHelper(graph_viewer_, graph_viewer_.GetAllInitializedTensors(),
                                 kernel_create_info_map_, "", 0, locations);

    for (size_t i = 0; i != locations.size(); ++i) {
      const std::vector<OrtMemoryInfo>& loc = locations[i];
      if (loc.empty()) continue;
      plan_.allocation_plan[i].alloc_kind = AllocKind::kAllocateStatically;
      // The planned location for an initializer is the location of its first usage.
      plan_.allocation_plan[i].location = loc[0];
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
      size_t max_pc = plan_.execution_plan.size();
      std::string node_arg_name;
      ORT_RETURN_IF_ERROR(ort_value_name_idx_map_.GetName(static_cast<int>(i), node_arg_name));
      auto node_arg = graph_viewer_.GetNodeArg(node_arg_name);
      plan_.allocation_plan[i].value_type = utils::GetMLDataType(*node_arg);
      plan_.allocation_plan[i].life_interval = std::pair<size_t, size_t>(0, max_pc);
#endif
    }
    return Status::OK();
  }

  bool IsSingleStream() {
    // if each execution provider instance only have 1 logic stream
    // we can safely reuse the existing memory sharing algorithm
    std::set<std::string> stream_providers_set;
    for (size_t i = 0; i < num_logic_streams_; ++i) {
      auto& stream = stream_nodes_[i];
      if (!stream.empty()) {
        auto& ep_type = plan_.execution_plan[i]->ep_->Type();
        if (stream_providers_set.find(ep_type) != stream_providers_set.end()) {
          return false;
        }
        stream_providers_set.insert(ep_type);
      }
    }
    return true;
  }

  // assume we already have a baseline reuse plan (no memory reuse at all)
  // this funciton will optimize the plan by build reusing consider the stream safety.
  Status OptimizeReusePlanForMultiStream() {
    InlinedHashMap<NodeIndex, int> dependents;
    for (const auto& it : dependence_graph_) {
      for (NodeIndex node_index : it.second) {
        dependents[node_index]++;
      }
    }
    std::deque<NodeIndex> que;
    for (const auto& it : dependence_graph_) {
      if (dependents[it.first] == 0) {
        que.push_back(it.first);
      }
    }

    // fetch_all_dependents will collect all dependent nodes for "node_index"
    std::function<std::set<NodeIndex>(NodeIndex)> fetch_all_dependents = [&](NodeIndex node_index) {
      std::set<NodeIndex> dependents;

      std::function<void(NodeIndex)> dfs = [&](NodeIndex curr) {
        if (dependents.find(curr) == dependents.end()) {
          dependents.insert(curr);
          for (NodeIndex dep : dependence_graph_[curr]) {
            dfs(dep);
          }
        }
      };

      dfs(node_index);
      return dependents;
    };

    // waiting_list keeps all values who want to reuse some upstream values' memory
    std::map<OrtMemoryInfo, std::map<size_t, typename std::map<const onnxruntime::NodeArg* const, std::set<NodeIndex>*>>> waiting_list;

    // for each node, dependents_map keeps all its dependent upstream nodes that are sure to be completed ahead
    std::map<NodeIndex, std::set<NodeIndex>> dependents_map;

    std::map<OrtValueIndex, std::set<OrtValueIndex>> input_output_map;

    std::set<OrtValueIndex> reused;

    const auto& graph_viewer = graph_viewer_;
    const auto& value_map = ort_value_name_idx_map_;
    const auto& kernel_create_info_map = kernel_create_info_map_;
    auto& allocation_plan = plan_.allocation_plan;

    std::function<void(NodeIndex)> TryReuseInput = [&](NodeIndex node_index) {
      auto* node = graph_viewer.GetNode(node_index);

      for (size_t output_arg_num = 0; output_arg_num < node->OutputDefs().size(); output_arg_num++) {
        auto p_output_arg = node->OutputDefs()[output_arg_num];
        OrtValueIndex output_idx_global{};

        if (!value_map.GetIdx(p_output_arg->Name(), output_idx_global).IsOK() ||
            allocation_plan[output_idx_global].alloc_kind != AllocKind::kAllocate) {
          continue;
        }

        auto kci_it = kernel_create_info_map.find(node_index);
        if (kci_it == kernel_create_info_map.end()) {
          continue;
        }

        const KernelCreateInfo& ci = *kci_it->second;
        if (ci.kernel_def == nullptr) {
          continue;
        }

        bool found_reusable = false;
        const auto& alias_map = ci.kernel_def->Alias();
        auto input_args = node->InputDefs();
        for (auto* input_arg : input_args) {
          OrtValueIndex input_idx_global{};
          if (value_map.GetIdx(input_arg->Name(), input_idx_global).IsOK()) {
            input_output_map[input_idx_global].insert(output_idx_global);
          }
        }

        for (auto& pair : alias_map) {
          size_t alias_map_second = (size_t)pair.second;
          if (alias_map_second == output_arg_num) {
            // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for reshape)
            if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
              auto p_input_arg = input_args[pair.first];
              if (p_input_arg->Exists()) {
                OrtValueIndex reusable_input{};
                if (value_map.GetIdx(p_input_arg->Name(), reusable_input).IsOK() &&
                    allocation_plan[reusable_input].alloc_kind == AllocKind::kAllocate) {
                  std::cout << p_input_arg->Name() << " reused by " << p_output_arg->Name() << " as input" << std::endl;
                  allocation_plan[output_idx_global].alloc_kind = AllocKind::kReuse;
                  allocation_plan[output_idx_global].reused_buffer = reusable_input;
                  value_consumer_map_[reusable_input].insert(value_consumer_map_[output_idx_global].begin(),
                                                             value_consumer_map_[output_idx_global].end());
                  reused.insert(reusable_input);
                  found_reusable = true;
                  break;
                }
              }
            }
          }
        }

        if (found_reusable) {
          continue;
        }

        const auto& variadic_alias_offsets = ci.kernel_def->VariadicAlias();
        if (variadic_alias_offsets.has_value()) {
          int input_offset = variadic_alias_offsets->first;
          int output_offset = variadic_alias_offsets->second;
          size_t alias_input_index = output_arg_num - output_offset + input_offset;

          if (alias_input_index < input_args.size()) {
            auto p_input_arg = input_args[alias_input_index];

            if (p_input_arg->Exists()) {
              OrtValueIndex reusable_input{};
              if (value_map.GetIdx(p_input_arg->Name(), reusable_input).IsOK() &&
                  allocation_plan[reusable_input].alloc_kind == AllocKind::kAllocate) {
                // LOGS(const_cast<SessionState&>(impl_->session_state_).Logger(), INFO) << p_input_arg->Name() << " reused by " << p_output_arg->Name() << " as input" << std::endl;
                std::cout << p_input_arg->Name() << " reused by " << p_output_arg->Name() << " as input" << std::endl;
                allocation_plan[output_idx_global].alloc_kind = AllocKind::kReuse;
                allocation_plan[output_idx_global].reused_buffer = reusable_input;
                value_consumer_map_[reusable_input].insert(value_consumer_map_[output_idx_global].begin(),
                                                           value_consumer_map_[output_idx_global].end());
                reused.insert(reusable_input);
                continue;
              }  //if
            }    //if
          }
        }

        const auto& inplace_map = ci.kernel_def->MayInplace();
        for (auto& pair : inplace_map) {
          size_t inplace_map_second = (size_t)pair.second;
          if (inplace_map_second == output_arg_num) {
            if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
              auto p_input_arg = input_args[pair.first];
              if (p_input_arg->Exists()) {
                OrtValueIndex input_arg_index{};
                if (value_map.GetIdx(p_input_arg->Name(), input_arg_index).IsOK() &&
                    allocation_plan[input_arg_index].alloc_kind == AllocKind::kAllocate) {
                  if (value_consumer_map_[input_arg_index].size() == 1 && SameSize(*p_input_arg, *p_output_arg)) {
                    std::cout << p_input_arg->Name() << " reused by " << p_output_arg->Name() << " as an input" << std::endl;
                    allocation_plan[output_idx_global].alloc_kind = AllocKind::kReuse;
                    allocation_plan[output_idx_global].reused_buffer = input_arg_index;
                    value_consumer_map_[input_arg_index].insert(value_consumer_map_[output_idx_global].begin(),
                                                                value_consumer_map_[output_idx_global].end());
                    reused.insert(input_arg_index);
                  }
                }
              }
            }
          }
        }
      }
    };  //TryReuseInput

    // go over the outputs of "node_index" and try to reuse its memory
    std::function<void(NodeIndex)> TryReuseOutput = [&](NodeIndex node_index) {
      dependents_map[node_index] = fetch_all_dependents(node_index);
      auto* node = graph_viewer.GetNode(node_index);
      const auto& output_defs = node->OutputDefs();

      for (size_t output_idx_local = 0; output_idx_local < output_defs.size(); ++output_idx_local) {
        const auto& node_output = output_defs[output_idx_local];
        if (!node_output->Exists()) continue;
        OrtValueIndex output_idx_global{};

        if (value_map.GetIdx(node_output->Name(), output_idx_global).IsOK()) {
          if (reused.find(output_idx_global) != reused.end() ||
              allocation_plan[output_idx_global].alloc_kind != AllocKind::kAllocate) {
            continue;  // skip when it is already reused
          }

          const auto* shape = context_->GetShape(*node_output);
          if (!shape) continue;
          size_t size_in_bytes = shape->ByteSizeLong();

          const auto& location = allocation_plan[output_idx_global].location;
          auto local_iter = waiting_list.find(location);
          if (local_iter == waiting_list.end()) {
            waiting_list[location][size_in_bytes][node_output] = &dependents_map[node_index];
            continue;
          }

          auto size_iter = local_iter->second.find(size_in_bytes);
          if (size_iter == local_iter->second.end()) {
            waiting_list[location][size_in_bytes][node_output] = &dependents_map[node_index];
            continue;
          }

          bool get_reused = false;
          for (auto node_iter = size_iter->second.begin(); node_iter != size_iter->second.end();) {
            const onnxruntime::NodeArg* const downstream_arg = node_iter->first;
            OrtValueIndex downstream_value{};

            if (!value_map.GetIdx(downstream_arg->Name(), downstream_value).IsOK()) {
              node_iter = next(node_iter);
              continue;
            }

            // skip if it is a pair of input and output
            if (input_output_map[output_idx_global].find(downstream_value) != input_output_map[output_idx_global].end()) {
              node_iter = next(node_iter);
              continue;
            }

            const auto* downstream_shape = context_->GetShape(*downstream_arg);
            //if (!(*downstream_shape == *shape)) {
            //  node_iter = next(node_iter);
            //  continue;
            //}
            if (!SameSize(*downstream_shape, *downstream_arg, *shape, *node_output)) {
              node_iter = next(node_iter);
              continue;
            }

            auto* deps = node_iter->second;

            if (deps->find(node_index) == deps->end()) {
              node_iter = next(node_iter);
              continue;
            }

            bool all_covered = true;
            for (auto consumer : value_consumer_map_[output_idx_global]) {
              if (deps->find(consumer) == deps->end()) {
                all_covered = false;
                break;
              }
            }
            if (all_covered) {
              //LOGS(const_cast<SessionState&>(impl_->session_state_).Logger(), INFO) << node_output->Name() << " reused by " << downstream_arg->Name() << " as remote tensor" << std::endl;
              std::cout << node_output->Name() << " reused by " << downstream_arg->Name() << " as remote tensor" << std::endl;
              allocation_plan[downstream_value].alloc_kind = AllocKind::kReuse;
              allocation_plan[downstream_value].reused_buffer = output_idx_global;
              get_reused = true;
              // add new consumer for the value to be reused
              value_consumer_map_[output_idx_global].insert(value_node_map_[downstream_value]);
              value_consumer_map_[output_idx_global].insert(value_consumer_map_[downstream_value].begin(),
                                                            value_consumer_map_[downstream_value].end());
              node_iter = size_iter->second.erase(node_iter);
              if (size_iter->second.empty()) {
                local_iter->second.erase(size_iter);
              }
              break;  // only resued once
            } else {
              // dependents not fully covered, cannot reuse, try next one in waiting_list
              node_iter = next(node_iter);
            }
          }  // for
          if (get_reused) {
            reused.insert(output_idx_global);
          } else {
            // if not getting reused, add to waiting
            waiting_list[location][size_in_bytes][node_output] = &dependents_map[node_index];
          }
        }
      }
    };  // TryReuseOutput

    // topological traverse of the dependency graph
    std::unordered_set<NodeIndex> visited;
    while (!que.empty()) {
      NodeIndex node_index = que.front();
      visited.insert(node_index);
      TryReuseInput(node_index);   // try reuse node's inputs as its outputs
      TryReuseOutput(node_index);  // try reuse node's outputs for downstream nodes
      que.pop_front();
      for (NodeIndex next_node_index : dependence_graph_[node_index]) {
        if (--dependents[next_node_index] == 0) {
          que.push_back(next_node_index);
        }
      }
    }
    return Status::OK();
  }

  Status ComputeReusePlan() {
    auto* backup_context = context_;
    ParalllelPlannerContext parallel_context;
    if (!IsSingleStream()) {
      // use parallel execution context to generate a baseline first (no memory sharing)
      context_ = &parallel_context;
    }
    // compute use count first
    ORT_RETURN_IF_ERROR(ComputeReuseCount());
    ORT_RETURN_IF_ERROR(ComputeSingleStreamReusePlan());
    if (IsSingleStream())
      return Status::OK();
    ORT_RETURN_IF_ERROR(OptimizeReusePlanForMultiStream());
    //restore context
    context_ = backup_context;

    return Status::OK();
  }

  // Should only be used after ProcessDef()
  Status ComputeSingleStreamReusePlan() {
    auto& execution_plan = graph_viewer_.GetNodesInTopologicalOrder();
    //copy the use counts to a vector, before computing reuse
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    std::vector<int> ort_value_usecount;
    for (auto ort_value_info : ort_value_info_) {
      ort_value_usecount.push_back(ort_value_info.usecount);
    }
#endif

    // Cached graph outputs.
    const auto& graph_outputs = graph_viewer_.GetOutputs();
    for (size_t program_counter = 0; program_counter < execution_plan.size(); ++program_counter) {
      auto node_index = execution_plan[program_counter];
      // the node (aka operator) which carries the considered program (aka computation).
      const auto* pnode = graph_viewer_.GetNode(node_index);
      // node outputs.
      const auto& output_defs = pnode->OutputDefs();
      // External outputs flag.
      bool has_external_outputs = HasExternalOutputs(*pnode);
      // output_arg_def_index is the index of ArgDefs in pnode's output list.
      // At the i-th iteration, we build the allocation plan for the i-th
      // NodeArg in pnode's output list. Allocation plan remains untouched for
      // optional-missing outputs (aka values with empty names).
      for (size_t output_arg_def_index = 0, end = output_defs.size(); output_arg_def_index < end; ++output_arg_def_index) {
        const auto& node_output = output_defs[output_arg_def_index];
        if (!node_output->Exists()) continue;
        // OrtValue index of the considered output NodeArg.
        const auto current = Index(node_output->Name());
        AllocPlan(current).value_type = utils::GetMLDataType(*node_output);
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
        AllocPlan(current).life_interval.first = program_counter;
#endif
        // Declare OrtValue index of the reused buffer.
        // The the OrtValue indexed by current may reuse the memory in the OrtValue indexed by reused.
        OrtValueIndex reused;
        bool is_strided_tensor = false;
        if (has_external_outputs) {
          ORT_ENFORCE(!IsNonTensor(*node_output), "Only tensors are supported for external outputs for now.");
          AllocPlan(current).alloc_kind = AllocKind::kAllocatedExternally;
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          AllocPlan(current).life_interval.second = execution_plan.size();
#endif
        } else if (std::find(graph_outputs.begin(), graph_outputs.end(), node_output) != graph_outputs.end()) {
          // node_output is graph's output, so we can't reuse intermediate buffer
          AllocPlan(current).alloc_kind = AllocKind::kAllocateOutput;
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          AllocPlan(current).life_interval.second = execution_plan.size();
#endif

          // hacky perf optimization to not copy a pre-existing value to an output if this is a Loop subgraph and
          // the value is not being changed in the subgraph.
          //
          // this usage of a loop state variable has been seen in two scenarios. both have better alternatives now.
          // we maintain the optimization for existing models.
          //
          // 1. a loop state variable was being provided due to ONNX not supporting empty variadic inputs.
          //    a dummy loop state variable was required in this case.
          //    ONNX now supports empty variadic inputs, so a new model should not add a dummy loop state variable.
          //
          // 2. a loop state variable was being used to explicitly pass in an outer scope value to the subgraph.
          //    this sort of usage is automatically handled via implicit inputs and there's no need to add a
          //    loop state variable in order to access the outer scope value.
          if (parent_node_ && pnode->OpType() == "Identity" && parent_node_->OpType() == "Loop") {
            const NodeArg* input = pnode->InputDefs()[0];

            // first input to the Loop subgraph is the iteration number.
            bool input_is_loop_iteration_number = input == graph_viewer_.GetInputs()[0];
            if (input_is_loop_iteration_number) {
              // as the value inside the OrtValue gets changed by the Loop implementation on each iteration
              // (so it can re-use the OrtValue instance) if it is also a subgraph output it must be allocated
              // so a copy of the current value is returned, so leave alloc_kind as kAllocateOutput
            } else {
              const auto& input_name = input->Name();
              const auto input_index = Index(input_name);

              const auto& alloc_plan = AllocPlan(input_index);
              if (alloc_plan.alloc_kind == AllocKind::kPreExisting) {
                Reuse(input_index, current, AllocKind::kShare);
              }
            }
          }
        } else if (!context_->IsParallelExecutionEnabled() &&
                   FindReusableInput(*pnode, static_cast<int>(output_arg_def_index), &reused, &is_strided_tensor)) {
          // Re-using inputs is applicable for tensors, sequence tensors,
          // and optional types if the kernel has marked certain inputs as
          // possible candidates for re-use
          //std::cout << ort_value_info_[reused].p_def_site->Name() << " reused by " << node_output->Name() << " as input" << std::endl;
          //auto* shape = ort_value_info_[reused].p_def_site->Shape();
          //for (int i = 0; i < shape->dim_size(); ++i) {
          //  std::cout << shape->dim().at(i).dim_value() << " ";
          //}
          //std::cout << std::endl;
          Reuse(reused, current, AllocKind::kReuse);
#ifdef ENABLE_TRAINING
          if (is_strided_tensor) AllocPlan(current).is_strided_tensor = true;
#else
          ORT_ENFORCE(!is_strided_tensor, "Strided tensor is not supported in non-training build for now.");
#endif  // ENABLE_TRAINING
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          InplaceReuse(reused, current);
#endif
        } else if (IsNonTensor(*node_output)) {
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
          AllocPlan(current).program_counter.AddStart(program_counter);
        } else if (!context_->IsParallelExecutionEnabled() &&
                   FindReusableTensor(*node_output, &reused)) {
          // Reuse an available (dead) buffer for this output, this is only for sequential execution.
          //std::cout << ort_value_info_[reused].p_def_site->Name() << " reused by " << node_output->Name() << " as remote tensor" << std::endl;
          //auto* shape = ort_value_info_[reused].p_def_site->Shape();
          //for (int i = 0; i < shape->dim_size(); ++i) {
          //  std::cout << shape->dim().at(i).dim_value() << " ";
          //}
          //std::cout << std::endl;
          Reuse(reused, current, AllocKind::kReuse);
          OrtValueIndex original = Buffer(reused);
          if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
            AllocPlan(original).program_counter.AddStart(program_counter);
          }
        } else {
          // otherwise: allocate a new buffer for this output
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
          AllocPlan(current).program_counter.AddStart(program_counter);
        }
      }

      // determine if inputs of *pnode can be freed:
      for (auto node_input : pnode->InputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          // Compute lifetime
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if ((original != -1) && (0 == DecrementUseCount(original))) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }

      for (auto node_input : pnode->ImplicitInputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          // Compute lifetime
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if ((original != -1) && (0 == DecrementUseCount(original))) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }

      // determine if any outputs of *pnode are unused and can be freed:
      for (auto node_output : pnode->OutputDefs()) {
        if (node_output->Exists()) {
          auto& sym = node_output->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if (0 == DecrementUseCount(original)) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }
    }
    return Status::OK();
  }
#ifdef ENABLE_TRAINING
  bool AllocateInputsContiguously(const Node& node) const {
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());
    if (ci.kernel_def == nullptr) {
      return false;
    }

    return ci.kernel_def->AllocateInputsContiguously();
  }

  // Compute allocation order for tensors that are required to be allocated contiguously.
  Status ComputeAllocationOrder() {
    for (auto& stream : stream_nodes_) {
      std::vector<OrtValueIndex>& initializer_allocation_order(plan_.initializer_allocation_order);
      std::vector<OrtValueIndex>& activation_allocation_order(plan_.activation_allocation_order);
      for (auto& step : stream) {
        const auto* pnode = graph_viewer_.GetNode(step);
        if (pnode == nullptr) return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Cannot find the node ", step);
        if (!AllocateInputsContiguously(*pnode)) continue;
        // This node has requested inputs be allocated contiguously.
        const auto& input_defs = pnode->InputDefs();
        onnxruntime::AllocKind input_kind = AllocKind::kAllocateStatically;
        bool set_input_kind = true;
        for (const auto& node_input : input_defs) {
          if (!node_input->Exists()) continue;
          const auto current_idx = Index(node_input->Name());
          const auto& current_plan = AllocPlan(current_idx);
          const auto actual_idx = current_plan.alloc_kind == AllocKind::kReuse ? current_plan.reused_buffer : current_idx;
          const auto& actual_plan = AllocPlan(actual_idx);
          if (set_input_kind) {
            input_kind = actual_plan.alloc_kind;
            set_input_kind = false;
          }

          if ((actual_plan.alloc_kind == AllocKind::kAllocateStatically) && (input_kind != AllocKind::kAllocateStatically))
            return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "AllocateInputsContiguously() requires all inputs to be initializers, or all inputs to be non-initializers.");

          if (actual_plan.alloc_kind == AllocKind::kAllocateStatically) {
            if (std::find(initializer_allocation_order.begin(), initializer_allocation_order.end(), actual_idx) == initializer_allocation_order.end())
              initializer_allocation_order.push_back(actual_idx);
          } else {
            if (std::find(activation_allocation_order.begin(), activation_allocation_order.end(), actual_idx) == activation_allocation_order.end())
              activation_allocation_order.push_back(actual_idx);
          }
        }
    }

    }
    return Status::OK();
  }
#endif

  void VerifyMemoryTimeSchedule() {
    size_t idx = 0;
    for (const auto& entry : plan_.allocation_plan) {
      if (entry.alloc_kind == AllocKind::kAllocate) {
        ORT_ENFORCE(entry.program_counter.HasValidEntries(), "Invalid program_counter entries at index ", idx);
      }

      ++idx;
    }
  }

  // Convert information in execution plan and memory reuse plan into release plan
  Status GenerateDeallocationPlan() {

    // 1. build the consumer list for each value
    std::vector<std::vector<NodeIndex>> value_consumers;
    int num_ml_values = ort_value_name_idx_map_.MaxIdx() + 1;
    value_consumers.resize(num_ml_values);

    // iterate each stream from back, so the first element is the last consumer in single stream case
    for (auto& stream : stream_nodes_) {
      for (auto it = stream.rbegin(), end = stream.rend(); it != end; ++it) {
        NodeIndex node_index = *it;
        auto* node = graph_viewer_.GetNode(node_index);

        auto process_input = [&](const NodeArg& input, size_t /*arg_idx*/) {
          if (input.Exists()) {
            const auto& name = input.Name();
            int value_idx;
            ORT_RETURN_IF_ERROR(ort_value_name_idx_map_.GetIdx(name, value_idx));
            auto origin = Buffer(value_idx);
            if (origin != -1 && plan_.allocation_plan[origin].alloc_kind == AllocKind::kAllocate) {
              // add current node as consumer for origin buffer
              value_consumers[origin].push_back(node_index);
            }
          }
          return Status::OK();
        };

        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(node->InputDefs(), process_input));
        ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(node->ImplicitInputDefs(), process_input));
      }
    }
    //2. build the release actions and fill into node's release list
    auto process_consumer = [&](size_t release_action_idx, NodeIndex node_index) {
      plan_.release_actions[release_action_idx].ref_count++;
      plan_.node_release_list[node_index].push_back(release_action_idx);
    };
    plan_.node_release_list.resize(graph_viewer_.MaxNodeIndex() + 1);
    for (size_t i = 0; i < value_consumers.size(); ++i) {
      if (!value_consumers[i].empty()) {
        plan_.release_actions.push_back(SequentialExecutionPlan::ReleaseAction{i, 0});
        auto release_action_idx = plan_.release_actions.size() - 1;
        // check whether we can static determine where to release.
        // TODO: here we use a temporary simple solution is only static release when all the consumers are on the same stream
        // we actually can do better if all the consumers depends on the last consumer.
        // will optimize it later
        bool is_all_consumer_same_stream = true;
        auto stream_idx = node_stream_map_[value_consumers[i][0]];
        for (size_t j = 1; j < value_consumers[i].size(); ++j) {
          if (node_stream_map_[value_consumers[i][j]] != stream_idx) {
            is_all_consumer_same_stream = false;
            break;
          }
        }
        if (is_all_consumer_same_stream) {
          // all the consumers are on the same stream, so the first element is the last consumer int the stream.
          process_consumer(release_action_idx, value_consumers[i][0]);
        } else {
          //can't static determin, add all the consumers, we will use ref count in release action
          for (auto node_index : value_consumers[i]) {
            process_consumer(release_action_idx, node_index);
          }
        }
      }
    }
    return Status::OK();
  }

  void PartitionIntoStreams(const logging::Logger& logger, const std::string& partition_config_file) {
    auto partitioner = INodePartitioner::CreateNodePartitioner(logger, partition_config_file);
    auto status = partitioner->GetStatus();
    ORT_ENFORCE(status.IsOK(), status.ErrorMessage());
    partitioner->PartitionNodes(graph_viewer_, stream_nodes_);
    node_stream_map_.resize(graph_viewer_.MaxNodeIndex() + 1);
    //int node_cnt = 0;
    for (size_t i = 0; i < stream_nodes_.size(); ++i) {
      for (auto node_index : stream_nodes_[i]) {
        node_stream_map_[node_index] = i;
        //node_cnt++;
      }
    }
    //std::cout << "total node partitioned: " << node_cnt << std::endl;
    num_logic_streams_ = stream_nodes_.size();
  }

  // build each logic streams
  Status BuildExecutionPlan(const ExecutionProviders& execution_providers,
                            const IStreamCommandHandleRegistry& stream_handle_registry) {
    //1. create logic stream instance
    auto& execution_plan = plan_.execution_plan;
    for (size_t i = 0; i < num_logic_streams_; ++i) {
      execution_plan.emplace_back(std::make_unique<SequentialExecutionPlan::LogicStream>());
    }
    //2. for each node, if any of its consumer partitioned to another stream, generate a notification
    size_t num_notifications = 0;
    std::unordered_map<NodeIndex, NotificationIndex> node_to_notification;
    for (size_t i = 0; i < num_logic_streams_; ++i) {
      for (auto node_index : stream_nodes_[i]) {
        auto* node = graph_viewer_.GetNode(node_index);
        for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
          if (std::find(stream_nodes_[i].begin(), stream_nodes_[i].end(), it->Index()) == stream_nodes_[i].end()) {
            node_to_notification[node_index] = num_notifications++;
            break;
          }
        }
      }
    }
    //3. Check the nodes in each logical stream, set EP instance;
    for (size_t i = 0; i < num_logic_streams_; ++i) {
      std::set<const IExecutionProvider*> providers;
      for (auto node_index : stream_nodes_[i]) {
        auto* node = graph_viewer_.GetNode(node_index);
        onnxruntime::ProviderType exec_provider_name = node->GetExecutionProviderType();
        const IExecutionProvider* ep = execution_providers.Get(exec_provider_name);
        if (execution_plan[node_stream_map_[node_index]]->ep_) {
          ORT_ENFORCE(execution_plan[node_stream_map_[node_index]]->ep_ == ep);
        } else {
          execution_plan[node_stream_map_[node_index]]->ep_ = ep;
        }
      }
    }
    //4. set notification owners
    plan_.notification_owners.resize(num_notifications);
    for (auto node_index : graph_viewer_.GetNodesInTopologicalOrder()) {
      auto it = node_to_notification.find(node_index);
      if (it != node_to_notification.end()) {
        // notification owned by the node who produced it.
        plan_.notification_owners[it->second] = node_stream_map_[node_index];
      }
    }
    //5. add commands to logic queue
    for (size_t i = 0; i < num_logic_streams_; ++i) {
      for (size_t j = 0; j < stream_nodes_[i].size(); ++j) {
        auto node_index = stream_nodes_[i][j];
        if (j > 0) {
          // add dependency for current logic stream
          dependence_graph_[node_index].insert(stream_nodes_[i][j - 1]);
        }
        //auto cur_stream_idx = node_stream_map_[node_index];
        // check if any producer is not in current stream, if yes, create a wait
        auto* node = graph_viewer_.GetNode(node_index);
        for (auto it = node->InputNodesBegin(); it != node->InputNodesEnd(); ++it) {
          if (std::find(stream_nodes_[i].begin(), stream_nodes_[i].end(), it->Index()) == stream_nodes_[i].end()) {
            // find the notificaiton id
            auto notfication_it = node_to_notification.find(it->Index());
            ORT_ENFORCE(notfication_it != node_to_notification.end());
            NotificationIndex notification_index = notfication_it->second;
            // push a barrier
            size_t barrier_id = plan_.num_barriers++;
            plan_.downstream_map[notification_index].push_back({i,
                static_cast<int>(execution_plan[i]->steps_.size())});
            execution_plan[i]->steps_.emplace_back(std::make_unique<BarrierStep>(barrier_id));
#ifdef ENABLE_TRAINING
            execution_plan[i]->step_node_index.push_back(node_index);
#endif
            // push a wait command if has EP registered it.
            auto wait_handle = stream_handle_registry.GetWaitHandle(
                execution_plan[plan_.notification_owners[notfication_it->second]]->ep_->Type(),
                node->GetExecutionProviderType());
            if (wait_handle) {
              execution_plan[i]->steps_.emplace_back(std::make_unique<WaitOnEPStep>(wait_handle, notification_index));
#ifdef ENABLE_TRAINING
              execution_plan[i]->step_node_index.push_back(node_index);
#endif
            }
          }
        }
        for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
          // add dependency for model graph
          dependence_graph_[it->Index()].insert(node_index);
        }
        // push launch kernel command
        execution_plan[i]->steps_.emplace_back(std::make_unique<LaunchKernelStep>(node_index));
#ifdef ENABLE_TRAINING
        execution_plan[i]->step_node_index.push_back(node_index);
#endif
        // check if any notification generated by this node, if yes, push a activate
        auto notification_it = node_to_notification.find(node_index);
        if (notification_it != node_to_notification.end()) {
          NotificationIndex notification_index = notification_it->second;
          execution_plan[i]->steps_.emplace_back(std::make_unique<ActivateNotificationStep>(notification_index));
#ifdef ENABLE_TRAINING
          // calculate the min consmer;
          auto& order = graph_viewer_.GetNodesInTopologicalOrder();
          size_t distance = graph_viewer_.NumberOfNodes();
          for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
            auto order_it = std::find(order.begin(), order.end(), it->Index());
            size_t cur_distance = std::distance(order.begin(), order_it);
            distance = std::min(distance, cur_distance);
          }
          // set the notification step as the triggering part of next node.
          execution_plan[i]->step_node_index.push_back(order[distance]);
#endif
          // notify downstreams
          execution_plan[i]->steps_.emplace_back(std::make_unique<TriggerDownstreamStep>(notification_index));
#ifdef ENABLE_TRAINING
          // set the notification step as the triggering part of next node.
          execution_plan[i]->step_node_index.push_back(order[distance]);
#endif
        }
      }
    }

    for (auto node_index : graph_viewer_.GetNodesInTopologicalOrder()) {
      auto* node = graph_viewer_.GetNode(node_index);
      const auto& output_defs = node->OutputDefs();
      for (size_t output_idx_local = 0; output_idx_local < output_defs.size(); ++output_idx_local) {
        const auto& node_output = output_defs[output_idx_local];
        if (!node_output->Exists()) continue;
        OrtValueIndex output_idx_global;
        ORT_THROW_IF_ERROR(ort_value_name_idx_map_.GetIdx(node_output->Name(), output_idx_global));
        plan_.value_to_stream_map[output_idx_global] = node_stream_map_[node_index];
        value_node_map_[output_idx_global] = node_index;
      }
    }

    return Status::OK();
  }

  static bool IsNonTensor(const onnxruntime::NodeArg& nodearg) {
    // TODO: unclear why we should go through a string-representation of type
    auto ptype = nodearg.Type();
    auto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(ptype);
    return !utils::HasTensorType(type_proto);
  }

#if !defined(DISABLE_OPTIONAL_TYPE)
  static bool IsOptionalType(const onnxruntime::NodeArg& nodearg) {
    const auto* type_proto = nodearg.TypeAsProto();
    return type_proto->value_case() == ONNX_NAMESPACE::TypeProto::kOptionalType;
  }
#endif

  // For in-place reuse tensors, the lifetime is the union of all the tensors that tensors that use that buffer
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  void AdjustInplaceLifeIntervals() {
    std::unordered_map<OrtValueIndex, std::vector<OrtValueIndex>> inplace_reuse_buffer;
    for (size_t i = 0; i < ort_value_info_.size(); ++i) {
      if (AllocPlan(OrtValueIndex(i)).inplace_reuse != OrtValueIndex(i)) {
        inplace_reuse_buffer[ort_value_info_[i].inplace_reused_buffer_index].push_back(OrtValueIndex(i));
      }
    }
    for (const auto& item : inplace_reuse_buffer) {
      IntervalT& lifetime = AllocPlan(item.first).life_interval;
      for (const auto& value : item.second) {
        auto start = AllocPlan(value).life_interval.first;
        auto end = AllocPlan(value).life_interval.second;
        lifetime.first = lifetime.first < start ? lifetime.first : start;
        lifetime.second = lifetime.second > end ? lifetime.second : end;
      }
      for (const auto& value : item.second) {
        AllocPlan(value).life_interval = lifetime;
      }
    }
  }
#endif
};

Status PlannerImpl::CreatePlan(const ExecutionProviders& execution_providers,
                               const IStreamCommandHandleRegistry& stream_handle_registry,
                               /*const ProviderStreamMap& provider_stream_map,
                               const OpStreamMap& op_stream_map,*/
                               const std::string& partition_config_file,
                               const logging::Logger& logger) {

  auto& p_graph_nodes = graph_viewer_.GetNodesInTopologicalOrder(context_->GetExecutionOrder());

  //1. partition graph into streams
  PartitionIntoStreams(logger, partition_config_file);

  //2. initialize the plan based on stream partition result
  int num_ml_values = ort_value_name_idx_map_.MaxIdx() + 1;

  Initialize(p_graph_nodes.size(), static_cast<size_t>(num_ml_values));

  // compute value location
  ORT_RETURN_IF_ERROR(ComputeValueLocation());
  ORT_RETURN_IF_ERROR(ComputePlanForInputsAndWeights());

  // build execution plan
  ORT_RETURN_IF_ERROR(BuildExecutionPlan(execution_providers, stream_handle_registry));

  // build value_node_map
  for (auto node_index : graph_viewer_.GetNodesInTopologicalOrder()) {
    auto* node = graph_viewer_.GetNode(node_index);
    const auto& output_defs = node->OutputDefs();
    for (size_t output_idx_local = 0; output_idx_local < output_defs.size(); ++output_idx_local) {
      const auto& node_output = output_defs[output_idx_local];
      if (!node_output->Exists()) continue;
      OrtValueIndex output_idx_global;
      ORT_THROW_IF_ERROR(ort_value_name_idx_map_.GetIdx(node_output->Name(), output_idx_global));
      value_node_map_[output_idx_global] = node_index;
    }
  }

  // determine sharing/reuse among ml-values
  ORT_RETURN_IF_ERROR(ComputeReusePlan());

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  // Adjust the allocate and lifetime intervals for all ml-values, based on their allocation kind.
  AdjustInplaceLifeIntervals();
#endif

#ifdef ENABLE_TRAINING
  // Determine allocation order for weights and activations. This needs to be done after ComputeReusePlan.
  ORT_RETURN_IF_ERROR(ComputeAllocationOrder());
#endif

  // convert information in the freelist_ into a deallocation plan in required format
  ORT_RETURN_IF_ERROR(GenerateDeallocationPlan());

  // Ensure Memory-Time schedule is valid. This should be called at the end because memory start/end timestamps
  // are updated until GenerateDeallocationPlan is finished.
  // TODO: enable verification
  // VerifyMemoryTimeSchedule();

  return Status::OK();
}

Status SequentialPlanner::CreatePlan(
    const Node* parent_node,
    const onnxruntime::GraphViewer& graph_viewer,
    gsl::span<const NodeArg* const> outer_scope_node_args,
    const ExecutionProviders& providers,
    const KernelCreateInfoMap& kernel_create_info_map,
    const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps,
    const InlinedHashMap<OrtValueName, OrtMemoryInfo>& outer_scope_node_arg_to_location_map,
    const OrtValueNameIdxMap& ort_value_name_idx_map,
    const ISequentialPlannerContext& context,
    const ExecutionProviders& execution_providers,
    const IStreamCommandHandleRegistry& stream_handle_registry,
    const std::string& partition_config_file,
    const logging::Logger& logger,
    std::optional<SequentialExecutionPlan>& plan) {
  // allocate/reset here so we know it's clean
  plan.emplace();

  PlannerImpl planner(parent_node, graph_viewer, outer_scope_node_args, providers,
                      kernel_create_info_map, subgraphs_kernel_create_info_maps,
                      outer_scope_node_arg_to_location_map,
                      ort_value_name_idx_map, context, *plan);

  return planner.CreatePlan(execution_providers,
                            stream_handle_registry,
                            /*provider_stream_map,
                            op_stream_map,*/
                            partition_config_file,
                            logger);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

std::unordered_map<std::string, INodePartitioner::NodePartitionerType> INodePartitioner::name_type_map = {{std::string{"DummyPartition"}, NodePartitionerType::DummyPartition}};

//INodePartitioner::INodePartitioner(const std::string& configuration_file, const logging::Logger& logger) : configuration_file_(configuration_file, logger) {}

class DummyPartitioner : public INodePartitioner {
 public:
  DummyPartitioner(const logging::Logger& logger, const std::string& configuration_file) : INodePartitioner(logger, configuration_file) {
    Initialize();
  }
  ~DummyPartitioner() {
    if (need_dump_) {
      DumpPartition();
    }
  }
  void DumpPartition() const;
  void PartitionNodes(const onnxruntime::GraphViewer& graph_viewer, std::vector<std::vector<NodeIndex>>& stream_nodes) override;
  virtual const std::string& Name() const override {
    return name;
  }

 private:
  void Initialize();
  int num_streams_{};
  std::map<std::string, int> max_streams_;
  std::vector<std::vector<std::string>> node_names_by_stream_;
  bool need_dump_ = false;
  static const std::string name;
};

const std::string DummyPartitioner::name = "DummyPartition";

/*
Format of the configuration file for dummpy partition:
line 1: DummyPartition                           # name of the partitioner
line 2: ExecutionProviders:2                     # number of execution providers
line 3: CpuExecutionProvider:2                   # number of streams of the 1st ep
line 4: GpuExecutionProvider:2                   # number of streams of the 2nd ep
line 5: node_name,node_name,node_name ...        # list of nodes on 1st stream of the 1st ep
line 6: node_name,node_name,node_name ...        # list of nodes on 2nd stream of the 1st ep
line 7: node_name,node_name,node_name ...        # list of nodes on 1st stream of the 2nd ep
line 8: node_name,node_name,node_name ...        # list of nodes on 2nd stream of the 2nd ep
*/

#define EXIT_ON_ERR(err)\
      status_ = ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, err);\
      if_stream.close();\
      return;

void DummyPartitioner::Initialize() {
  if (configuration_file_.empty()) {
    return;
  }
  std::ifstream if_stream(configuration_file_);
  if (if_stream.is_open()) {
    std::string line;
    if (!std::getline(if_stream, line) || line != Name()) {
      EXIT_ON_ERR("configuration file should start with a line of partition name");
    }
    if (std::getline(if_stream, line)) {
      auto columns = INodePartitioner::Split(line,':');
      if (columns.size() != 2 || columns[0] != "ExecutionProviders") {
        EXIT_ON_ERR("2nd line of configuration file should be of format: ExecutionProviders,<an integer>");
      }
      int eps = atoi(columns[1].c_str());
      if (eps <= 0) {
        EXIT_ON_ERR("2nd line, the number of ExecutionProviders must be a positive value");
      }
      for (int i = 0; i < eps; ++i) {
        if (std::getline(if_stream, line)) {
          columns = INodePartitioner::Split(line,':');
          if (columns.size() != 2) {
            EXIT_ON_ERR("invalid configuration - failed to read execution provider stream setting")
          }
        } else {
          EXIT_ON_ERR("invalid configuration - failed to read execution provider stream setting");
        }
        auto num_current_stream = atoi(columns[1].c_str());
        max_streams_[columns[0]] = num_current_stream;  //TODO: handle the case when columns[1] has non alpha char
        num_streams_ += num_current_stream;
      }
      while (getline(if_stream, line)) {
        node_names_by_stream_.push_back(INodePartitioner::Split(line,','));
        if (node_names_by_stream_.back().empty()) {
          EXIT_ON_ERR("invalid configuration - the line of node names is empty");
        }
      }
      if (node_names_by_stream_.size() != (size_t)num_streams_) {
        EXIT_ON_ERR("invalid configuration - the total number of line of streams mismatch with the sum of execution provider stream setting");
      }
    } else {
      need_dump_ = true;
    }
    if_stream.close();
  }
}

void DummyPartitioner::DumpPartition() const {
  if (configuration_file_.empty()) {
    return;
  }
  std::ofstream of_stream(configuration_file_, std::ios_base::out | std::ios_base::trunc);
  if (of_stream.is_open()) {
    of_stream << Name() << std::endl;
    of_stream << "ExecutionProviders:" << max_streams_.size() << std::endl;
    for (const auto& kv : max_streams_) {
      of_stream << kv.first << ":" << kv.second << std::endl;
    }
    for (const auto& nodes : node_names_by_stream_) {
      std::copy(nodes.begin(), nodes.end() - 1, std::ostream_iterator<std::string>(of_stream, ","));
      if (!nodes.empty()) {
        of_stream << nodes.back() << std::endl;
      }
    }
    of_stream.close();
  } else {
    LOGS(logger_, WARNING) << "DummyPartitioner failed to dump configuration to file: " << configuration_file_;
  }
}

void DummyPartitioner::PartitionNodes(const onnxruntime::GraphViewer& graph_viewer, std::vector<std::vector<NodeIndex>>& stream_nodes) {
  if (!status_.IsOK()) {
    return;  // input configuration has errors, do nothing
  }

  std::unordered_map<std::string, int> op_type_counter;
  auto& p_graph_nodes = graph_viewer.GetNodesInTopologicalOrder();

  if (max_streams_.empty() && node_names_by_stream_.empty()) { // input configure empty, do it from scratch
    // partition by ep, each has one stream
    std::unordered_map<std::string, int> ep_to_stream;
    for (auto node_index : p_graph_nodes) {
      const auto* node = graph_viewer.GetNode(node_index);
      const auto& op_type = node->OpType();
      const auto& node_name = node->Name();
      onnxruntime::ProviderType exec_provider_name = node->GetExecutionProviderType();
      if (max_streams_.find(exec_provider_name) == max_streams_.end()) {
        max_streams_[exec_provider_name] = 1;
      }
      auto it = ep_to_stream.find(exec_provider_name);
      if (it == ep_to_stream.end()) {
        ep_to_stream[exec_provider_name] = static_cast<int>(node_names_by_stream_.size());
        node_names_by_stream_.push_back({});
        it = ep_to_stream.find(exec_provider_name);
      }
      if (node_name.empty()) {
        node_names_by_stream_[it->second].push_back(op_type + std::to_string(op_type_counter[op_type]++));
      } else {
        node_names_by_stream_[it->second].push_back(node_name);
      }
    }
  }
  std::unordered_map<std::string, size_t> node_stream_map;
  for (size_t i = 0; i < node_names_by_stream_.size(); ++i) {
    for (const auto& node_name : node_names_by_stream_[i]) {
      node_stream_map[node_name] = i;
    }
  }
  op_type_counter.clear();
  stream_nodes.clear();
  stream_nodes.resize(node_names_by_stream_.size());
  for (auto node_index : p_graph_nodes) {
    const auto* node = graph_viewer.GetNode(node_index);
    const auto& op_type = node->OpType();
    const auto& node_name = node->Name();
    if (node_name.empty()) {
      auto tmp_name = op_type + std::to_string(op_type_counter[op_type]++);
      ORT_ENFORCE(node_stream_map.find(tmp_name) != node_stream_map.end());
      stream_nodes[node_stream_map[tmp_name]].push_back(node_index);
    } else {
      stream_nodes[node_stream_map[node_name]].push_back(node_index);
    }
  }
}

std::vector<std::string> INodePartitioner::Split(const std::string& line, char splitor) {
  std::vector<std::string> columns;
  std::string column;
  std::stringstream ss;
  ss << line;
  while (getline(ss, column, splitor)) {
    columns.push_back(column);
  }
  return columns;
}

std::unique_ptr<INodePartitioner> INodePartitioner::CreateNodePartitioner(const logging::Logger& logger, const std::string& configuration_file) {
  std::string cfg_file = configuration_file;
  INodePartitioner::NodePartitionerType partitioner_type = INodePartitioner::NodePartitionerType::DummyPartition;
  if (!cfg_file.empty()) {
    std::ifstream if_stream(cfg_file);
    if (if_stream.is_open()) {
      std::string partitioner_name;
      std::getline(if_stream, partitioner_name);
      if_stream.close();
      auto iter = name_type_map.find(partitioner_name);
      ORT_ENFORCE(iter != name_type_map.end(), "invalid node partitioner name");
      partitioner_type = iter->second;
    } else { // create and initialize the configure file if not already there
      std::ofstream of_stream(cfg_file, std::ios_base::out | std::ios_base::trunc);
      ORT_ENFORCE(of_stream.is_open(), "cannnot write configuration to", cfg_file.c_str());
      of_stream << "DummyPartition" << std::endl;
      of_stream.close();
    }
  }//else means configuration will not be written to a file
  std::unique_ptr<INodePartitioner> node_partitioner;
  switch (partitioner_type) {
    case INodePartitioner::NodePartitionerType::DummyPartition:
      node_partitioner.reset(new DummyPartitioner(logger, cfg_file));
      break;
    default:
      break;
  }
  return node_partitioner;
}

}  // namespace onnxruntime
