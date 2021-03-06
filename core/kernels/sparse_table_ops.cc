// Copyright (c) 2020, Qihoo, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "core/utility/semaphore.h"

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/refcount.h"

#include "core/kernels/resource_var_wrapper.h"

#include <brpc/controller.h>
#include <sstream>

#include "core/ps/ps_server_interface.h"
#include "core/ps/ps_cluster.h"

using namespace tensornet;

namespace tensorflow {

template <typename T, bool use_dynamic_cast>
Status LookupResource(OpKernelContext* ctx, const ResourceHandle& p, T** value);

const ResourceHandle& HandleFromInput(OpKernelContext* ctx, int input);

class SparsePullCall {
public:
    SparsePullCall(int table_handle, int shard_id, int dim)
        : shard_id_(shard_id) {
        req.set_table_handle(table_handle);
        req.set_dim(dim);
    }

    ~SparsePullCall() {}

    void AddRequestSign(uint64 sign) {
        req.add_signs(sign);
    }

    void Start(const tensornet::Callback& done) {
        const PsServerInterface* si =
            PsCluster::Instance()->GetServer(shard_id_);
        si->SparsePullAsync(&cntl, &req, &resp, done);
    }

public:
    brpc::Controller cntl;
    SparsePullRequest req;
    SparsePullResponse resp;

private:
    int shard_id_ = -1;
};

class SparsePushCall {
public:
    SparsePushCall(int table_handle, int shard_id, int dim)
        : shard_id_(shard_id) {
        req.set_table_handle(table_handle);
        req.set_dim(dim);
    }

    ~SparsePushCall() {}

    void AddRequestGrad(uint64 sign, const float* grad_vec, int dim, int show, uint32 version) {
        VariableWeight* weight = req.add_weight();
        weight->set_sign(sign);
        weight->set_show(show);
        weight->set_version(version);

        for (int i = 0; i < dim; i++) {
            weight->add_w(grad_vec[i]);
        }
    }

    void Start(const tensornet::Callback& done) {
        const PsServerInterface* si =
            PsCluster::Instance()->GetServer(shard_id_);
        si->SparsePushAsync(&cntl, &req, &resp, done);
    }

public:
    brpc::Controller cntl;
    SparsePushRequest req;
    SparsePushResponse resp;

private:
    int shard_id_ = -1;
};

struct SparsePullVarInfo {
public:
    SparsePullVarInfo(tensorflow::Var* t_var, const tensorflow::Tensor* value)
        : var(t_var)
        , sign_value(value) {
        const int64* feasign_vec = value->flat<int64>().data();

        for (int i = 0; i < value->NumElements(); ++i) {
            const uint64 sign = (uint64)feasign_vec[i];
            int sign_id = 0;
            if (sign_id_mapping.count(sign)) {
                sign_id = sign_id_mapping[sign];
            } else {
                sign_id = sign_id_mapping.size();
                sign_id_mapping[sign] = sign_id;
            }
        }

        const Tensor* var_tensor = var->tensor();

        const uint64 max_var_count = var_tensor->shape().dim_size(0);
        CHECK_LT(sign_id_mapping.size(), max_var_count);
    }

    int VarDim() const {
        const Tensor* var_tensor = var->tensor();
        return var_tensor->shape().dim_size(1);
    }

public:
    // sparse feature column embedding variable.
    // shape: [max_var_count, emb_dim]
    tensorflow::Var* var;

    // sign list of feature column
    // shape: [sign_count]
    // its store feature signs of one feature_column in one batch
    const tensorflow::Tensor* sign_value;

    // in order to use embedding_lookup, we must map the feature sign into index
    // which will not exceed *var* first dimension(max_var_count)
    // the mapping id begin from 0, depend on signs order in sign_value 
    std::map<uint64, int> sign_id_mapping;

    int dim;
};

class SparseTablePullKernel : public AsyncOpKernel {
public:
    explicit SparseTablePullKernel(OpKernelConstruction* c)
        : AsyncOpKernel(c) {
        OP_REQUIRES_OK(c, c->GetAttr("table_handle", &table_handle_));
        OP_REQUIRES_OK(c, c->GetAttr("N", &N_));
    }

    void ComputeAsync(OpKernelContext* c, DoneCallback done) override {
        OP_REQUIRES_ASYNC(c, c->num_inputs() == N_ * 2,
                          errors::InvalidArgument("SparseTable pull num_inputs:",
                                                  c->num_inputs(),
                                                  " not equal:", N_ * 2),
                          done);

        std::vector<SparsePullVarInfo> var_infos;

        for (int i = 0; i < N_; i++) {
            const ResourceHandle& handle = HandleFromInput(c, i);

            Var* variable = nullptr;
            const auto status = LookupResource<Var, false>(c, handle, &variable);

            OP_REQUIRES_OK_ASYNC(c, status, done);
            CHECK(variable);

            const Tensor* var_tensor = variable->tensor();

            OP_REQUIRES_ASYNC(
                c, TensorShapeUtils::IsMatrix(var_tensor->shape()),
                errors::InvalidArgument(
                    "sparse pull variable must Matrix(sign_id_cnt, dim), saw: ",
                    var_tensor->shape().DebugString()),
                done);

            SparsePullVarInfo var_info(variable, &c->input(N_ + i));

            var_infos.emplace_back(var_info);
        }

        CHECK_GT(var_infos.size(), 0);

        int dim = var_infos[0].VarDim();

        for (size_t i = 0; i < var_infos.size(); i++) {
            CHECK_EQ(dim, var_infos[i].VarDim());
        }

        PsCluster* cluster = PsCluster::Instance();
        OP_REQUIRES_ASYNC(
            c, true == cluster->IsInitialized(),
            errors::InvalidArgument("cluster instance not initialized:"), done);

        std::vector<SparsePullCall*> calls;

        for (size_t shard_id = 0; shard_id < cluster->RankNum(); shard_id++) {
            calls.emplace_back(
                new SparsePullCall(table_handle_, shard_id, dim));
        }

        std::map<uint64, std::vector<int>> sign_varid;
        std::map<uint64, uint32> sign_version;

        for (size_t i = 0; i < var_infos.size(); i++) {
            for (const auto& sign_iter : var_infos[i].sign_id_mapping) {
                uint64 sign = sign_iter.first;

                // if already exist, skip
                if (sign_varid.count(sign) == 0) {
                    int shard_id = sign % cluster->RankNum();
                    calls[shard_id]->AddRequestSign(sign);
                }

                sign_varid[sign].push_back(i);
                sign_version[sign] = 0;
            }
        }

        Semaphore semaphore(calls.size());

        for (auto& call : calls) {
            call->Start([this, call, &var_infos, &sign_varid, &sign_version, &semaphore]() {
                auto status = PopulatePulledVariable_(var_infos, sign_varid, call->resp, sign_version);
                if (!status.ok()) {
                    LOG(INFO) << "populate variable fail:" << status.ToString();
                }
                semaphore.Notify();
                delete call;
            });
        }

        semaphore.WaitForSemaphore();

        // allocate version output
        for (int i = 0; i < N_; i++) {
            Tensor* out_tensor = nullptr;
            Tensor* version_tensor = nullptr;

            const Tensor* value = var_infos[i].sign_value;
            OP_REQUIRES_OK_ASYNC(c, c->allocate_output(i, value->shape(), &out_tensor), done);
            OP_REQUIRES_OK_ASYNC(c, c->allocate_output(N_ + i, value->shape(), &version_tensor), done);

            const int64* feasign_vec = value->flat<int64>().data();
            int64* out = out_tensor->flat<int64>().data();
            int32* version_out = version_tensor->flat<int32>().data();

            for (int j = 0; j < value->NumElements(); ++j) {
                const uint64 sign = (uint64)feasign_vec[j];
                out[j] = var_infos[i].sign_id_mapping[sign];
                CHECK_GT(sign_version.count(sign), 0) << "sign:" << sign;
                version_out[j] = sign_version[sign];
            }
        }

        done();

        return;
    }

private:
    Status PopulatePulledVariable_(const std::vector<SparsePullVarInfo>& var_infos,
                                   const std::map<uint64, std::vector<int>>& sign_varid,
                                   const SparsePullResponse& resp,
                                   std::map<uint64, uint32>& sign_version) {
        for (int i = 0; i < resp.weight_size(); i++) {
            const auto& weight = resp.weight(i);
            uint64 sign = weight.sign();

            auto varid_iter = sign_varid.find(sign);
            CHECK(varid_iter != sign_varid.end());

            for (auto& var_index : varid_iter->second) {
                auto& var_info = var_infos[var_index];

                auto sign_id_iter = var_info.sign_id_mapping.find(sign);
                CHECK(sign_id_iter != var_info.sign_id_mapping.end());

                int sign_id = sign_id_iter->second;

                mutex_lock ml(*var_info.var->mu());
                Tensor* var_tensor = var_info.var->tensor();

                CHECK_EQ(resp.dim(), var_info.VarDim());

                auto w_matrix = var_tensor->matrix<float>();
                for (int j = 0; j < weight.w_size(); j++) {
                    w_matrix(sign_id, j) = weight.w(j);
                }

                // add version
                sign_version[sign] = weight.version();
            }
        }

        return Status::OK();
    }

private:
    int table_handle_;
    int N_;
};

REGISTER_KERNEL_BUILDER(Name("SparseTablePull").Device(DEVICE_CPU),
                        SparseTablePullKernel);

struct SparsePushVarInfo {
public:
    SparsePushVarInfo(const Tensor* t_value, const Tensor* t_grad, const Tensor* t_version)
        : value(t_value)
        , grad(t_grad)
        , version(t_version) {
        CHECK(value->NumElements() == version->NumElements());

        const int64* feasign_vec = value->flat<int64>().data();
        const int* version_vec = version->flat<int>().data();

        std::map<uint64, int> sign_id_mapping;
        for (int i = 0; i < value->NumElements(); ++i) {
            const uint64 sign = (uint64)feasign_vec[i];
            if (0 == sign_id_mapping.count(sign)) {
                int sign_id = sign_id_mapping.size();
                sign_id_mapping[sign] = sign_id;
            }
            if (0 == sign_count_mapping.count(sign)) {
                sign_count_mapping[sign] = 1;
            } else {
                sign_count_mapping[sign]++;
            }
            sign_version_mapping[sign] = (uint32)version_vec[i];
        }
        for (auto it : sign_id_mapping) {
            id_sign_mapping[it.second] = it.first;
        }

        // const int sign_id_cnt = grad->shape().dim_size(0);

        // TODO, BUG, tensorflow will use a dummy (default is 0) grad when feature sign is
        // empty, maybe we could solve this by custom dataset
        // CHECK_EQ((int)id_sign_mapping.size(), sign_id_cnt);
    }

    int SignIdCount() const {
        return (int)id_sign_mapping.size();
    }

    int GradDim() const {
        return grad->shape().dim_size(1);
    }

    int ShowNum(int64 sign) {
        return sign_count_mapping[sign];
    }

public:
    const Tensor* value;
    const Tensor* grad;
    const Tensor* version;

    std::map<int, int64> id_sign_mapping;
    std::map<int64, uint32> sign_version_mapping;
    std::map<int64, int> sign_count_mapping;
};

class SparseTablePushKernel : public AsyncOpKernel {
public:
    explicit SparseTablePushKernel(OpKernelConstruction* c)
        : AsyncOpKernel(c) {
        OP_REQUIRES_OK(c, c->GetAttr("table_handle", &table_handle_));
        OP_REQUIRES_OK(c, c->GetAttr("N", &N_));
    }

    void ComputeAsync(OpKernelContext* c, DoneCallback done) override {
        OP_REQUIRES_ASYNC(c, c->num_inputs() == N_ * 3,
                          errors::InvalidArgument("SparseTable push num_inputs:",
                                                  c->num_inputs(),
                                                  " not equal:", N_ * 3),
                          done);
        std::vector<SparsePushVarInfo> var_infos;

        for (int i = 0; i < N_; i++) {
            const Tensor* value = &c->input(i);
            const Tensor* grad = &c->input(N_ + i);
            const Tensor* version = &c->input(2 * N_ + i);

            OP_REQUIRES_ASYNC(
                c, TensorShapeUtils::IsMatrix(grad->shape()),
                errors::InvalidArgument(
                    "sparse push grad must Matrix(sign_id_cnt, dim), saw: ",
                    grad->shape().DebugString()),
                done);

            SparsePushVarInfo var_info(value, grad, version);

            var_infos.emplace_back(var_info);
        }

        CHECK_GT(var_infos.size(), 0);

        int dim = var_infos[0].GradDim();
        for (size_t i = 0; i < var_infos.size(); i++) {
            CHECK_EQ(dim, var_infos[i].GradDim());
        }

        std::vector<SparsePushCall*> calls;
        PsCluster* cluster = PsCluster::Instance();

        for (size_t shard_id = 0; shard_id < cluster->RankNum(); shard_id++) {
            calls.emplace_back(
                new SparsePushCall(table_handle_, shard_id, dim));
        }

        for (size_t i = 0; i < var_infos.size(); i++) {
            // NOTE, tensorfow use RowMajor layout
            const float* grad_matrix = var_infos[i].grad->matrix<float>().data();

            for (int sign_id = 0; sign_id < var_infos[i].SignIdCount(); sign_id++) {
                auto iter = var_infos[i].id_sign_mapping.find(sign_id);
                CHECK(iter != var_infos[i].id_sign_mapping.end());

                const uint64 sign = iter->second;

                CHECK_GT(var_infos[i].sign_version_mapping.count(sign), 0);
                const uint32 version = var_infos[i].sign_version_mapping[sign];

                int shard_id = sign % cluster->RankNum();
                const float* grad = grad_matrix + dim * sign_id;
                calls[shard_id]->AddRequestGrad(sign, grad, dim, var_infos[i].ShowNum(sign), version);
            }
        }

        Semaphore semaphore(calls.size());
        for (auto& call : calls) {
            call->Start([this, c, call, &semaphore]() {
                semaphore.Notify();
                delete call;
            });
        }

        semaphore.WaitForSemaphore();
        done();
    }

private:
    int table_handle_;
    int N_;
};

REGISTER_KERNEL_BUILDER(Name("SparseTablePush").Device(DEVICE_CPU),
                        SparseTablePushKernel);

}  // namespace tensorflow
